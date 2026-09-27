// src/slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp
#pragma once
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "OrcaMCPMainThreadGate.hpp"
#include "OrcaMCPSliceProgress.hpp"

namespace Slic3r {
class Model;
class ModelObject;
namespace GUI {
class PartPlate;
class PartPlateList;
class Plater;
namespace OrcaMCP {
struct MeshHealth;

// Runs `func` on the wx main thread and blocks the calling HTTP worker until it returns.
// `func` must return nlohmann::json. Exceptions propagate to the caller. Once the app has begun to
// quit this throws McpShuttingDown instead, without running `func` (call_through, QueuedCalls::run).
template<typename Func>
nlohmann::json run_on_main_thread(Func&& func)
{
    return call_through(main_thread_gate(), McpWork(std::forward<Func>(func)));
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

// The same contract for an object-valued parameter: a JSON object, or a string holding the JSON text
// of one -- what a client sends when its cached schema still types the parameter as a string (a
// parameter that grew an object form, like render_plate_view's layer_view). Anything else, including
// a string that is not an object's text, returns false without touching `out`.
bool parse_object_param(const nlohmann::json& value, nlohmann::json& out);

// A `settings` parameter: a list of {key, value} objects (each with a string type too when
// `with_type`, as apply_config takes them), or a string holding the JSON text of one, for the same
// reason as above. Returns false with `error` saying what is wrong -- not a list, or which item
// lacks what -- without touching `out`. What set_object_config, set_object_layer_range and
// apply_config read their settings through: a caller that sent an object got the JSON library's
// type_error instead.
bool parse_settings_param(const nlohmann::json& value, nlohmann::json& out, std::string& error, bool with_type = false);

// {"status": "error", "message": message}: what a tool returns for a call it refuses.
nlohmann::json error_response(const std::string& message);

// The object params["object_id"] names in `model`, with its index in `object_id`, or nullptr and
// `error` saying why: missing, not a whole number, or out of range. Every tool that takes an
// object_id reads it here, so all of them refuse a bad one with the same words.
ModelObject* resolve_object_id(const nlohmann::json& params, Model& model, int& object_id, std::string& error);

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

//
// `moved` false (the call turned out to change nothing) re-homes nothing: notify_instance_update
// marks the instance's plate not sliced even when the instance stays where it was, so a no-op call
// would throw away the plate's slice. Only the report is written then, and `changed` says which.
void rehome_and_report_placement(nlohmann::json& result, int object_id, bool moved);

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

// Whether an instance whose lowest point was at `min_z_before` and is at `min_z_after` once a
// transform is done goes back onto Z = 0. The GUI's own rule, from GLCanvas3D::do_scale, do_rotate
// and do_mirror: an instance that was sinking stays sinking unless the transform lifted it clear of
// the bed; any other lands on the bed.
bool should_drop_to_bed(double min_z_before, double min_z_after);

// transform_instances_in_plate_frame, followed by the drop to the bed the GUI does after a scale,
// rotate or mirror: each instance's lowest point is recorded first, and should_drop_to_bed decides
// per instance afterwards. Instances with auto_drop off are left where the transform put them, as
// the GUI leaves them. A pivot at the bounding-box centre is what moves the lowest point at all: a
// uniform 1.49x scale about the centre of a 99 mm figurine put its feet 24 mm under the bed.
void transform_instances_on_bed(ModelObject& object, const Transform3d& world_transform);

// Why flatten_object cannot orient object `object_id` now, or nothing. It orients through the orient
// job's selection path (the toolbar's Orient), which leaves out an object marked not printable
// (ModelObject::printable) and every instance on a locked plate -- and, left with an empty selection,
// orients every object instead -- and which Plater::orient does not start while another job runs.
std::optional<std::string> flatten_refusal(int object_id, bool printable, size_t instances, size_t instances_on_locked_plates,
                                           bool job_running);

// Why flatten_object must not start the orient job on the selection it made: `selected` (object index ->
// its selected instances, Selection::get_content) is not exactly the `instances` instances of object
// `object_id`. A 3D view whose reload is postponed has no volumes for an object it has not caught up
// with, and an empty selection makes the job orient every object instead.
std::optional<std::string> flatten_selection_refusal(int object_id, size_t instances, const std::map<int, std::set<int>>& selected);

// A scale the transform tools accept: every factor positive and finite. A zero factor makes the
// instance matrix singular, and a negative one is a mirror under a scale's name (mirror_object says so).
bool valid_scale_factors(const Vec3d& factors);

// The x, y and z a transform was given, each optional: move_object's position, rotate_object's degrees,
// scale_object's factors, and the same inside a transform_objects entry.
struct PlateAxes
{
    std::array<std::optional<double>, 3> axis;
    Vec3d value_or(const Vec3d& fallback) const;
};
// Reads the x, y and z of `object` into `out` (only those keys), or says what is wrong: an axis given as
// anything but a number, named `prefix` + the axis ("position.x"). A JSON value of the wrong type
// read with nlohmann's get or value throws, which in transform_objects stopped a batch half applied.
std::optional<std::string> read_plate_axes(const nlohmann::json& object, const std::string& prefix, PlateAxes& out);

// One transform_objects entry, read whole and checked before any entry is applied, so applying it
// reads no JSON. `error` is empty when the entry can be applied; the batch is applied only when no
// entry has one, so a rejected entry never leaves the others half done.
struct TransformEntry
{
    int         object_id = -1;
    PlateAxes   position;                  // the bounding-box centre in plate mm; an axis not given stays
    Vec3d       rotation = Vec3d::Zero();  // degrees about the plate's axes, applied X then Y then Z
    Vec3d       scale    = Vec3d::Ones();  // the entry's scale factors, ones when it gives none
    std::string error;
};
std::vector<TransformEntry> read_transform_entries(const nlohmann::json& transforms, size_t object_count);

// What is wrong with transform_objects' `transforms` argument -- missing, or not an array -- or nullopt.
// Reading a missing key from a const json with operator[] is undefined behaviour, not an exception.
std::optional<std::string> transforms_argument_error(const nlohmann::json& params);

// The box every MCP tool reports an object by, and reads and writes its position ("the
// bounding-box centre") in: the exact world box of all its instances, ModelObject::bounding_box_exact,
// which transforms every vertex and is cached until the object changes. Not bounding_box_approx():
// that one turns the mesh's own box with each instance, and once an instance is rotated the turned
// box's corners stand off the part -- a T-shaped part tilted 30 degrees and resting on the bed read
// min z -4 mm, its footprint too wide, with footprint_is_exact true.
const BoundingBoxf3& object_world_box(const ModelObject& object);

// The instances of one object a plate holds, and their exact world box. Every per-plate description
// of an object is built from this -- get_scene_info's entry and occupancy footprint, the first-layer
// plan, the prime tower's conflicts -- and a render's fit to the object frames it: an object with
// instances on several plates is described under each plate by the instances there, never by the
// box spanning them all. `holds(i)` says whether the plate holds instance i.
struct InstancesOnPlate
{
    std::vector<int> ids;  // instance indices, ascending
    BoundingBoxf3    box;  // the union of their exact boxes; undefined when there are none
};
InstancesOnPlate instances_on_plate(const ModelObject& object, const std::function<bool(int)>& holds);

// The same, read from `plate`'s own instance list (PartPlate::contain_instance), the list
// get_scene_info groups objects by. `object_index` is the object's index in the model.
InstancesOnPlate instances_on_plate(const ModelObject& object, int object_index, PartPlate& plate);

// The box a per-plate description of `object` uses: its instances' there, or the whole object's for
// an object the plate lists without holding an instance of it (only a stale list does that).
BoundingBoxf3 plate_box_of(const ModelObject& object, const InstancesOnPlate& here);

// Whether setting every instance of `object` printable (or not) changes one: a call that changes
// nothing takes no undo snapshot and leaves the plates' slice results alone.
bool printable_changes(const ModelObject& object, bool printable);

// Whether set_brim_ears changes an object's ears: `given` replaces `current`, or is added to it with
// `append`. The same ears again, or none appended, change nothing.
bool brim_ears_change(const std::vector<BrimPoint>& current, const std::vector<BrimPoint>& given, bool append);

// The keys of an object's overrides (`object_keys`) that a reset with no keys named clears: every one
// but "extruder", so the object keeps its filament, as the GUI's reset leaves it.
std::vector<std::string> object_overrides_to_reset(const std::vector<std::string>& object_keys);

// The index of `object` in the plater's model, matched by pointer or by ObjectID (a Print's copy of
// an object carries the original's id), or -1.
int model_object_index(const ModelObject* object);
// The same in `model`, for code that reads a Print without the app (a unit test's Print and Model).
int model_object_index(const Model& model, const ModelObject* object);

// A change to what a plate prints reaches the plate's Print only when the plate is next applied --
// selected, or reached by Slice All -- so until then an unselected plate reported its old result as
// valid: get_slicing_status's plates, get_print_estimate(plate_index) and the run's outcome all went
// by it. A tool that changes an object's settings, layers, name or filaments without moving it (a move
// re-homes the instance, and the plate list marks the plates itself) marks every plate holding one of
// its instances not sliced, as Tab::on_presets_changed marks every plate after a preset edit. The
// Print is left as it is: one the change did not reach is still finished, and the next slice takes
// its result back without slicing it again (PlateNotStarted::already_sliced).
void mark_object_plates_unsliced(PartPlateList& plates, int object_index);
// The same for one plate, by index: a change to that plate's own settings (its prime tower).
void mark_plate_unsliced(PartPlateList& plates, int plate_index);

// One model object as every MCP response describes it: id, name, object_index (the index other
// tools take), instance_count, volume_count, position (bounding-box centre), rotation_degrees and
// scale of the first instance, bounding_box {size_x, size_y, size_z, min, max}, in plate mm, and
// mesh_warning (with mesh_warning_reason when true: add_mesh_warning, OrcaMCPMeshHealth.hpp). A
// caller describing many objects reads their health once (model_mesh_health) and passes it in.
// get_scene_info adds brim, footprint, layer-height and filament fields; load_model's
// loaded_objects is exactly this.
nlohmann::json model_object_summary_json(const ModelObject& object, int object_index);
nlohmann::json model_object_summary_json(const ModelObject& object, int object_index, const MeshHealth& health);

// The same object as one plate's entry describes it: bounding_box and position are those of the
// instances `here` covers (instances_on_plate), rotation_degrees and scale are the first of them,
// and instances_on_plate lists them.
nlohmann::json model_object_summary_json(const ModelObject& object, int object_index, const InstancesOnPlate& here);
nlohmann::json model_object_summary_json(const ModelObject& object, int object_index, const InstancesOnPlate& here,
                                         const MeshHealth& health);

// Always returns {"count": N, "warnings": [{level, message, type}...]}.
nlohmann::json get_active_warnings_json(Plater* plater);

// Appends `entries` to an active_warnings section and sets its count to match: for warnings only
// some tools report, such as get_scene_info's and load_model's MeshErrors (mesh_error_warnings).
void add_warnings(nlohmann::json& active_warnings, const nlohmann::json& entries);

// Adds the preview `capture` makes to `result`: preview_path, or preview_error when capture fails,
// by returning {"error": ...} or by throwing. Never throws: a preview rides on a call that has
// already changed the scene, and an error there would read as "nothing happened" -- the retry then
// applies the change twice.
void add_preview_to(nlohmann::json& result, const std::function<nlohmann::json()>& capture);

// A turntable preview of the selected plate, through add_preview_to, when `include_preview` is set.
void add_turntable_preview_if_requested(nlohmann::json& result, bool include_preview, int view_count = 4, int resolution = 256);

// `lines` one per line, for a message made of several.
inline std::string join_lines(const std::vector<std::string>& lines)
{
    std::string joined;
    for (const std::string& line : lines)
        joined += (joined.empty() ? "" : "\n") + line;
    return joined;
}

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
    // Everything the suppressed dialogs said, errors included.
    std::vector<std::string> messages() const { return get_mcp_suppressed_messages(); }
    // The errors among them: what show_error would have shown (add_mcp_suppressed_error).
    std::vector<std::string> errors() const { return get_mcp_suppressed_errors(); }
    // The messages that are not errors, for a response that reports its errors apart.
    std::vector<std::string> notices() const
    {
        std::vector<std::string> said = messages();
        for (const std::string& error : errors())
            if (auto it = std::find(said.begin(), said.end(), error); it != said.end())
                said.erase(it);
        return said;
    }
    // Adds what the suppressed dialogs said to `response`: info_messages and error_messages, each
    // when there is one. Returns the response, so a handler can end with `return guard.report(result);`
    // on every path.
    nlohmann::json report(nlohmann::json response) const
    {
        if (const auto said = notices(); !said.empty())
            response["info_messages"] = said;
        if (const auto failed = errors(); !failed.empty())
            response["error_messages"] = failed;
        return response;
    }
    // A call the app answered with an error dialog failed, whatever the handler made of it: status
    // error, message the dialog's words, and error_messages. Unchanged when there was no error.
    nlohmann::json fail_on_errors(nlohmann::json response) const
    {
        const std::vector<std::string> failed = errors();
        if (failed.empty())
            return response;
        response["status"]         = "error";
        response["message"]        = join_lines(failed);
        response["error_messages"] = failed;
        return response;
    }
    void answer_prompt(const std::string& key, int answer_id, const std::string& note = std::string())
    {
        set_mcp_prompt_answer(key, answer_id, note);
    }

private:
    bool m_was_enabled;
};

// The answer of load_model, load_project and new_project. A load that changed the scene (added
// objects, opened the project, started a new one) succeeded, even when the app raised an error dialog
// on the way: reporting it failed sends an agent to load it again, and the scene then holds the
// objects twice. Its errors go beside it as error_messages. A load that changed nothing failed, with
// the dialogs' words (fail_on_errors).
inline nlohmann::json load_answer(const McpDialogSuppressionGuard& guard, bool changed_scene, nlohmann::json response)
{
    if (!changed_scene)
        return guard.fail_on_errors(std::move(response));
    if (const std::vector<std::string> errors = guard.errors(); !errors.empty())
        response["error_messages"] = errors;
    return response;
}

// One undo snapshot for a tool call: taken right before its first change, and never for a call that
// changes nothing. A snapshot discards the redo stack, so a no-op call must not take one; and a call
// that changes several objects is one undo step, as the GUI's own edits are.
class SnapshotOnce
{
public:
    explicit SnapshotOnce(std::function<void()> take) : m_take(std::move(take)) {}
    // Call right before each change; the first call takes the snapshot.
    void before_change()
    {
        if (!m_taken) {
            m_take();
            m_taken = true;
        }
    }
    bool taken() const { return m_taken; }

private:
    std::function<void()> m_take;
    bool                  m_taken = false;
};

// Applies a settings change the slicer has not taken in yet (`apply`: Plater::apply_pending_background_update)
// when should_apply_pending_update says so, and says whether it did. The update can raise an error
// dialog (show_error), and one raised with no suppression open is a modal that blocks every later
// call, so it takes the caller's open guard, which captures what the update says.
template<typename Apply>
bool apply_pending_update(const McpDialogSuppressionGuard&, const PipelineState& state, bool update_scheduled, Apply&& apply)
{
    if (!should_apply_pending_update(state, update_scheduled))
        return false;
    apply();
    return true;
}

}}} // namespace Slic3r::GUI::OrcaMCP
