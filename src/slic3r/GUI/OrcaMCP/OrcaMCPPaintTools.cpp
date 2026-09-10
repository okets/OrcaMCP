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
#include <cstddef>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// The volumes one call addresses, and the instance whose transform defines plate coordinates.
// Paint lives on the ModelVolume, so it applies to every instance of the object; `instance_idx`
// only decides which instance's frame the caller's coordinates are read in.
struct PaintTarget
{
    Slic3r::ModelObject*              object   = nullptr;
    std::vector<Slic3r::ModelVolume*> volumes;
    std::vector<int>                  volume_ids;
    std::size_t                       instance_idx = 0;
    int                               object_id    = -1;
};

// Main thread only. Reads object_id (required), volume_id (optional, -1 = every model part)
// and instance_id (optional, default 0).
//
// A tool that writes paint validates through here first and only then takes its undo snapshot
// (the set_object_printable / set_object_filament shape), and follows the write with
// refresh_after_paint below, which carries the GUI bookkeeping every such write owes.
bool resolve_paint_target(const nlohmann::json& params, PaintTarget& out, std::string& error)
{
    Plater*        plater = wxGetApp().plater();
    Slic3r::Model& model  = plater->model();

    // Told apart from a bad one: defaulting a missing object_id to -1 and falling into the range
    // check below would report "Invalid object_id -1", which reads as a value the caller chose.
    if (!params.contains("object_id")) {
        error = "object_id is required: pass the 0-based index of the object to paint";
        return false;
    }
    const int object_id = params.value("object_id", -1);
    if (object_id < 0 || object_id >= int(model.objects.size())) {
        error = "Invalid object_id " + std::to_string(object_id) + ": the scene has " +
                std::to_string(model.objects.size()) + " objects";
        return false;
    }
    out.object    = model.objects[std::size_t(object_id)];
    out.object_id = object_id;

    const int instance_id = params.value("instance_id", 0);
    if (instance_id < 0 || instance_id >= int(out.object->instances.size())) {
        error = "Invalid instance_id " + std::to_string(instance_id) + ": the object has " +
                std::to_string(out.object->instances.size()) + " instances";
        return false;
    }
    out.instance_idx = std::size_t(instance_id);

    const int volume_id = params.value("volume_id", -1);
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
// brim_points are stored object-local (Model.hpp:390; Brim.cpp:373 transforms them by the
// instance matrix to get a world position), so the read-back has to transform them forward.
// An out-of-range instance falls back to instance 0, the same way volume_to_plate does, so the
// two halves of one response cannot end up in different frames.
nlohmann::json brim_ears_json(const Slic3r::ModelObject& obj, std::size_t instance_idx)
{
    nlohmann::json ears = nlohmann::json::array();
    const Slic3r::Transform3d to_plate =
        obj.instances.empty()
            ? Slic3r::Transform3d::Identity()
            : obj.instances[instance_idx < obj.instances.size() ? instance_idx : 0]->get_matrix();
    for (const Slic3r::BrimPoint& point : obj.brim_points) {
        const Slic3r::Vec3d plate_pos = to_plate * point.pos.cast<double>();
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

bool read_vec3(const nlohmann::json& value, Slic3r::Vec3d& out, const char* what, std::string& error)
{
    if (!value.is_array() || value.size() != 3) {
        error = std::string(what) + " must be an array of three numbers [x, y, z] in plate millimetres";
        return false;
    }
    out = Slic3r::Vec3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
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
        out.range_from = params.contains("from") ? params["from"].get<double>() : bbox.min[row];
        out.range_to   = params.contains("to") ? params["to"].get<double>() : bbox.max[row];

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
                band.from = entry["from"].get<double>();
                band.to   = entry["to"].get<double>();
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
        out.sphere.radius = params["sphere"]["radius"].get<double>();
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
        "bounding_box and position in, not object-local coordinates. A facet belongs to the band "
        "or region containing its centroid. Paint lives on the volume, so it applies to every "
        "instance; instance_id only says whose transform reads your coordinates. Verify with "
        "get_object_paint, undo with undo, reset with clear_object_paint.",
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
            return run_on_main_thread([params]() -> nlohmann::json {
                Plater*     plater = wxGetApp().plater();
                PaintTarget target;
                std::string error;
                if (!resolve_paint_target(params, target, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                PaintMode mode = PaintMode::Color;
                if (params.contains("mode") && !parse_paint_mode(params["mode"].get<std::string>(), mode))
                    return nlohmann::json{{"status", "error"},
                                          {"message", "Unknown mode; expected color, support, seam or fuzzy_skin"}};

                PaintRequest request;
                if (!parse_paint_request(params, mode, target, request, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                // apply_facet_states rejects a mesh with no facets, and its bool cannot be told
                // apart from "the annotation already held this". Caught here, before the snapshot,
                // so the call fails instead of reporting success over a volume it never touched.
                for (std::size_t i = 0; i < target.volumes.size(); ++i)
                    if (target.volumes[i]->mesh().its.indices.empty())
                        return nlohmann::json{{"status", "error"},
                                              {"message", "volume_id " + std::to_string(target.volume_ids[i]) +
                                                          " has an empty mesh, so it has no facets to paint"}};

                // Everything is validated -- only now touch the undo stack, the way
                // set_object_filament does (OrcaMCPFilamentUtils.cpp, set_object_filament).
                // Named per mode: painting colour and then supports would otherwise leave two
                // identical entries in the undo menu with nothing to tell them apart.
                plater->take_snapshot(_u8L("Paint Object") + " (" + paint_mode_name(mode) + ")");

                // Facets the selection COVERED, which is not the same as facets given a paint:
                // `filament: 0` covers a facet and unpaints it. Named for what it counts.
                int              facets_selected   = 0;
                int              original_facets   = 0;
                int              facets_unassigned = 0;
                std::vector<int> band_counts(request.bands.size(), 0);
                bool             changed = false;

                for (Slic3r::ModelVolume* mv : target.volumes) {
                    const Slic3r::Transform3d to_plate =
                        volume_to_plate(*target.object, *mv, target.instance_idx);
                    const std::vector<Slic3r::Vec3d> centroids = facet_centroids(mv->mesh().its, to_plate);
                    original_facets += int(centroids.size());

                    FacetAssignment assignment;
                    if (request.selection == "bands")
                        assignment = assign_bands(centroids, request.axis, request.bands);
                    else if (request.selection == "box")
                        assignment = assign_box(centroids, request.box, request.state);
                    else if (request.selection == "sphere")
                        assignment = assign_sphere(centroids, request.sphere, request.state);
                    else
                        assignment = assign_all(centroids.size(), request.state);

                    facets_unassigned += assignment.unassigned;
                    for (std::size_t i = 0; i < assignment.band_counts.size() && i < band_counts.size(); ++i)
                        band_counts[i] += assignment.band_counts[i];
                    facets_selected += int(assignment.states.size()) - assignment.unassigned;

                    // False here can only mean "the annotation already held exactly this": every
                    // other reason apply_facet_states rejects a write was ruled out above.
                    changed |= apply_facet_states(*mv, mode, assignment.states, request.replace);
                }

                refresh_after_paint(target);

                nlohmann::json volumes = nlohmann::json::array();
                for (std::size_t i = 0; i < target.volumes.size(); ++i)
                    volumes.push_back({{"volume_id", target.volume_ids[i]},
                                       {"painted", painted_json(*target.volumes[i], mode)}});

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"mode", paint_mode_name(mode)},
                    {"selection", request.selection},
                    {"coordinate_frame", "plate"},
                    {"instance_id", int(target.instance_idx)},
                    {"replace", request.replace},
                    {"annotation_changed", changed},
                    // Original triangles, named as get_object_paint names them: the per-state
                    // facet_count counts leaf triangles and can exceed this, so the two are not a
                    // part and a whole. coverage_percent is the ratio to read.
                    {"original_facets", original_facets},
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
                if (facets_unassigned > 0 && request.selection != "bands")
                    messages.push_back(std::to_string(facets_unassigned) +
                                       " facets fell outside the selection and kept their previous state.");
                if (facets_selected == 0) {
                    // With replace this is not a no-op: the reset happened and the selection then
                    // wrote nothing back, so the volume came out bare. Saying only "nothing was
                    // painted" would read as "nothing happened".
                    std::string message = "Nothing was painted: no facet centroid fell inside the "
                                          "selection. Check the coordinates against get_object_info's "
                                          "bounding_box, which is in the same plate frame.";
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
                                    "Paint is shared by every instance."}
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

                std::vector<PaintMode> modes = {PaintMode::Color, PaintMode::Support,
                                                PaintMode::Seam, PaintMode::FuzzySkin};
                if (params.contains("mode")) {
                    PaintMode one = PaintMode::Color;
                    if (!parse_paint_mode(params["mode"].get<std::string>(), one))
                        return nlohmann::json{{"status", "error"},
                                              {"message", "Unknown mode; expected color, support, seam or fuzzy_skin"}};
                    modes = {one};
                }

                nlohmann::json        volumes = nlohmann::json::array();
                // Merged as the per-volume boxes are computed rather than by a second pass through
                // target_plate_bbox, which would walk every vertex of every volume twice.
                Slic3r::BoundingBoxf3 object_bbox;
                for (std::size_t i = 0; i < target.volumes.size(); ++i) {
                    Slic3r::ModelVolume* mv = target.volumes[i];
                    nlohmann::json by_mode = nlohmann::json::object();
                    for (PaintMode mode : modes)
                        by_mode[paint_mode_name(mode)] = painted_json(*mv, mode);
                    const Slic3r::BoundingBoxf3 volume_bbox = mv->mesh().transformed_bounding_box(
                        volume_to_plate(*target.object, *mv, target.instance_idx));
                    object_bbox.merge(volume_bbox);
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

                return nlohmann::json{
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"coordinate_frame", "plate"},
                    {"instance_id", int(target.instance_idx)},
                    {"bounding_box", bbox_json(object_bbox)},
                    {"volumes", volumes},
                    {"brim_ears", brim_ears_json(*target.object, target.instance_idx)}
                };
            });
        }
    });
}
