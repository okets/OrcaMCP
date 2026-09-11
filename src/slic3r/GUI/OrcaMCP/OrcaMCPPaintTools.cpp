// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPaintModel.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// Every number and every integer id in this file is read through one of these two, so a caller
// gets the same kind of message wherever it mistypes a scalar. The alternative -- a bare
// get<double>() or a typed json::value -- throws nlohmann's type_error.302 ("type must be number,
// but is string"), which the dispatcher catches and forwards verbatim: safe, but it names neither
// the field nor what would have been accepted, and this batch exists to give agents actionable
// errors. parse_double_param/parse_integer_param also accept the stringified spelling a client
// with a cached older schema sends ("110", "2"), which is the same leniency parse_boolean_param
// already applied to `replace` and `append` in this file.
bool parse_number_field(const nlohmann::json& value, const std::string& what, double& out, std::string& error)
{
    if (parse_double_param(value, out))
        return true;
    // One message for every rejection parse_double_param makes -- not a number at all, a
    // non-finite one, or a hex spelling -- because "finite number" describes what is wanted in
    // all three cases and the field name is what the caller actually needs here.
    error = what + " must be a finite number";
    return false;
}

// Optional integer parameter: absent leaves `fallback`. Kept integral -- parse_integer_param
// refuses 1.5 -- because every caller of this reads an index, and rounding an index is how a
// caller ends up painting a different object than it asked for.
bool parse_integer_field(const nlohmann::json& params, const char* key, int fallback, int& out, std::string& error)
{
    out = fallback;
    if (!params.contains(key))
        return true;
    if (!parse_integer_param(params[key], out)) {
        error = std::string(key) + " must be a whole number";
        return false;
    }
    return true;
}

// What a tool needs resolved out of its parameters. Defaulted to the paint tools' needs, which
// three of the four have; the one exception states itself at its call site.
struct PaintTargetNeeds
{
    // Read volume_id, collect the object's model parts, and fail an object that has none. Only a
    // tool that writes or reads facet paint needs that -- set_brim_ears addresses the object as a
    // whole (brim_points live on the ModelObject), so requiring it a surface to paint would refuse
    // an object it could serve perfectly well, over a parameter its schema does not even declare.
    bool volumes = true;
    // Read instance_id: which instance's transform the caller's coordinates are read through. A
    // tool whose request and response carry no coordinates at all has no frame to choose, and
    // validating the parameter anyway lets it reject a call it could have served.
    bool instance = true;
};

// Main thread only. Reads object_id (required), and -- each only when the caller says it needs it
// -- instance_id (optional, default 0) and volume_id (optional, -1 = every model part).
//
// A tool that writes paint validates through here first and only then takes its undo snapshot
// (the set_object_printable / set_object_filament shape), and follows the write with
// refresh_after_paint below, which carries the GUI bookkeeping every such write owes.
bool resolve_paint_target(const nlohmann::json&  params,
                          PaintTarget&           out,
                          std::string&           error,
                          const PaintTargetNeeds needs = {})
{
    Plater*        plater = wxGetApp().plater();
    Slic3r::Model& model  = plater->model();

    // Told apart from a bad one: defaulting a missing object_id to -1 and falling into the range
    // check below would report "Invalid object_id -1", which reads as a value the caller chose.
    if (!params.contains("object_id")) {
        error = "object_id is required: pass the 0-based index of the object to paint";
        return false;
    }
    int object_id = -1;
    if (!parse_integer_field(params, "object_id", -1, object_id, error))
        return false;
    if (object_id < 0 || object_id >= int(model.objects.size())) {
        error = "Invalid object_id " + std::to_string(object_id) + ": the scene has " +
                std::to_string(model.objects.size()) + " objects";
        return false;
    }
    out.object    = model.objects[std::size_t(object_id)];
    out.object_id = object_id;

    if (needs.instance) {
        int instance_id = 0;
        if (!parse_integer_field(params, "instance_id", 0, instance_id, error))
            return false;
        if (instance_id < 0 || instance_id >= int(out.object->instances.size())) {
            error = "Invalid instance_id " + std::to_string(instance_id) + ": the object has " +
                    std::to_string(out.object->instances.size()) + " instances";
            return false;
        }
        out.instance_idx = std::size_t(instance_id);
    }

    if (!needs.volumes)
        return true;

    int volume_id = -1;
    if (!parse_integer_field(params, "volume_id", -1, volume_id, error))
        return false;
    if (volume_id < -1) {
        error = "Invalid volume_id " + std::to_string(volume_id) +
                ": use a 0-based part index, or omit it (or pass -1) for every part of the object";
        return false;
    }
    if (volume_id >= 0) {
        if (volume_id >= int(out.object->volumes.size())) {
            error = "Invalid volume_id " + std::to_string(volume_id) + ": the object has " +
                    std::to_string(out.object->volumes.size()) + " volumes";
            return false;
        }
        Slic3r::ModelVolume* mv = out.object->volumes[std::size_t(volume_id)];
        // Only a model part has a printed surface. A modifier, a support blocker or a negative
        // volume carries the same annotation members but nothing reads them, so painting one
        // would report success and change nothing the caller can see.
        if (!mv->is_model_part()) {
            error = "volume_id " + std::to_string(volume_id) +
                    " is not a model part, so it has no surface to paint";
            return false;
        }
        out.volumes.push_back(mv);
        out.volume_ids.push_back(volume_id);
    } else {
        for (int i = 0; i < int(out.object->volumes.size()); ++i)
            if (out.object->volumes[std::size_t(i)]->is_model_part()) {
                out.volumes.push_back(out.object->volumes[std::size_t(i)]);
                out.volume_ids.push_back(i);
            }
        if (out.volumes.empty()) {
            error = "Object " + std::to_string(object_id) + " has no model parts to paint";
            return false;
        }
    }
    return true;
}

// The `mode` parameter both paint readers/clearers share: absent means all four annotations,
// present means exactly one. Kept here, once, so get_object_paint and clear_object_paint cannot
// drift the way a copy of this block did before this was factored out.
bool resolve_paint_modes(const nlohmann::json& params, std::vector<PaintMode>& out, std::string& error)
{
    out = {PaintMode::Color, PaintMode::Support, PaintMode::Seam, PaintMode::FuzzySkin};
    if (!params.contains("mode"))
        return true;
    PaintMode one = PaintMode::Color;
    if (!parse_paint_mode(params["mode"].get<std::string>(), one)) {
        error = "Unknown mode; expected color, support, seam or fuzzy_skin";
        return false;
    }
    out = {one};
    return true;
}

// The plate-frame bounding box of exactly the volumes a call paints, so a band range defaulted
// from it covers what is actually being painted rather than the whole object.
Slic3r::BoundingBoxf3 target_plate_bbox(const PaintTarget& target)
{
    Slic3r::BoundingBoxf3 bbox;
    for (Slic3r::ModelVolume* mv : target.volumes)
        bbox.merge(mv->mesh().transformed_bounding_box(
            volume_to_plate(*target.object, *mv, target.instance_idx)));
    return bbox;
}

// The plate-frame bounding box of every model part on an object, at one instance's transform.
// set_brim_ears needs this rather than target_plate_bbox above because it resolves no volumes
// of its own (PaintTargetNeeds::volumes is false for it -- brim ears are object-level, not tied
// to a model part): the box it reports has to come from the object directly.
Slic3r::BoundingBoxf3 object_plate_bbox(const Slic3r::ModelObject& object, std::size_t instance_idx)
{
    Slic3r::BoundingBoxf3 bbox;
    for (Slic3r::ModelVolume* mv : object.volumes)
        if (mv->is_model_part())
            bbox.merge(mv->mesh().transformed_bounding_box(volume_to_plate(object, *mv, instance_idx)));
    return bbox;
}

nlohmann::json bbox_json(const Slic3r::BoundingBoxf3& bbox)
{
    return {{"min", {{"x", bbox.min.x()}, {"y", bbox.min.y()}, {"z", bbox.min.z()}}},
            {"max", {{"x", bbox.max.x()}, {"y", bbox.max.y()}, {"z", bbox.max.z()}}}};
}

// One mode's paint on one volume, as the response reports it.
nlohmann::json painted_json(const Slic3r::ModelVolume& mv, PaintMode mode)
{
    nlohmann::json states = nlohmann::json::array();
    for (const PaintedStateInfo& info : read_volume_paint(mv, mode)) {
        nlohmann::json entry = {{"state", info.state},
                                {"label", paint_state_label(mode, info.state)},
                                {"facet_count", info.facet_count},
                                {"coverage_percent", info.area_ratio * 100.0}};
        entry["filament"] = mode == PaintMode::Color && info.state > 0
                                ? nlohmann::json(info.state)
                                : nlohmann::json(nullptr);
        states.push_back(entry);
    }
    return states;
}

// The brim ears on an object, converted back into the plate coordinates the API speaks.
// brim_points are stored object-local (Model.hpp:390); the actual object-local -> plate
// transform is brim_point_to_plate (OrcaMCPPaintModel), which is what set_brim_ears's write
// path inverts, so this and that call cannot drift into different frames.
nlohmann::json brim_ears_json(const Slic3r::ModelObject& obj, std::size_t instance_idx)
{
    nlohmann::json ears = nlohmann::json::array();
    for (const Slic3r::BrimPoint& point : obj.brim_points) {
        const Slic3r::Vec3d plate_pos = brim_point_to_plate(obj, point.pos, instance_idx);
        ears.push_back({{"x", plate_pos.x()},
                        {"y", plate_pos.y()},
                        {"z", plate_pos.z()},
                        {"radius", double(point.head_front_radius)}});
    }
    return ears;
}

// The GUI bookkeeping a paint write owes. It lives here, once, because every tool that writes
// paint owes exactly the same and a copy per tool is how the four modes drift apart.
// GLGizmoMmuSegmentation::update_model_object does the same three things after its
// FacetsAnnotation::set (GLGizmoMmuSegmentation.cpp, update_model_object). Without them the
// annotation is correct in the data and invisible in the app: the object list keeps the info
// items it built before the paint existed, and the plate keeps a slice result computed without
// it, so the preview never shows the paint and nothing reslices. (The gizmo's fourth call,
// update_used_filaments, refreshes a cache internal to the gizmo and has no MCP equivalent.)
//
// Every instance is notified, not just the one whose frame the coordinates were read in. Paint
// lives on the ModelVolume, so it applies to every instance of the object, and instances can sit
// on different plates; notifying one would leave the other plates holding a slice result that no
// longer matches the model.
//
// ONLY SAFE AFTER A WRITE THAT LEAVES THE MESH GEOMETRY UNCHANGED, which paint does. If an
// instance's convex-hull bounding box moves, notify_instance_update re-homes it onto another plate
// and that branch can reach show_spiral_mode_settings_dialog (PartPlate.cpp, the spiral_mode check
// in the add_instance branch), a MessageDialog. These tools run without a suppression guard, and a
// modal opened inside run_on_main_thread blocks the GUI thread forever, so the MCP call never
// returns. Paint never moves a vertex, so the re-homing branch is unreachable from here.
void refresh_after_paint(const PaintTarget& target)
{
    Plater* plater = wxGetApp().plater();
    wxGetApp().obj_list()->update_info_items(std::size_t(target.object_id));
    for (std::size_t i = 0; i < target.object->instances.size(); ++i)
        plater->get_partplate_list().notify_instance_update(target.object_id, int(i));
    // Rescheduling the background process is what eventually makes the paint visible in the
    // preview. Guarded the way Plater.cpp guards its own canvas notifications: get_view3D_canvas3D
    // returns null only when the Plater pimpl is gone, and a canvas that exists but has not run its
    // first render has no event handler worth posting to yet.
    GLCanvas3D* canvas = plater->get_view3D_canvas3D();
    if (canvas && canvas->is_initialized())
        canvas->post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    plater->update();
}

// A filament slot a caller asked to paint with. 0 means "unpainted" -- back to whatever filament
// the volume itself is assigned -- which is the colour-mode equivalent of state NONE.
bool validate_color_slot(int slot, std::string& error)
{
    if (slot == 0)
        return true;
    const int filament_count = int(wxGetApp().preset_bundle->filament_presets.size());
    if (slot < 0 || slot > filament_count) {
        error = "filament " + std::to_string(slot) + " out of range 1.." + std::to_string(filament_count) +
                " (0 means unpainted)";
        return false;
    }
    if (slot > max_paint_state()) {
        error = "filament " + std::to_string(slot) + " cannot be painted: a facet state stops at " +
                std::to_string(max_paint_state()) + " (EnforcerBlockerType::ExtruderMax), so slots above "
                "that can only be assigned to a whole object or part with set_object_filament";
        return false;
    }
    return true;
}

// One parsed paint_object call.
struct PaintRequest
{
    PaintMode              mode      = PaintMode::Color;
    std::string            selection;                 // "bands" | "box" | "sphere" | "all"
    PaintAxis              axis      = PaintAxis::Z;
    std::vector<PaintBand> bands;                     // resolved, even split already expanded
    double                 range_from = 0.0;
    double                 range_to   = 0.0;
    PaintBox               box;
    PaintSphere            sphere;
    int                    state     = 0;             // box / sphere / all
    bool                   replace   = true;
};

// Reads the one state a non-band selection paints with: `filament` in colour mode, `state` in
// the other three.
bool parse_single_state(const nlohmann::json& params, PaintMode mode, int& out, std::string& error)
{
    if (mode == PaintMode::Color) {
        if (!params.contains("filament")) {
            error = "mode 'color' needs a `filament` (1-based slot, or 0 to unpaint)";
            return false;
        }
        // Through parse_integer_param rather than get<int>: a client whose cached tool schema
        // predates this tool sends "2" as a string, and get<int> would throw a nlohmann type_error
        // instead of the message validate_color_slot exists to produce.
        if (!parse_integer_param(params["filament"], out)) {
            error = "filament must be a whole number: a 1-based slot, or 0 to unpaint";
            return false;
        }
        return validate_color_slot(out, error);
    }
    if (!params.contains("state")) {
        error = std::string("mode '") + paint_mode_name(mode) +
                "' needs a `state`: none, enforcer" + (mode == PaintMode::FuzzySkin ? "" : " or blocker");
        return false;
    }
    if (!parse_paint_state(mode, params["state"].get<std::string>(), out)) {
        error = std::string("Unknown state for mode '") + paint_mode_name(mode) + "': expected none, enforcer" +
                (mode == PaintMode::FuzzySkin
                     ? " (fuzzy skin has no blocker: it is painted or it is not)"
                     : " or blocker");
        return false;
    }
    return true;
}

bool read_vec3(const nlohmann::json& value, Slic3r::Vec3d& out, const std::string& what, std::string& error)
{
    if (!value.is_array() || value.size() != 3) {
        error = what + " must be an array of three numbers [x, y, z] in plate millimetres";
        return false;
    }
    // Per component, so a caller that mistyped one of the three is told which: the whole-array
    // message above cannot say that, and a bare get<double>() would name nothing at all.
    for (int i = 0; i < 3; ++i)
        if (!parse_number_field(value[std::size_t(i)], what + "[" + std::to_string(i) + "]", out[i], error))
            return false;
    return true;
}

bool parse_paint_request(const nlohmann::json& params,
                         PaintMode             mode,
                         const PaintTarget&    target,
                         PaintRequest&         out,
                         std::string&          error)
{
    out.mode    = mode;
    // json::value with a bool default throws type_error.302 on "replace": "false", which is what a
    // client with a stale schema sends; parse_boolean_param accepts that spelling. Anything it
    // cannot read is reported rather than silently taken as false -- replace decides whether the
    // existing paint survives, so guessing at it is the one thing not to do here.
    out.replace = true;
    if (params.contains("replace") && !parse_boolean_param(params["replace"], out.replace)) {
        error = "replace must be true or false";
        return false;
    }

    out.selection = params.value("selection", std::string());
    if (out.selection.empty()) {
        error = "selection is required: bands, box, sphere or all";
        return false;
    }

    if (out.selection == "bands") {
        std::string axis_name = params.value("axis", std::string());
        if (!parse_paint_axis(axis_name, out.axis)) {
            error = "selection 'bands' needs an axis: x, y or z (plate axes)";
            return false;
        }

        const Slic3r::BoundingBoxf3 bbox = target_plate_bbox(target);
        const int                   row  = int(out.axis);
        out.range_from = bbox.min[row];
        out.range_to   = bbox.max[row];
        if (params.contains("from") && !parse_number_field(params["from"], "from", out.range_from, error))
            return false;
        if (params.contains("to") && !parse_number_field(params["to"], "to", out.range_to, error))
            return false;

        if (params.contains("bands")) {
            const nlohmann::json& raw = params["bands"];
            if (!raw.is_array() || raw.empty()) {
                error = "bands must be a non-empty array of {from, to, filament|state}";
                return false;
            }
            for (const nlohmann::json& entry : raw) {
                PaintBand band;
                if (!entry.contains("from") || !entry.contains("to")) {
                    error = "every band needs `from` and `to` in plate millimetres";
                    return false;
                }
                if (!parse_number_field(entry["from"], "band.from", band.from, error) ||
                    !parse_number_field(entry["to"], "band.to", band.to, error))
                    return false;
                if (!(band.to > band.from)) {
                    error = "band from " + std::to_string(band.from) + " to " + std::to_string(band.to) +
                            " is empty: `to` must exceed `from`";
                    return false;
                }
                if (!parse_single_state(entry, mode, band.state, error))
                    return false;
                out.bands.push_back(band);
            }
            // An explicit set defines its own range; report the span it actually covers.
            out.range_from = out.bands.front().from;
            out.range_to   = out.bands.front().to;
            for (const PaintBand& band : out.bands) {
                out.range_from = std::min(out.range_from, band.from);
                out.range_to   = std::max(out.range_to, band.to);
            }
            return true;
        }

        if (mode != PaintMode::Color) {
            error = std::string("mode '") + paint_mode_name(mode) +
                    "' has no even-split form: pass explicit `bands` with a `state` each. An even "
                    "split alternates filaments, which only means something in colour mode";
            return false;
        }
        if (!params.contains("filaments") || !params["filaments"].is_array() || params["filaments"].empty()) {
            error = "selection 'bands' needs either `filaments` (one 1-based slot per band, split "
                    "evenly) or explicit `bands`";
            return false;
        }
        std::vector<int> slots;
        for (const nlohmann::json& slot : params["filaments"]) {
            int value = 0;
            if (!parse_integer_param(slot, value)) {
                error = "every entry of `filaments` must be a whole number: a 1-based slot, or 0 "
                        "to unpaint";
                return false;
            }
            if (!validate_color_slot(value, error))
                return false;
            slots.push_back(value);
        }
        out.bands = make_even_bands(slots, out.range_from, out.range_to);
        if (out.bands.empty()) {
            error = "cannot split " + std::to_string(out.range_from) + ".." + std::to_string(out.range_to) +
                    " into " + std::to_string(slots.size()) +
                    " bands: `to` must exceed `from`. With no from/to the object's own extent along "
                    "the axis is used, so a zero span means the object is flat on that axis";
            return false;
        }
        return true;
    }

    if (out.selection == "box") {
        if (!params.contains("box") || !params["box"].contains("min") || !params["box"].contains("max")) {
            error = "selection 'box' needs box: {min: [x,y,z], max: [x,y,z]} in plate millimetres";
            return false;
        }
        if (!read_vec3(params["box"]["min"], out.box.min, "box.min", error) ||
            !read_vec3(params["box"]["max"], out.box.max, "box.max", error))
            return false;
        if (!paint_box_is_valid(out.box)) {
            error = "box.min must not exceed box.max on any axis";
            return false;
        }
        return parse_single_state(params, mode, out.state, error);
    }

    if (out.selection == "sphere") {
        if (!params.contains("sphere") || !params["sphere"].contains("center") ||
            !params["sphere"].contains("radius")) {
            error = "selection 'sphere' needs sphere: {center: [x,y,z], radius: n} in plate millimetres";
            return false;
        }
        if (!read_vec3(params["sphere"]["center"], out.sphere.center, "sphere.center", error))
            return false;
        if (!parse_number_field(params["sphere"]["radius"], "sphere.radius", out.sphere.radius, error))
            return false;
        if (!(out.sphere.radius > 0.0)) {
            error = "sphere.radius must be greater than zero";
            return false;
        }
        return parse_single_state(params, mode, out.state, error);
    }

    if (out.selection == "all")
        return parse_single_state(params, mode, out.state, error);

    error = "Unknown selection '" + out.selection + "': expected bands, box, sphere or all";
    return false;
}

// The object's own override of a print setting if it has one, otherwise the global print preset's
// value -- the same precedence the slicer applies. Null when neither carries the key, which a
// caller of this is expected to treat as "nothing to warn about" rather than dereference: opt_bool
// and opt_enum both go through ConfigOption::getInt and would crash on a missing option.
const Slic3r::ConfigOption* effective_print_option(const Slic3r::DynamicPrintConfig& object_cfg,
                                                   const Slic3r::DynamicPrintConfig& global,
                                                   const char*                       key)
{
    const Slic3r::ConfigOption* option = object_cfg.option(key);
    return option ? option : global.option(key);
}

// Painted supports and painted fuzzy skin do nothing unless the corresponding setting is on.
// The gizmos say so on screen (GLGizmoFdmSupports.cpp, GLGizmoFuzzySkin.cpp); over MCP the
// equivalent is an info message, or the caller paints, slices, and sees no difference.
std::vector<std::string> paint_prerequisite_messages(const Slic3r::ModelObject& obj, PaintMode mode)
{
    std::vector<std::string> messages;
    const Slic3r::DynamicPrintConfig& global     = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const Slic3r::DynamicPrintConfig& object_cfg = obj.config.get();

    if (mode == PaintMode::Support) {
        const Slic3r::ConfigOption* option = effective_print_option(object_cfg, global, "enable_support");
        if (option && !option->getBool())
            messages.push_back("Painted support enforcers and blockers have no effect while "
                               "enable_support is false. Set it with apply_config or set_object_config.");
    }
    if (mode == PaintMode::FuzzySkin) {
        const Slic3r::ConfigOption* option = effective_print_option(object_cfg, global, "fuzzy_skin");
        if (option && option->getInt() == int(Slic3r::FuzzySkinType::Disabled_fuzzy))
            messages.push_back("Painted fuzzy skin has no effect while fuzzy_skin is 'disabled_fuzzy' "
                               "(the default). Set fuzzy_skin to 'none' to use painted regions only.");
    }
    return messages;
}

} // namespace

void OrcaMCPServer::register_paint_tools()
{
    register_tool({
        "paint_object",
        "Paint per-triangle annotations on an object, the same data the GUI paint gizmos write. "
        "mode selects which: color (multi-material / MMU segmentation), support, seam or "
        "fuzzy_skin. selection selects where: bands along a plate axis (an even split across a "
        "list of filaments, or explicit ranges), a box, a sphere, or the whole volume. "
        "ALL COORDINATES ARE PLATE MILLIMETRES -- the same frame get_object_info reports its "
        "bounding_box and position in, not object-local coordinates. But for the numbers, use "
        "THIS call's own bounding_box in the response (or get_object_paint's), not "
        "get_object_info's: that one is a looser box (untransformed-AABB corners, unioned over "
        "every instance) and only matches this tool's for a single unrotated instance. A facet "
        "belongs to the band or region containing its centroid. Paint lives on the volume, so it "
        "applies to every instance; instance_id only says whose transform reads your "
        "coordinates. Verify with get_object_paint, undo with undo, reset with clear_object_paint.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"description", "Object index (0-based)"}
                }},
                {"volume_id", {
                    {"type", "integer"},
                    {"minimum", -1},
                    {"description", "Part index within the object (0-based); omit, or pass -1, for every part"}
                }},
                {"instance_id", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"description", "Which instance's transform reads your coordinates (default 0)"}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", {"color", "support", "seam", "fuzzy_skin"}},
                    {"description", "Which annotation to write (default: color)"}
                }},
                {"selection", {
                    {"type", "string"},
                    {"enum", {"bands", "box", "sphere", "all"}},
                    {"description", "Where to paint"}
                }},
                {"axis", {
                    {"type", "string"},
                    {"enum", {"x", "y", "z"}},
                    {"description", "Plate axis the bands run along (selection=bands)"}
                }},
                {"filaments", {
                    {"type", "array"},
                    {"items", {{"type", "integer"}}},
                    {"description", "One 1-based filament slot per band, split evenly along the axis "
                                    "(selection=bands, mode=color). 0 means unpainted."}
                }},
                {"bands", {
                    {"type", "array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"from", {{"type", "number"}}},
                            {"to", {{"type", "number"}}},
                            {"filament", {{"type", "integer"}}},
                            {"state", {{"type", "string"},
                                       {"enum", {"none", "enforcer", "blocker", "fuzzy_skin"}}}}
                        }}
                    }},
                    {"description", "Explicit ranges in plate mm, each with a filament (mode=color) or "
                                    "a state. Ranges need not tile the object; facets outside them all "
                                    "are left alone."}
                }},
                {"from", {{"type", "number"}, {"description", "Start of the even split in plate mm "
                                                              "(default: the object's own extent). "
                                                              "Ignored when explicit `bands` are given, "
                                                              "which carry their own ranges."}}},
                {"to", {{"type", "number"}, {"description", "End of the even split in plate mm "
                                                            "(default: the object's own extent). "
                                                            "Ignored when explicit `bands` are given, "
                                                            "which carry their own ranges."}}},
                {"box", {
                    {"type", "object"},
                    {"properties", {
                        {"min", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                        {"max", {{"type", "array"}, {"items", {{"type", "number"}}}}}
                    }},
                    {"description", "Axis-aligned box in plate mm (selection=box)"}
                }},
                {"sphere", {
                    {"type", "object"},
                    {"properties", {
                        {"center", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                        {"radius", {{"type", "number"}}}
                    }},
                    {"description", "Sphere in plate mm (selection=sphere)"}
                }},
                {"filament", {
                    {"type", "integer"},
                    {"description", "1-based filament slot for selection=box/sphere/all (mode=color). "
                                    "0 means unpainted."}
                }},
                {"state", {
                    {"type", "string"},
                    // "fuzzy_skin" is the token get_object_paint reads back for that mode's
                    // enforcer state, so it has to be accepted here or the round-trip we built
                    // is blocked for a client that validates against this schema.
                    {"enum", {"none", "enforcer", "blocker", "fuzzy_skin"}},
                    {"description", "State for selection=box/sphere/all when mode is not color. "
                                    "fuzzy_skin accepts none and enforcer (spelled either "
                                    "'enforcer' or 'fuzzy_skin'), never blocker."}
                }},
                {"replace", {
                    {"type", "boolean"},
                    {"description", "true (default) discards this mode's existing paint first; "
                                    "false paints on top of it"}
                }}
            }},
            {"required", {"object_id", "selection"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            // Three hops, because painting a multi-million-facet mesh takes minutes and the GUI
            // thread is the only one every other tool also needs. Only the validation and the
            // write actually touch the Model; the arithmetic between them does not, and doing it
            // inside run_on_main_thread made the whole MCP server unreachable for the duration.
            //
            // Hop 1 -- main thread: validate everything, take nothing, capture the plan.
            PaintPlan    plan;
            PaintRequest request;
            PaintMode    mode = PaintMode::Color;
            {
                nlohmann::json gate = run_on_main_thread([&params, &plan, &request, &mode]() -> nlohmann::json {
                    PaintTarget target;
                    std::string error;
                    if (!resolve_paint_target(params, target, error))
                        return nlohmann::json{{"status", "error"}, {"message", error}};

                    // is_string first: get<std::string>() on a number throws nlohmann's
                    // type_error.302, which the dispatcher forwards verbatim in place of the
                    // message below. Same leniency the scalar parameters already get in this file.
                    if (params.contains("mode") &&
                        !(params["mode"].is_string() && parse_paint_mode(params["mode"].get<std::string>(), mode)))
                        return nlohmann::json{{"status", "error"},
                                              {"message", "Unknown mode; expected color, support, seam or fuzzy_skin"}};

                    if (!parse_paint_request(params, mode, target, request, error))
                        return nlohmann::json{{"status", "error"}, {"message", error}};

                    // apply_facet_states rejects a mesh with no facets, and its bool cannot be told
                    // apart from "the annotation already held this". Caught here, before the
                    // snapshot, so the call fails instead of reporting success over a volume it
                    // never touched.
                    for (std::size_t i = 0; i < target.volumes.size(); ++i)
                        if (target.volumes[i]->mesh().its.indices.empty())
                            return nlohmann::json{{"status", "error"},
                                                  {"message", "volume_id " + std::to_string(target.volume_ids[i]) +
                                                              " has an empty mesh, so it has no facets to paint"}};

                    plan = capture_paint_plan(target);
                    return nlohmann::json{{"status", "success"}};
                });
                if (gate.value("status", "") != "success")
                    return gate;
            }

            // Hop 2 -- this HTTP worker thread: the geometry, which is all of the cost. Nothing
            // here touches the Model: the meshes are shared_ptr<const> snapshots that outlive any
            // concurrent edit, and the transforms are copies.
            std::vector<FacetAssignment> assignments(plan.volumes.size());
            for (std::size_t i = 0; i < plan.volumes.size(); ++i) {
                const PaintPlanVolume& pv          = plan.volumes[i];
                const std::size_t      facet_count = pv.mesh->its.indices.size();
                if (request.selection == "all") {
                    // Every facet, no geometry: computing 4 million centroids to then ignore
                    // them was the whole-branch review's M11.
                    assignments[i] = assign_all(facet_count, request.state);
                } else {
                    const std::vector<Slic3r::Vec3d> centroids = facet_centroids(pv.mesh->its, pv.to_plate);
                    if (request.selection == "bands")
                        assignments[i] = assign_bands(centroids, request.axis, request.bands);
                    else if (request.selection == "box")
                        assignments[i] = assign_box(centroids, request.box, request.state);
                    else
                        assignments[i] = assign_sphere(centroids, request.sphere, request.state);
                }
            }

            // Hop 3 -- main thread: confirm the scene is unchanged, then snapshot, write, refresh.
            return run_on_main_thread([&params, &plan, &request, &mode, &assignments]() -> nlohmann::json {
                Plater*     plater = wxGetApp().plater();
                PaintTarget target;
                std::string error;
                // Re-resolved rather than remembered: the pointers hop 1 held could have been
                // freed while hop 2 ran. plan_still_valid then proves the freshly resolved scene
                // is the one the states were computed against.
                if (!resolve_paint_target(params, target, error) || !plan_still_valid(target, plan, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                // Everything is validated -- only now touch the undo stack, the way
                // set_object_filament does (OrcaMCPFilamentUtils.cpp, set_object_filament).
                // Named per mode: painting colour and then supports would otherwise leave two
                // identical entries in the undo menu with nothing to tell them apart.
                plater->take_snapshot(_u8L("Paint Object") + " (" + paint_mode_name(mode) + ")");

                // Facets the selection COVERED, which is not the same as facets given a paint:
                // `filament: 0` covers a facet and unpaints it. Named for what it counts.
                int              facets_selected   = 0;
                int              original_facets_total = 0;
                int              facets_unassigned = 0;
                std::vector<int> band_counts(request.bands.size(), 0);
                bool             changed = false;

                for (std::size_t i = 0; i < plan.volumes.size(); ++i) {
                    Slic3r::ModelVolume*   mv         = target.volumes[std::size_t(plan.volumes[i].index)];
                    const FacetAssignment& assignment = assignments[i];
                    original_facets_total += int(mv->mesh().its.indices.size());

                    facets_unassigned += assignment.unassigned;
                    for (std::size_t b = 0; b < assignment.band_counts.size() && b < band_counts.size(); ++b)
                        band_counts[b] += assignment.band_counts[b];
                    facets_selected += int(assignment.states.size()) - assignment.unassigned;

                    // False here can only mean "the annotation already held exactly this": every
                    // other reason apply_facet_states rejects a write was ruled out in hop 1, and
                    // plan_still_valid just proved the mesh it was ruled out on is still this one.
                    changed |= apply_facet_states(*mv, mode, assignment.states, request.replace);
                }

                refresh_after_paint(target);

                // The same per-volume shape get_object_paint and clear_object_paint report
                // `volumes` in: volume_id, name, original_facets, bounding_box, modes. Only this
                // call's one mode is populated in `modes` -- painting color proves nothing about
                // support, seam or fuzzy_skin, so this does not fabricate a read of the other
                // three -- but an agent that learned modes.<mode> from get_object_paint reads the
                // identical accessor here.
                nlohmann::json volumes = nlohmann::json::array();
                for (std::size_t i = 0; i < plan.volumes.size(); ++i) {
                    Slic3r::ModelVolume* mv = target.volumes[std::size_t(plan.volumes[i].index)];
                    nlohmann::json by_mode = nlohmann::json::object();
                    by_mode[paint_mode_name(mode)] = painted_json(*mv, mode);
                    volumes.push_back({
                        {"volume_id", plan.volumes[i].volume_id},
                        {"name", mv->name},
                        // Per volume, the way get_object_paint reports it, so the name means one
                        // thing across the feature: this volume's own mesh triangle count. The
                        // top-level original_facets_total is the sum, and says so in its name.
                        {"original_facets", int(mv->mesh().its.indices.size())},
                        // Through the plan's transform, which plan_still_valid has just confirmed
                        // is still the volume's own, so the box describes the frame the states
                        // were computed in rather than a second, separately-read one.
                        {"bounding_box", bbox_json(mv->mesh().transformed_bounding_box(plan.volumes[i].to_plate))},
                        {"modes", by_mode}
                    });
                }

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"mode", paint_mode_name(mode)},
                    {"selection", request.selection},
                    {"coordinate_frame", "plate"},
                    // The same field name and shape get_object_paint reports (its per-volume
                    // "bounding_box"): the snug AABB of this call's actually-transformed volumes,
                    // for this instance -- not get_object_info's looser bounding_box_approx
                    // (corners of the untransformed AABB, unioned over every instance), which
                    // agrees with this one only for a single unrotated instance. Reported here so
                    // an agent bands from the box these tools themselves use, never from the other
                    // one.
                    {"bounding_box", bbox_json(target_plate_bbox(target))},
                    {"instance_id", int(target.instance_idx)},
                    {"replace", request.replace},
                    {"annotation_changed", changed},
                    // The sum over the volumes this call addressed, which is why it is not called
                    // original_facets: that name means one volume's own mesh triangle count, here
                    // and in get_object_paint, and a field that means the count on one tool and a
                    // sum on the other is a field an agent cannot read. Either way the per-state
                    // facet_count counts leaf triangles and can exceed it, so the two are not a
                    // part and a whole; coverage_percent is the ratio to read.
                    {"original_facets_total", original_facets_total},
                    {"facets_selected", facets_selected},
                    {"facets_unassigned", facets_unassigned},
                    {"volumes", volumes},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                if (request.selection == "bands") {
                    result["axis"] = paint_axis_name(request.axis);
                    result["axis_range"] = {{"from", request.range_from}, {"to", request.range_to}};
                    nlohmann::json bands = nlohmann::json::array();
                    for (std::size_t i = 0; i < request.bands.size(); ++i) {
                        nlohmann::json band = {{"from", request.bands[i].from},
                                               {"to", request.bands[i].to},
                                               {"state", request.bands[i].state},
                                               {"label", paint_state_label(mode, request.bands[i].state)},
                                               {"facet_count", band_counts[i]}};
                        band["filament"] = mode == PaintMode::Color && request.bands[i].state > 0
                                               ? nlohmann::json(request.bands[i].state)
                                               : nlohmann::json(nullptr);
                        bands.push_back(band);
                    }
                    result["bands"] = bands;
                }

                std::vector<std::string> messages = paint_prerequisite_messages(*target.object, mode);
                if (facets_unassigned > 0) {
                    if (request.selection == "bands")
                        // The symptom of banding from too wide a range (e.g. get_object_info's
                        // looser bounding_box instead of this response's own): facets outside
                        // axis_range are silently left at their previous state. Previously this
                        // count was reported for every selection except bands -- the one where a
                        // mistyped from/to is most likely (M9).
                        messages.push_back(std::to_string(facets_unassigned) +
                                           " facets fell outside every band and kept their previous "
                                           "state. If that number is unexpectedly high, check "
                                           "axis_range against this response's own bounding_box.");
                    else
                        messages.push_back(std::to_string(facets_unassigned) +
                                           " facets fell outside the selection and kept their previous state.");
                }
                if (facets_selected == 0) {
                    // With replace this is not a no-op: the reset happened and the selection then
                    // wrote nothing back, so the volume came out bare. Saying only "nothing was
                    // painted" would read as "nothing happened".
                    std::string message = "Nothing was painted: no facet centroid fell inside the "
                                          "selection. Check the coordinates against this response's "
                                          "own bounding_box -- same plate frame as get_object_info, "
                                          "but a different (tighter, per-instance) computation.";
                    if (request.replace && changed)
                        message += " Because replace was true, this mode's existing paint was cleared.";
                    messages.push_back(message);
                }
                if (!messages.empty())
                    result["info_messages"] = messages;

                return result;
            });
        }
    });

    register_tool({
        "clear_object_paint",
        "Reset a paint annotation on an object back to unpainted -- the equivalent of the paint "
        "gizmo's 'Remove all' button. mode picks which annotation; omit it to clear all four, "
        "which paint_object cannot do in one call. Clearing is instance-independent: paint lives "
        "on the volume, so there is no instance_id here.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
                {"volume_id", {
                    {"type", "integer"},
                    {"minimum", -1},
                    {"description", "Part index within the object (0-based); omit, or pass -1, for every part"}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", {"color", "support", "seam", "fuzzy_skin"}},
                    {"description", "Which annotation to clear; omit to clear all four"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([params]() -> nlohmann::json {
                Plater*     plater = wxGetApp().plater();
                PaintTarget target;
                std::string error;
                // No instance: clearing resets the annotation on the volume, which every instance
                // shares, and this response carries no coordinate. Validating instance_id here
                // rejected {object_id: 0, instance_id: 1} on a single-instance object over a value
                // that could not have changed the outcome.
                if (!resolve_paint_target(params, target, error,
                                          PaintTargetNeeds{/*volumes=*/true, /*instance=*/false}))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                std::vector<PaintMode> modes;
                if (!resolve_paint_modes(params, modes, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                // Asked before the first write, because a snapshot has to precede the mutation it
                // protects and this is the only order in which "will anything change" can still be
                // answered. has_volume_paint is the same emptiness test clear_volume_paint makes,
                // so the two cannot disagree.
                bool will_change = false;
                for (PaintMode mode : modes)
                    for (Slic3r::ModelVolume* mv : target.volumes)
                        if (has_volume_paint(*mv, mode))
                            will_change = true;

                // Snapshot before the first write, so one undo restores every mode this call
                // cleared -- but only when there is a write to protect. Plater::priv::take_snapshot
                // refreshes dirty_state from the undo stack (Plater.cpp:14316), so an unconditional
                // one put an undo entry that restores nothing into the user's history and marked
                // the project dirty for a call that changed nothing.
                // Named per mode, the same reason paint_object is: a clear-all call would otherwise
                // leave four identical "Clear Object Paint" entries in the undo menu.
                if (will_change)
                    plater->take_snapshot(_u8L("Clear Object Paint") + " (" +
                                          (modes.size() == 1 ? paint_mode_name(modes.front()) : "all") + ")");

                nlohmann::json cleared = nlohmann::json::array();
                bool           changed = false;
                for (PaintMode mode : modes) {
                    // A subset reference into `volumes` below, not a second listing of the volumes
                    // themselves.
                    nlohmann::json cleared_volume_ids = nlohmann::json::array();
                    for (std::size_t i = 0; i < target.volumes.size(); ++i)
                        if (clear_volume_paint(*target.volumes[i], mode))
                            cleared_volume_ids.push_back(target.volume_ids[i]);
                    changed |= !cleared_volume_ids.empty();
                    cleared.push_back({{"mode", paint_mode_name(mode)},
                                       {"volumes_cleared", int(cleared_volume_ids.size())},
                                       {"cleared_volume_ids", cleared_volume_ids}});
                }

                // clear_volume_paint never moves a vertex, so refresh_after_paint's constraint holds
                // here exactly as it does for paint_object: reuse it rather than reimplement the GUI
                // bookkeeping it owes (object-list refresh, every instance's plate notified, reslice).
                // Skipped when nothing was written, for the same reason the snapshot is: a call that
                // changed nothing has no reason to reschedule the background process.
                if (changed)
                    refresh_after_paint(target);

                // Same set of volumes for every mode this call touched, so it is reported once
                // rather than repeated identically in each entry of `cleared` above. Built after
                // the clearing loop so `modes` reflects the result, not the pre-clear state: an
                // agent can confirm the clear from this response alone, without a second
                // get_object_paint call. The same per-volume shape paint_object and
                // get_object_paint report `volumes` in (volume_id, name, original_facets,
                // bounding_box, modes) -- one concept, one shape across the feature.
                nlohmann::json volumes = nlohmann::json::array();
                for (std::size_t i = 0; i < target.volumes.size(); ++i) {
                    Slic3r::ModelVolume* mv = target.volumes[i];
                    nlohmann::json by_mode = nlohmann::json::object();
                    for (PaintMode mode : modes)
                        by_mode[paint_mode_name(mode)] = painted_json(*mv, mode);
                    volumes.push_back({
                        {"volume_id", target.volume_ids[i]},
                        {"name", mv->name},
                        {"original_facets", int(mv->mesh().its.indices.size())},
                        // No instance_id parameter on this tool, so always instance 0's -- the
                        // same convention set_brim_ears uses for the same reason.
                        {"bounding_box", bbox_json(mv->mesh().transformed_bounding_box(
                            volume_to_plate(*target.object, *mv, target.instance_idx)))},
                        {"modes", by_mode}
                    });
                }

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"volumes", volumes},
                    {"cleared", cleared},
                    {"annotation_changed", changed},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
                if (!changed)
                    result["info_messages"] = std::vector<std::string>{
                        "Nothing was cleared: the requested annotation(s) were already empty."};
                return result;
            });
        }
    });

    register_tool({
        "set_brim_ears",
        "Place brim ears on an object -- the small tabs the brim adds at chosen points. Brim ears "
        "are NOT facet paint: they are points on the object (ModelObject::brim_points), so they "
        "have their own tool. Positions are PLATE millimetres, the same frame get_object_info "
        "reports its bounding_box in -- but for the numbers, use THIS call's own bounding_box in "
        "the response (or get_object_paint's), not get_object_info's: that one is a looser box "
        "(untransformed-AABB corners, unioned over every instance) and only matches this tool's "
        "for a single unrotated instance. Only x and y matter, because an ear always sits on the "
        "bottom of the object. Brim ears are stored per object, not per instance, and slicing "
        "resolves them through instance 0 only, so instance_id must be 0 (or omitted), and this "
        "response's bounding_box is always instance 0's regardless. Pass an empty points array "
        "with append left false to remove them all. They only produce brim unless brim_type is "
        "'painted'.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
                {"instance_id", {
                    {"type", "integer"},
                    {"description", "Must be 0 (or omitted): brim ears are object-level data and "
                                    "slicing resolves them through instance 0 only"}
                }},
                {"points", {
                    {"type", "array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"x", {{"type", "number"}}},
                            {"y", {{"type", "number"}}},
                            {"radius", {{"type", "number"}, {"description", "Ear radius in mm, 0.1 to 100"}}}
                        }},
                        {"required", {"x", "y"}}
                    }},
                    {"description", "Ear positions in plate mm. An empty array removes every ear, "
                                    "when append is left false."}
                }},
                {"radius", {
                    {"type", "number"},
                    {"description", "Default radius for points that do not carry one (default 5.0 mm)"}
                }},
                {"append", {
                    {"type", "boolean"},
                    {"description", "false (default) replaces the object's ears; true adds to them"}
                }}
            }},
            {"required", {"object_id", "points"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([params]() -> nlohmann::json {
                Plater*     plater = wxGetApp().plater();
                PaintTarget target;
                std::string error;
                // No volumes: brim ears are BrimPoints on the ModelObject, not facets on a volume,
                // so this tool neither declares volume_id in its schema nor needs the object to
                // own a model part. Resolving them anyway refused an object with no model part
                // ("Object N has no model parts to paint", from a tool that paints nothing) and
                // read an undeclared volume_id, which could reject the call outright when it named
                // a modifier.
                if (!resolve_paint_target(params, target, error, PaintTargetNeeds{/*volumes=*/false, /*instance=*/true}))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                // brim_points is object-level data. Brim.cpp:368-374 hardcodes instances[0]'s
                // transformation when it reads brim_points back for slicing, so a write through
                // any other instance's frame would store a point that slicing reinterprets
                // through instance 0 -- printed somewhere else, or dropped by the world-z>0 test
                // -- while this same call's response (built through that other instance's frame)
                // would still read back looking correct. There is no correct non-zero frame here,
                // so this is rejected outright rather than accepted with a caveat.
                if (target.instance_idx != 0)
                    return nlohmann::json{{"status", "error"},
                                          {"message", "brim ears are stored per object, not per "
                                                      "instance, and slicing resolves them through "
                                                      "instance 0, so only instance_id: 0 is "
                                                      "meaningful here"}};

                // params["points"] would be UB on a missing key (const operator[] asserts
                // find != end(), and NDEBUG compiles that assert out in every non-Debug config
                // this project ships) rather than throwing something the dispatcher could catch.
                // Nothing validates `required` server-side, so this guard is load-bearing, not
                // belt-and-suspenders.
                if (!params.contains("points") || !params["points"].is_array())
                    return nlohmann::json{{"status", "error"},
                                          {"message", "points must be an array of {x, y, radius?}"}};

                // The gizmo's own bounds (GLGizmoBrimEars.cpp:18-19). Its default radius is derived
                // from the initial layer line width; 5.0 mm is a printable stand-in for a tool that
                // has no nozzle context of its own to lean on and states its default in the schema.
                constexpr double k_radius_min = 0.1;
                constexpr double k_radius_max = 100.0;
                double default_radius = 5.0;
                if (params.contains("radius") &&
                    !parse_number_field(params["radius"], "radius", default_radius, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                Slic3r::BrimPoints points;
                for (const nlohmann::json& entry : params["points"]) {
                    if (!entry.is_object() || !entry.contains("x") || !entry.contains("y"))
                        return nlohmann::json{{"status", "error"},
                                              {"message", "every point needs x and y in plate millimetres"}};
                    double x = 0.0, y = 0.0;
                    if (!parse_number_field(entry["x"], "x", x, error) ||
                        !parse_number_field(entry["y"], "y", y, error))
                        return nlohmann::json{{"status", "error"}, {"message", error}};
                    // Brim.cpp:385 scales a stored ear into an int32_t (`scale_(pos.x())`), so a
                    // coordinate past 2^31 microns overflows it during slicing -- undefined
                    // behaviour reached from a plain MCP call. parse_number_field already refuses
                    // NaN and infinity; this is the magnitude half of the same guard. The bound is
                    // below the 2147.48 mm hard limit so the arithmetic never reaches it, and it is
                    // far outside any real build plate, so it rejects only mistakes (microns passed
                    // where millimetres were meant is the usual one).
                    constexpr double k_brim_xy_max = 2000.0;
                    if (std::abs(x) > k_brim_xy_max || std::abs(y) > k_brim_xy_max)
                        return nlohmann::json{{"status", "error"},
                                              {"message", "brim ear x/y must be within +/-" +
                                                          std::to_string(k_brim_xy_max) +
                                                          " mm of the plate origin; got x=" +
                                                          std::to_string(x) + ", y=" + std::to_string(y)}};
                    double radius = default_radius;
                    if (entry.contains("radius") && !parse_number_field(entry["radius"], "radius", radius, error))
                        return nlohmann::json{{"status", "error"}, {"message", error}};
                    if (radius < k_radius_min || radius > k_radius_max)
                        return nlohmann::json{{"status", "error"},
                                              {"message", "radius " + std::to_string(radius) + " out of range " +
                                                          std::to_string(k_radius_min) + ".." +
                                                          std::to_string(k_radius_max)}};
                    // An ear always sits on the underside: brim_point_to_object pins world z to
                    // -0.0001 and converts to object-local, exactly what GLGizmoBrimEars.cpp
                    // (~395-397) does on a click, and Brim.cpp:373-374 skips any stored point
                    // whose world z is above 0.
                    const Slic3r::Vec3f local = brim_point_to_object(*target.object, x, y, target.instance_idx);
                    points.emplace_back(local, float(radius));
                }

                // append must come through parse_boolean_param, not a typed json::value default:
                // a client with a stale cached schema sends "true"/"false" as a string, and
                // json::value<bool> throws type_error.302 on that instead of reading it.
                bool append = false;
                if (params.contains("append") && !parse_boolean_param(params["append"], append))
                    return nlohmann::json{{"status", "error"}, {"message", "append must be true or false"}};

                plater->take_snapshot(_u8L("Set Brim Ears"));
                if (!append)
                    target.object->brim_points.clear();
                target.object->brim_points.insert(target.object->brim_points.end(), points.begin(), points.end());

                // Direct field mutation, not a paint annotation write: nothing else marks the
                // project dirty for this change, so it is done explicitly here, the same way
                // GLGizmoBrimEars::update_model_object does right after the same assignment.
                plater->set_plater_dirty(true);
                // brim_points carries no vertex and is not part of any mesh or convex hull, so
                // this write leaves geometry untouched -- refresh_after_paint's one precondition
                // (see its docblock above) -- and reusing it here covers the object-list refresh,
                // every instance's plate notification and the reslice reschedule the gizmo's own
                // update_model_object performs, without a second copy of that bookkeeping.
                refresh_after_paint(target);

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"coordinate_frame", "plate"},
                    {"instance_id", int(target.instance_idx)},
                    // The same field name and shape paint_object and get_object_paint report
                    // (bbox_json of a plate-frame BoundingBoxf3), so an agent placing ears near an
                    // edge reads it from the box these tools actually use, not from
                    // get_object_info's looser one (untransformed-AABB corners, unioned over every
                    // instance). Always instance 0's, the same instance brim_points resolve
                    // through -- target.instance_idx is already guaranteed 0 above.
                    {"bounding_box", bbox_json(object_plate_bbox(*target.object, target.instance_idx))},
                    {"brim_ear_count", int(target.object->brim_points.size())},
                    {"brim_ears", brim_ears_json(*target.object, target.instance_idx)},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Painted ears are only produced when brim_type is btPainted (Brim.cpp:449 and
                // Brim.cpp:530), so say so rather than let the caller slice and find no brim.
                const Slic3r::DynamicPrintConfig& global =
                    wxGetApp().preset_bundle->prints.get_edited_preset().config;
                const Slic3r::DynamicPrintConfig& object_cfg = target.object->config.get();
                const Slic3r::ConfigOption* brim_type_option =
                    effective_print_option(object_cfg, global, "brim_type");
                const Slic3r::BrimType brim_type = brim_type_option
                    ? static_cast<Slic3r::BrimType>(brim_type_option->getInt())
                    : Slic3r::BrimType::btAutoBrim;
                if (brim_type != Slic3r::BrimType::btPainted && !target.object->brim_points.empty())
                    result["info_messages"] = std::vector<std::string>{
                        "Brim ears are placed but will not be printed while brim_type is not 'painted'. "
                        "Set brim_type to 'painted' with apply_config or set_object_config."};

                return result;
            });
        }
    });

    register_tool({
        "get_object_paint",
        "Read what is currently painted on an object: per-volume facet counts and surface "
        "coverage for each of the four paint modes (color, support, seam, fuzzy_skin), plus its "
        "brim ears. Coordinates are PLATE millimetres, the same frame get_object_info reports "
        "its bounding_box in. Use it to verify a paint_object call did what you asked. "
        "coverage_percent is how much of the surface a state covers -- use that. Do not divide "
        "facet_count by original_facets: facet_count counts the leaf triangles a paint stroke "
        "subdivided the mesh into, so on a painted volume it can exceed the original count.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"description", "Object index (0-based)"}
                }},
                {"volume_id", {
                    {"type", "integer"},
                    {"minimum", -1},
                    {"description", "Part index within the object (0-based); omit, or pass -1, for every part"}
                }},
                {"instance_id", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"description", "Which instance's transform defines plate coordinates (default 0). "
                                    "Paint is shared by every instance. Brim ears are reported through "
                                    "instance 0 regardless of this value (see brim_ears_instance_id in "
                                    "the response): slicing resolves brim_points through instance 0 "
                                    "only, unlike paint."}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", {"color", "support", "seam", "fuzzy_skin"}},
                    {"description", "Omit to report all four modes"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([params]() -> nlohmann::json {
                PaintTarget target;
                std::string error;
                if (!resolve_paint_target(params, target, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                std::vector<PaintMode> modes;
                if (!resolve_paint_modes(params, modes, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                nlohmann::json        volumes = nlohmann::json::array();
                // Merged as the per-volume boxes are computed rather than by a second pass through
                // target_plate_bbox, which would walk every vertex of every volume twice.
                Slic3r::BoundingBoxf3 object_bbox;
                int                   original_facets_total = 0;
                for (std::size_t i = 0; i < target.volumes.size(); ++i) {
                    Slic3r::ModelVolume* mv = target.volumes[i];
                    nlohmann::json by_mode = nlohmann::json::object();
                    for (PaintMode mode : modes)
                        by_mode[paint_mode_name(mode)] = painted_json(*mv, mode);
                    const Slic3r::BoundingBoxf3 volume_bbox = mv->mesh().transformed_bounding_box(
                        volume_to_plate(*target.object, *mv, target.instance_idx));
                    object_bbox.merge(volume_bbox);
                    original_facets_total += int(mv->mesh().its.indices.size());
                    volumes.push_back({
                        {"volume_id", target.volume_ids[i]},
                        {"name", mv->name},
                        // Original triangles. Deliberately not named facets_total: the per-state
                        // facet_count is a count of leaf triangles, so the two are not a part and
                        // a whole and dividing them can exceed 100%. coverage_percent is the ratio.
                        {"original_facets", int(mv->mesh().its.indices.size())},
                        {"bounding_box", bbox_json(volume_bbox)},
                        {"modes", by_mode}
                    });
                }

                // brim_points is object-level data that Brim.cpp resolves through instance 0 only
                // when slicing (unlike paint, which every instance shares in its own frame), so it
                // is always reported through instance 0 here, independent of instance_id -- which
                // instance the paint volumes/bounding_box above are read through. Named explicitly
                // in the response, not left implicit, so a caller reading a non-zero instance_id
                // does not assume brim_ears shares that frame too. set_brim_ears enforces the same
                // instance 0 rule on the write side, so the two agree.
                return nlohmann::json{
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"coordinate_frame", "plate"},
                    {"instance_id", int(target.instance_idx)},
                    {"bounding_box", bbox_json(object_bbox)},
                    // The same sum paint_object reports under the same name, so a caller comparing
                    // a write against a read is comparing two fields that mean the same thing.
                    {"original_facets_total", original_facets_total},
                    {"volumes", volumes},
                    {"brim_ears_instance_id", 0},
                    {"brim_ears", brim_ears_json(*target.object, 0)}
                };
            });
        }
    });
}
