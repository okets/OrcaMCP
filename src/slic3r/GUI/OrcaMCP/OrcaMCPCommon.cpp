#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPlateUtils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

bool is_hex_color(const std::string& value, bool allow_alpha)
{
    if (value.size() != 7 && !(allow_alpha && value.size() == 9))
        return false;
    if (value.front() != '#')
        return false;
    for (size_t i = 1; i < value.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            return false;
    return true;
}

bool color_changed(const std::string& before, const std::string& after)
{
    if (before == after)
        return false;
    // Only hex spellings of the same colour are interchangeable; anything else is compared as text.
    if (!is_hex_color(before, /*allow_alpha=*/true) || !is_hex_color(after, /*allow_alpha=*/true))
        return true;
    if (before.size() != after.size())
        return true; // "#RRGGBB" and "#RRGGBBAA" are not the same value
    for (size_t i = 0; i < before.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(before[i])) != std::tolower(static_cast<unsigned char>(after[i])))
            return true;
    return false;
}

bool parse_integer_param(const nlohmann::json& value, int& out)
{
    if (value.is_number_integer()) {
        out = value.get<int>();
        return true;
    }
    if (value.is_number_float()) {
        const double d = value.get<double>();
        if (d != std::floor(d) || std::abs(d) > 1e9)
            return false;
        out = int(d);
        return true;
    }
    if (value.is_string()) {
        const std::string str = value.get<std::string>();
        try {
            size_t    pos    = 0;
            const int parsed = std::stoi(str, &pos);
            if (pos != str.size())
                return false;
            out = parsed;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

bool parse_double_param(const nlohmann::json& value, double& out)
{
    if (value.is_number()) {
        const double parsed = value.get<double>();
        // Belt and braces: a JSON number literal cannot spell NaN or Infinity, so this can only
        // fire for a value built by something other than parsing the wire text. The string branch
        // below rejects the same spellings, which std::stod does accept.
        if (!std::isfinite(parsed))
            return false;
        out = parsed;
        return true;
    }
    if (value.is_string()) {
        const std::string str = value.get<std::string>();
        // Refused before std::stod sees it: "0x10" is a fully-consumed, finite parse of 16, and
        // "0x1p4" of 16 as well, so neither pos nor isfinite below can tell the caller's mistake
        // apart from a deliberate value. No decimal spelling of a number contains an 'x'.
        if (str.find('x') != std::string::npos || str.find('X') != std::string::npos)
            return false;
        try {
            size_t       pos    = 0;
            const double parsed = std::stod(str, &pos);
            // std::stod accepts "nan" and "inf" (any case, either sign) as valid, fully-consumed
            // parses; isfinite is what rejects them.
            if (pos == str.size() && std::isfinite(parsed)) {
                out = parsed;
                return true;
            }
        } catch (const std::exception&) {
            // out of range, or nothing numeric at all -- both are "not a number" to the caller
        }
    }
    return false;
}

bool parse_boolean_param(const nlohmann::json& value, bool& out)
{
    if (value.is_boolean()) {
        out = value.get<bool>();
        return true;
    }
    // 0/1 and "true"/"false"/"1"/"0" only: anything else is a caller mistake worth reporting rather
    // than a value to guess at.
    int as_int = 0;
    if (value.is_number() && parse_integer_param(value, as_int) && (as_int == 0 || as_int == 1)) {
        out = as_int == 1;
        return true;
    }
    if (value.is_string()) {
        const std::string str = value.get<std::string>();
        if (str == "true" || str == "1") {
            out = true;
            return true;
        }
        if (str == "false" || str == "0") {
            out = false;
            return true;
        }
    }
    return false;
}

bool object_within_plate(const BoundingBoxf3& object_bbox, const BoundingBoxf3& plate_box, double z_tolerance)
{
    return object_bbox.min.x() >= plate_box.min.x() &&
           object_bbox.min.y() >= plate_box.min.y() &&
           object_bbox.max.x() <= plate_box.max.x() &&
           object_bbox.max.y() <= plate_box.max.y() &&
           object_bbox.min.z() >= -z_tolerance;
}

void report_placement(nlohmann::json& result, int object_id)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    Model& model = plater->model();
    if (object_id < 0 || object_id >= int(model.objects.size()))
        return;

    PartPlateList& plate_list = plater->get_partplate_list();
    ModelObject*   object     = model.objects[object_id];

    // Which plate holds instance 0. find_instance answers from the plate's own instance list, which
    // is what get_scene_info reports from, so the two agree by construction. Testing against the
    // *selected* plate instead would answer about a plate the object is not on: with plate 1
    // selected, an object sitting correctly on plate 4 reads as outside the printable area.
    const int  plate_index = plate_list.find_instance(object_id, 0);
    PartPlate* plate       = plate_index >= 0 ? plate_list.get_plate(plate_index) : nullptr;

    result["plate_index"] = plate_index >= 0 ? nlohmann::json(plate_index) : nlohmann::json(nullptr);

    const BoundingBoxf3 object_bbox = object->bounding_box_approx();
    const bool on_bed = plate != nullptr && object_within_plate(object_bbox, plate->get_plate_box());
    result["on_bed"]  = on_bed;
    if (!on_bed)
        result["placement_warning"] = plate == nullptr
            ? "Object is not on any plate"
            : "Object positioned outside the printable area of plate " + std::to_string(plate_index);
    else
        result.erase("placement_warning");
}

void rehome_and_report_placement(nlohmann::json& result, int object_id)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    Model& model = plater->model();
    if (object_id < 0 || object_id >= int(model.objects.size()))
        return;

    // Re-home first: a transform can have carried the object onto a different plate, and the plate
    // lists only learn that from notify_instance_update. report_placement then reads the result.
    PartPlateList& plate_list = plater->get_partplate_list();
    ModelObject*   object     = model.objects[object_id];
    for (size_t i = 0; i < object->instances.size(); ++i)
        plate_list.notify_instance_update(object_id, int(i), /*is_new=*/true);

    report_placement(result, object_id);
}

void transform_instances_in_plate_frame(ModelObject& object, const Transform3d& world_transform)
{
    for (size_t i = 0; i < object.instances.size(); ++i) {
        ModelInstance* instance = object.instances[i];
        if (instance == nullptr)
            continue;

        // The pivot keeps the operation in place: composing the transform about the world origin
        // instead would fling an object standing at x=430 across the bed. instance_bounding_box is
        // read before this instance is touched, and only this instance is touched, so the pivot is
        // always the pre-transform centre. An object with no model-part volumes has no bounding box
        // at all (min = +inf), and center() on that is not a number -- fall back to the instance's
        // own origin rather than writing a NaN matrix.
        const BoundingBoxf3 bbox  = object.instance_bounding_box(i);
        const Vec3d         pivot = bbox.defined ? bbox.center() : instance->get_offset();

        const Transform3d about_pivot = Geometry::translation_transform(pivot) * world_transform *
                                        Geometry::translation_transform(-pivot);

        // Left-multiplied, so `world_transform` is read in plate axes; right-multiplying would read
        // it in the instance's own axes, which is the bug this function exists to avoid.
        Geometry::Transformation transformation;
        transformation.set_matrix(about_pivot * instance->get_transformation().get_matrix());
        instance->set_transformation(transformation);
    }
    object.invalidate_bounding_box();
}

// Helper to get active warnings as JSON object (always includes count, even if 0)
nlohmann::json model_object_summary_json(const ModelObject& object, int object_index)
{
    const BoundingBoxf3 bbox   = object.bounding_box_approx();
    const Vec3d         center = bbox.center();
    const Vec3d         size   = bbox.size();

    nlohmann::json summary = {
        {"id", std::to_string(object.id().id)},
        {"name", object.name},
        {"object_index", object_index},
        {"instance_count", static_cast<int>(object.instances.size())},
        {"volume_count", static_cast<int>(object.volumes.size())},
        // Position is the bounding-box centre, which stays accurate after any transform.
        {"position", {{"x", center.x()}, {"y", center.y()}, {"z", center.z()}}},
        {"bounding_box",
         {{"size_x", size.x()},
          {"size_y", size.y()},
          {"size_z", size.z()},
          {"min", {{"x", bbox.min.x()}, {"y", bbox.min.y()}, {"z", bbox.min.z()}}},
          {"max", {{"x", bbox.max.x()}, {"y", bbox.max.y()}, {"z", bbox.max.z()}}}}}};

    // The first (primary) instance's transform. rotation_degrees reflects the UI/initial rotation
    // only: MCP rotations are applied to the geometry.
    if (!object.instances.empty()) {
        const ModelInstance& instance = *object.instances.front();
        const Vec3d          rotation = instance.get_rotation();
        const Vec3d          scale    = instance.get_scaling_factor();
        summary["rotation_degrees"]   = {{"x", Geometry::rad2deg(rotation.x())},
                                         {"y", Geometry::rad2deg(rotation.y())},
                                         {"z", Geometry::rad2deg(rotation.z())}};
        summary["scale"]              = {{"x", scale.x()}, {"y", scale.y()}, {"z", scale.z()}};
    }
    return summary;
}

nlohmann::json get_active_warnings_json(Plater* plater) {
    nlohmann::json result;
    nlohmann::json warnings_array = nlohmann::json::array();

    if (plater) {
        auto* notification_manager = plater->get_notification_manager();
        if (notification_manager) {
            auto warnings = notification_manager->get_active_warnings();
            for (const auto& warning : warnings) {
                warnings_array.push_back({
                    {"level", warning.level},
                    {"message", warning.message},
                    {"type", warning.type}
                });
            }
        }
    }

    result["count"] = warnings_array.size();
    result["warnings"] = warnings_array;
    return result;
}

// Helper to add turntable preview to result if requested
void add_turntable_preview_if_requested(nlohmann::json& result, bool include_preview,
                                         int view_count, int resolution) {
    if (!include_preview) return;

    Plater* plater = wxGetApp().plater();
    int plate_index = plater->get_partplate_list().get_curr_plate_index();

    nlohmann::json preview = OrcaMCPPlateUtils::CaptureTurntablePreview(plate_index, view_count, resolution);
    if (preview.contains("preview_path")) {
        result["preview_path"] = preview["preview_path"];
    }
}

}}} // namespace Slic3r::GUI::OrcaMCP
