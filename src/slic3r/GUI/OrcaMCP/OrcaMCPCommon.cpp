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
#include <limits>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

MainThreadGate& main_thread_gate()
{
    // Never destroyed: the HTTP thread can still reach it while the app object is being torn down.
    static MainThreadGate* const gate = new MainThreadGate(
        [](std::function<void()> task) { wxGetApp().CallAfter(std::move(task)); },
        [] { return wxGetApp().is_closing(); });
    return *gate;
}

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

    // The exact box (object_world_box): the approximate one's corners sit below a tilted part's
    // lowest real point, so a part dropped onto the bed read as under it, on_bed false.
    const BoundingBoxf3 object_bbox = object_world_box(*object);
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

namespace {

// Turns one instance by `world_transform` about the centre of `box`, its world box before the turn.
//
// The pivot keeps the operation in place: composing the transform about the world origin instead
// would fling an object standing at x=430 across the bed. An object with no model-part volumes has no
// bounding box at all (min = +inf), and center() on that is not a number -- fall back to the
// instance's own origin rather than writing a NaN matrix.
void transform_instance_about_box(ModelInstance& instance, const Transform3d& world_transform, const BoundingBoxf3& box)
{
    const Vec3d       pivot       = box.defined ? box.center() : instance.get_offset();
    const Transform3d about_pivot = Geometry::translation_transform(pivot) * world_transform *
                                    Geometry::translation_transform(-pivot);

    // Left-multiplied, so `world_transform` is read in plate axes; right-multiplying would read
    // it in the instance's own axes, which is the bug transform_instances_in_plate_frame exists to avoid.
    Geometry::Transformation transformation;
    transformation.set_matrix(about_pivot * instance.get_transformation().get_matrix());
    instance.set_transformation(transformation);
}

// The lowest point of one instance, read from the model parts' convex hulls as the GUI's drop reads
// it (ModelObject::get_instance_min_z). That function visits every hull vertex once per facet it
// belongs to, about six times over; this visits each once. A hull qhull could not build is empty,
// and the mesh stands in for it, as it does there.
double instance_min_z(const ModelObject& object, size_t instance_idx)
{
    const Transform3d instance_matrix = object.instances[instance_idx]->get_matrix();
    double            min_z           = std::numeric_limits<double>::max();
    for (const ModelVolume* volume : object.volumes) {
        if (!volume->is_model_part())
            continue;
        const TriangleMesh& hull     = volume->get_convex_hull();
        const TriangleMesh& vertices = hull.its.indices.empty() ? volume->mesh() : hull;
        const Transform3d   matrix   = instance_matrix * volume->get_matrix();
        for (const stl_vertex& v : vertices.its.vertices)
            min_z = std::min(min_z, (matrix * v.cast<double>()).z());
    }
    return min_z;
}

} // namespace

void transform_instances_in_plate_frame(ModelObject& object, const Transform3d& world_transform)
{
    // Each instance turns about its own pre-transform centre: instance_bounding_box is read before
    // this instance is touched, and only this instance is touched.
    for (size_t i = 0; i < object.instances.size(); ++i)
        if (ModelInstance* instance = object.instances[i])
            transform_instance_about_box(*instance, world_transform, object.instance_bounding_box(i));
    object.invalidate_bounding_box();
}

bool should_drop_to_bed(double min_z_before, double min_z_after)
{
    // GLCanvas3D::do_scale's condition, word for word: "leave sinking instances as sinking".
    return (min_z_before >= SINKING_Z_THRESHOLD || min_z_after > SINKING_Z_THRESHOLD) && min_z_after != 0.0;
}

void transform_instances_on_bed(ModelObject& object, const Transform3d& world_transform)
{
    // One walk over the mesh per instance, for the box that gives both the pivot and the lowest point
    // before; one over the convex hull for the lowest point after. The GUI reads the same two. An
    // instance with no model part has no lowest point to keep, so it is not dropped.
    for (size_t i = 0; i < object.instances.size(); ++i) {
        ModelInstance* instance = object.instances[i];
        if (instance == nullptr)
            continue;
        const BoundingBoxf3 before = object.instance_bounding_box(i);
        transform_instance_about_box(*instance, world_transform, before);
        if (!instance->auto_drop || !before.defined)
            continue;
        const double min_z_after = instance_min_z(object, i);
        if (should_drop_to_bed(before.min.z(), min_z_after))
            instance->set_offset(instance->get_offset() - Vec3d(0.0, 0.0, min_z_after));
    }
    object.invalidate_bounding_box();
}

const BoundingBoxf3& object_world_box(const ModelObject& object) { return object.bounding_box_exact(); }

InstancesOnPlate instances_on_plate(const ModelObject& object, const std::function<bool(int)>& holds)
{
    InstancesOnPlate here;
    for (size_t i = 0; i < object.instances.size(); ++i)
        if (holds(int(i))) {
            here.ids.push_back(int(i));
            here.box.merge(object.instance_bounding_box(i));
        }
    return here;
}

InstancesOnPlate instances_on_plate(const ModelObject& object, int object_index, PartPlate& plate)
{
    return instances_on_plate(object, [&plate, object_index](int instance) { return plate.contain_instance(object_index, instance); });
}

BoundingBoxf3 plate_box_of(const ModelObject& object, const InstancesOnPlate& here)
{
    return here.box.defined ? here.box : object_world_box(object);
}

int model_object_index(const ModelObject* object)
{
    if (object == nullptr)
        return -1;
    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    for (size_t i = 0; i < objects.size(); ++i)
        if (objects[i] == object || objects[i]->id() == object->id())
            return int(i);
    return -1;
}

namespace {

// One description of `object`: its identity, `box` for bounding_box and position (the box's centre,
// which stays accurate after any transform), and instance `instance_idx`'s rotation and scale --
// the instance's own, which the MCP transforms write to as the GUI's gizmos do.
nlohmann::json object_summary_json(const ModelObject& object, int object_index, const BoundingBoxf3& box, size_t instance_idx)
{
    const Vec3d center = box.center();
    const Vec3d size   = box.size();

    nlohmann::json summary = {
        {"id", std::to_string(object.id().id)},
        {"name", object.name},
        {"object_index", object_index},
        {"instance_count", static_cast<int>(object.instances.size())},
        {"volume_count", static_cast<int>(object.volumes.size())},
        {"position", {{"x", center.x()}, {"y", center.y()}, {"z", center.z()}}},
        {"bounding_box",
         {{"size_x", size.x()},
          {"size_y", size.y()},
          {"size_z", size.z()},
          {"min", {{"x", box.min.x()}, {"y", box.min.y()}, {"z", box.min.z()}}},
          {"max", {{"x", box.max.x()}, {"y", box.max.y()}, {"z", box.max.z()}}}}}};

    if (instance_idx < object.instances.size()) {
        const ModelInstance& instance = *object.instances[instance_idx];
        const Vec3d          rotation = instance.get_rotation();
        const Vec3d          scale    = instance.get_scaling_factor();
        summary["rotation_degrees"]   = {{"x", Geometry::rad2deg(rotation.x())},
                                         {"y", Geometry::rad2deg(rotation.y())},
                                         {"z", Geometry::rad2deg(rotation.z())}};
        summary["scale"]              = {{"x", scale.x()}, {"y", scale.y()}, {"z", scale.z()}};
    }
    return summary;
}

} // namespace

nlohmann::json model_object_summary_json(const ModelObject& object, int object_index)
{
    return object_summary_json(object, object_index, object_world_box(object), 0);
}

nlohmann::json model_object_summary_json(const ModelObject& object, int object_index, const InstancesOnPlate& here)
{
    // The first copy on this plate: instance 0 may stand on another plate, turned or scaled otherwise.
    const size_t   first   = here.ids.empty() ? 0 : size_t(here.ids.front());
    nlohmann::json summary = object_summary_json(object, object_index, plate_box_of(object, here), first);
    summary["instances_on_plate"] = here.ids;
    return summary;
}

// Helper to get active warnings as JSON object (always includes count, even if 0)
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

void add_preview_to(nlohmann::json& result, const std::function<nlohmann::json()>& capture)
{
    try {
        const nlohmann::json preview = capture();
        if (preview.contains("preview_path"))
            result["preview_path"] = preview["preview_path"];
        else
            result["preview_error"] = preview.value("error", std::string("the preview returned no image"));
    } catch (const std::exception& e) {
        result["preview_error"] = std::string("the turntable preview could not be made: ") + e.what();
    }
}

void add_turntable_preview_if_requested(nlohmann::json& result, bool include_preview, int view_count, int resolution)
{
    if (!include_preview)
        return;
    const int plate_index = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
    add_preview_to(result, [&] { return OrcaMCPPlateUtils::CaptureTurntablePreview(plate_index, view_count, resolution); });
}

}}} // namespace Slic3r::GUI::OrcaMCP
