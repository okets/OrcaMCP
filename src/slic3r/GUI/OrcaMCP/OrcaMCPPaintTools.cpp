// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPaintModel.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/Model.hpp"

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
// (the set_object_printable / set_object_filament shape), and follows the write with the GUI
// bookkeeping GLGizmoMmuSegmentation::update_model_object does -- obj_list()->update_info_items,
// notify_instance_update, EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS. That refresh belongs beside
// these helpers when the first writing tool lands, not copied into each of them.
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

} // namespace

void OrcaMCPServer::register_paint_tools()
{
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
