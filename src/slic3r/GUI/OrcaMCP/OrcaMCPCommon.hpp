// src/slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp
#pragma once
#include <future>
#include <functional>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r {
class ModelObject;
namespace GUI {
class Plater;
namespace OrcaMCP {

// Runs `func` on the wx main thread and blocks the calling HTTP worker until it returns.
// `func` must return nlohmann::json. Exceptions propagate to the caller.
template<typename Func>
nlohmann::json run_on_main_thread(Func&& func)
{
    std::promise<nlohmann::json> promise;
    auto future = promise.get_future();
    wxGetApp().CallAfter([&promise, func = std::forward<Func>(func)]() {
        try {
            promise.set_value(func());
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    return future.get();
}

// MCP clients do not all deliver scalars the same way: a client whose cached tool schema predates a
// new parameter tends to send it as a string ("1", "true"), and some send every number as a float
// (1.0). These accept any JSON value that is exactly the wanted type, and reject everything else, so
// a stale client can still reach a new parameter. Both return false without touching `out`.
bool parse_integer_param(const nlohmann::json& value, int& out);
bool parse_boolean_param(const nlohmann::json& value, bool& out);

// The same contract for a real-valued parameter, with two rejections a bare get<double>() does not
// make. A non-finite value is refused on both branches: JSON's grammar has no NaN or Infinity token,
// but std::stod parses "nan" and "inf" as fully-consumed valid numbers, and a NaN that reaches a
// coordinate defeats every range check written as a comparison (each one is false against NaN) and
// ends in an undefined scaled() conversion. A hexadecimal spelling is refused for the same class of
// reason: std::stod reads "0x10" as 16 and "0x1p4" as 16 too, and a coordinate nobody meant to write
// in base 16 is a caller mistake worth reporting rather than silently giving a different number.
bool parse_double_param(const nlohmann::json& value, double& out);

// True for "#RRGGBB" -- and, with allow_alpha, also "#RRGGBBAA". Upstream's parsers are lenient in
// ways that turn a typo into a wrong colour rather than an error: can_decode_color only checks the
// length and the '#' (so "#GGGGGG" decodes as black) and color_decompose_hex_to_rgb accepts any
// trailing garbage after six digits. Every MCP entry point that takes a colour from a caller
// validates it here first, so the caller is told instead of quietly getting a different colour.
bool is_hex_color(const std::string& value, bool allow_alpha = false);

// True when `after` is a different colour from `before`. Two hex colours are compared
// case-insensitively, because "#ff0000" and "#FF0000" are the same colour and calling that a change
// is not free: apply_config gives every slot whose colour moved the colour picker's three-key
// treatment, which flattens a gradient. Anything that is not a hex colour -- an empty slot, a name
// this code cannot interpret -- is compared exactly, since nothing here can say what it means.
bool color_changed(const std::string& before, const std::string& after);

// Pure geometry: true when `object_bbox` sits inside `plate_box` in X and Y, and is not sunk more
// than `z_tolerance` below the bed. Z is only checked downwards: an object taller than the plate's
// box is a height problem the slicer reports itself, not a placement one.
bool object_within_plate(const BoundingBoxf3& object_bbox, const BoundingBoxf3& plate_box, double z_tolerance = 0.1);

// The plate bookkeeping an instance transform owes, and the answer a caller needs afterwards.
// It lives here, once, because move/rotate/scale/mirror/transform_objects all owe exactly the same
// and a copy per tool is how they drift apart (move_object had neither half; the others had the
// second half wrong).
//
// Sets on `result`: "plate_index" (the plate the object is on afterwards, null when it is on none),
// "on_bed", and "placement_warning" when it is not. Measuring against the *selected* plate, which is
// what these tools used to do, calls a correct cross-plate move "outside printable area".
//
// Every instance is notified, not just the first: a multi-instance object can have its instances on
// different plates, and a plate that keeps an instance it no longer holds slices the wrong thing.
//
// The third argument to notify_instance_update is `is_new`, and it must stay true here. With
// is_new=false the re-homing branch that adds an instance to a spiral-mode plate opens
// show_spiral_mode_settings_dialog (PartPlate.cpp, the add_instance branch) -- a MessageDialog, and a
// modal opened inside run_on_main_thread blocks the GUI thread forever, so the MCP call never
// returns. With is_new=true that branch applies the vase-mode object config directly instead, which
// is the same outcome the dialog produces: under is_object_config the dialog is OK-only and its
// answer is forced to wxID_YES regardless (ConfigManipulation::show_spiral_mode_settings_dialog).
// clone_object already passes true for the same reason.
// Report which plate holds the object and whether it sits inside that plate's printable area,
// writing plate_index / on_bed / placement_warning into `result`. Read-only: use this from query
// tools. A tool that has just moved geometry wants rehome_and_report_placement instead.
void report_placement(nlohmann::json& result, int object_id);

void rehome_and_report_placement(nlohmann::json& result, int object_id);

// Applies `world_transform` -- a rotation, a scale or a mirror written in *plate* axes -- to every
// instance of `object`, each about its own world bounding-box centre, and invalidates the object's
// cached bounding boxes.
//
// This is the instrument the transform tools owe their callers. The ModelObject::rotate/scale/mirror
// family loops over `this->volumes` and transforms the *mesh*, which sits beneath the instance
// transform: on an instance already rotated 90 degrees about X, a request phrased in plate axes
// comes out along a different world axis entirely, and the instance's own rotation/scale -- which is
// what every response and get_object_info report -- never changes at all. Transforming the instance
// is also what the GUI's gizmos do (Selection::transform_instance_relative composes exactly this
// T(pivot) * world_transform * T(-pivot) * instance_matrix), and it leaves the shared mesh alone,
// which matters because painting, the 3MF and every facet index are written against that mesh.
//
// The pivot is per instance, so a multi-instance object turns each copy in place rather than
// swinging the constellation about a shared centre -- the same thing the GUI's
// synchronize_unselected_instances does.
void transform_instances_in_plate_frame(ModelObject& object, const Transform3d& world_transform);

// Always returns {"count": N, "warnings": [{level, message, type}...]}.
nlohmann::json get_active_warnings_json(Plater* plater);

void add_turntable_preview_if_requested(nlohmann::json& result, bool include_preview, int view_count = 4, int resolution = 256);

// RAII: suppress modal dialogs for the lifetime of the guard and collect their messages.
// Nest-safe: an inner guard keeps the outer guard's messages and restores its state.
// answer_prompt chooses the answer for one keyed prompt (MsgDialog::set_mcp_prompt_key) until the
// outermost guard ends, so no later call inherits it.
struct McpDialogSuppressionGuard
{
    McpDialogSuppressionGuard() : m_was_enabled(is_mcp_dialog_suppression_enabled())
    {
        if (!m_was_enabled) {
            clear_mcp_suppressed_messages();
            clear_mcp_prompt_answers();
        }
        set_mcp_dialog_suppression(true);
    }
    ~McpDialogSuppressionGuard()
    {
        if (!m_was_enabled)
            clear_mcp_prompt_answers();
        set_mcp_dialog_suppression(m_was_enabled);
    }
    std::vector<std::string> messages() const { return get_mcp_suppressed_messages(); }
    void answer_prompt(const std::string& key, int answer_id) { set_mcp_prompt_answer(key, answer_id); }

private:
    bool m_was_enabled;
};

}}} // namespace Slic3r::GUI::OrcaMCP
