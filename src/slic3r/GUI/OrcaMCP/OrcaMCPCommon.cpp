#include "OrcaMCPCommon.hpp"
#include "OrcaMCPInstanceBox.hpp"
#include "OrcaMCPMeshHealth.hpp"
#include "OrcaMCPPlateUtils.hpp"
#include "OrcaMCPQuit.hpp"
#include "OrcaMCPSliceCredit.hpp"
#include "OrcaMCPUiJob.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Geometry.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

MainThreadGate& main_thread_gate()
{
    // Never destroyed: the HTTP thread can still reach it while the app object is being torn down.
    static MainThreadGate* const gate =
        new MainThreadGate([](std::function<void()> task) { wxGetApp().CallAfter(std::move(task)); });
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

bool parse_object_param(const nlohmann::json& value, nlohmann::json& out)
{
    if (value.is_object()) {
        out = value;
        return true;
    }
    if (!value.is_string())
        return false;
    const nlohmann::json parsed = nlohmann::json::parse(value.get<std::string>(), nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object())
        return false;  // also a parse failure, which comes back as a discarded value
    out = parsed;
    return true;
}

bool parse_settings_param(const nlohmann::json& value, nlohmann::json& out, std::string& error, bool with_type)
{
    const std::string shape   = with_type ? "a list of {type, key, value}" : "a list of {key, value}";
    const std::string example = with_type ? "[{\"type\": \"print\", \"key\": \"wall_loops\", \"value\": 3}]"
                                          : "[{\"key\": \"wall_loops\", \"value\": 3}]";
    const nlohmann::json list = value.is_string() ? nlohmann::json::parse(value.get<std::string>(), nullptr, /*allow_exceptions=*/false)
                                                  : value;
    if (!list.is_array()) {
        error = "settings must be " + shape + ", e.g. " + example + "; got " + value.dump();
        return false;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        const nlohmann::json& item = list[i];
        const std::string     at   = "settings[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            error = at + " must be an object {key, value}; got " + item.dump();
            return false;
        }
        if (with_type && !(item.contains("type") && item["type"].is_string())) {
            error = at + " needs a string type (print, filament, printer or project)";
            return false;
        }
        if (!(item.contains("key") && item["key"].is_string())) {
            error = at + " needs a string key";
            return false;
        }
        if (!item.contains("value")) {
            error = at + " (" + item["key"].get<std::string>() + ") needs a value";
            return false;
        }
    }
    out = list;
    return true;
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

nlohmann::json error_response(const std::string& message)
{
    return {{"status", "error"}, {"message", message}};
}

ModelObject* resolve_object_id(const nlohmann::json& params, Model& model, int& object_id, std::string& error)
{
    // Told apart from a bad one: defaulting a missing object_id to -1 and falling into the range
    // check below would report "Invalid object_id -1", which reads as a value the caller chose.
    if (!params.contains("object_id")) {
        error = "object_id is required: the 0-based object_index get_scene_info reports";
        return nullptr;
    }
    if (!parse_integer_param(params["object_id"], object_id)) {
        error = "object_id must be a whole number";
        return nullptr;
    }
    if (object_id < 0 || object_id >= int(model.objects.size())) {
        error = "Invalid object_id " + std::to_string(object_id) + ": the scene has " +
                std::to_string(model.objects.size()) + " objects";
        return nullptr;
    }
    return model.objects[std::size_t(object_id)];
}

PipelineState pipeline_state(Plater& plater, int plate_count)
{
    const BackgroundSlicingProcess& process = plater.background_process();
    const auto                      state   = process.state();
    return {plater.is_background_process_slicing(),
            state == BackgroundSlicingProcess::STATE_STARTED || state == BackgroundSlicingProcess::STATE_RUNNING,
            state == BackgroundSlicingProcess::STATE_FINISHED || state == BackgroundSlicingProcess::STATE_CANCELED,
            process.is_export_scheduled(),
            process.is_upload_scheduled(),
            plater.slice_all_plate_in_progress(),
            plate_count};
}

std::string close_open_toolbar_tool(Plater& plater)
{
    GLCanvas3D*      canvas = plater.get_view3D_canvas3D();
    GLGizmosManager& gizmos = canvas->get_gizmos_manager();
    if (gizmos.get_current_type() == GLGizmosManager::Undefined)
        return {};
    const GLGizmoBase* current = gizmos.get_current();
    std::string        name    = current != nullptr ? current->get_name(false) : std::string("a toolbar tool");
    canvas->reset_all_gizmos();
    return name;
}

bool toolbar_tool_open(Plater& plater)
{
    return plater.get_view3D_canvas3D()->get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined;
}

nlohmann::json with_closed_tool(nlohmann::json answer, const std::string& closed_tool)
{
    if (!closed_tool.empty())
        answer["closed_toolbar_tool"] = closed_tool;
    return answer;
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
    write_placement(result, instance_placements(*model.objects[object_id], object_id, plater->get_partplate_list()));
}

std::vector<InstancePlacement> instance_placements(const ModelObject& object, int object_index, PartPlateList& plates)
{
    std::vector<InstancePlacement> placements;
    for (size_t i = 0; i < object.instances.size(); ++i) {
        // The plate's own instance list, which get_scene_info reports from, so the two agree by
        // construction. Testing against the *selected* plate instead would answer about a plate the
        // instance is not on. The exact box (instance_box): the approximate one's corners sit below a
        // tilted part's lowest real point, so a part dropped onto the bed read as under it.
        const int           plate_index = plates.find_instance(object_index, int(i));
        PartPlate*          plate       = plate_index >= 0 ? plates.get_plate(plate_index) : nullptr;
        const BoundingBoxf3 box         = instance_box(object, i);
        placements.push_back({int(i), plate_index, plate != nullptr && object_within_plate(box, plate->get_plate_box()),
                              box.center()});
    }
    return placements;
}

namespace {

// Why one instance is not on the bed, as a clause: "positioned outside the printable area of plate
// 1" or "is not on any plate".
std::string off_bed_clause(const InstancePlacement& placement)
{
    return placement.plate_index < 0 ? "is not on any plate" :
                                       "positioned outside the printable area of plate " + std::to_string(placement.plate_index);
}

std::string placement_warning(const std::vector<InstancePlacement>& placements)
{
    if (placements.size() <= 1)
        return "Object " + (placements.empty() ? std::string("is not on any plate") : off_bed_clause(placements.front()));
    std::string warning;
    for (const InstancePlacement& placement : placements)
        if (!placement.on_bed)
            warning += (warning.empty() ? "Instance " : "; instance ") + std::to_string(placement.instance_id) + " " +
                       off_bed_clause(placement);
    return warning;
}

nlohmann::json plate_or_null(int plate_index) { return plate_index >= 0 ? nlohmann::json(plate_index) : nlohmann::json(nullptr); }

} // namespace

void write_placement(nlohmann::json& result, const std::vector<InstancePlacement>& placements)
{
    nlohmann::json   each = nlohmann::json::array();
    std::set<int>    plates;
    bool             on_bed = !placements.empty();
    for (const InstancePlacement& placement : placements) {
        each.push_back({{"instance_id", placement.instance_id},
                        {"plate_index", plate_or_null(placement.plate_index)},
                        {"on_bed", placement.on_bed},
                        {"position", {{"x", placement.position.x()}, {"y", placement.position.y()}, {"z", placement.position.z()}}}});
        if (placement.plate_index >= 0)
            plates.insert(placement.plate_index);
        on_bed = on_bed && placement.on_bed;
    }
    result["plate_index"]        = plate_or_null(placements.empty() ? -1 : placements.front().plate_index);
    result["plate_indices"]      = std::vector<int>(plates.begin(), plates.end());
    result["on_bed"]             = on_bed;
    result["instance_placement"] = std::move(each);
    if (on_bed)
        result.erase("placement_warning");
    else
        result["placement_warning"] = placement_warning(placements);
}

std::optional<std::string> flatten_refusal(int object_id, bool printable, size_t instances,
                                           const std::vector<int>& instances_on_locked_plates, bool job_running)
{
    const std::string object = "object " + std::to_string(object_id);
    if (job_running)
        return ui_job_busy_message("flatten_object");
    if (!printable)
        return object + " is marked not printable, and only printable objects are oriented: turn it with rotate_object instead";
    if (instances == 0)
        return object + " has no instance to orient";
    if (instances_on_locked_plates.size() == instances)
        return object + " is on a locked plate, which is never oriented: unlock the plate (set_plate_settings locked: false), or turn it with "
                        "rotate_object";
    if (!instances_on_locked_plates.empty()) {
        const bool  one = instances_on_locked_plates.size() == 1;
        std::string ids;
        for (int id : instances_on_locked_plates)
            ids += (ids.empty() ? "" : ", ") + std::to_string(id);
        return object + " has " + (one ? "instance " : "instances ") + ids +
               " on a locked plate, which the orient would move up or down with the others without turning " +
               (one ? "it: unlock its plate (set_plate_settings locked: false), or move it" :
                     "them: unlock their plate (set_plate_settings locked: false), or move them") +
               " to another plate, then call flatten_object again";
    }
    return std::nullopt;
}

std::optional<std::string> flatten_selection_refusal(int object_id, size_t instances, const std::map<int, std::set<int>>& selected)
{
    std::set<int> every_instance;
    for (size_t i = 0; i < instances; ++i)
        every_instance.insert(int(i));
    if (selected.size() == 1 && selected.begin()->first == object_id && selected.begin()->second == every_instance)
        return std::nullopt;
    return "the 3D view has not caught up with object " + std::to_string(object_id) +
           " yet, so the orient would not be this object alone, and was not started: show the Prepare tab, then call "
           "flatten_object again";
}

bool valid_scale_factors(const Vec3d& factors)
{
    return std::all_of(factors.data(), factors.data() + 3, [](double f) { return std::isfinite(f) && f > 0.0; });
}

Vec3d PlateAxes::value_or(const Vec3d& fallback) const
{
    return Vec3d(axis[0].value_or(fallback.x()), axis[1].value_or(fallback.y()), axis[2].value_or(fallback.z()));
}

namespace {
std::string not_a_number(const std::string& name, const nlohmann::json& value)
{
    return name + " must be a number, not " + value.type_name();
}

// `entry`'s `key`, when given: an object of x, y and z, read into `out`.
std::optional<std::string> read_entry_axes(const nlohmann::json& entry, const char* key, PlateAxes& out)
{
    const auto it = entry.find(key);
    if (it == entry.end())
        return std::nullopt;
    if (!it->is_object())
        return std::string(key) + " must be an object {x, y, z}, not " + it->type_name();
    return read_plate_axes(*it, std::string(key) + ".", out);
}

// One entry of transform_objects, every value it gives read and checked.
TransformEntry read_transform_entry(const nlohmann::json& t, size_t object_count)
{
    TransformEntry entry;
    const auto rejected = [&entry](std::string error) {
        entry.error = std::move(error);
        return entry;
    };
    if (!t.is_object())
        return rejected(std::string("each transform must be an object {object_id, position, rotation, scale}, not ") +
                        t.type_name());
    const auto id = t.find("object_id");
    if (id == t.end())
        return rejected("object_id is missing");
    if (!id->is_number_integer())
        return rejected(std::string("object_id must be an integer, not ") + id->type_name());
    entry.object_id = id->get<int>();
    if (entry.object_id < 0 || size_t(entry.object_id) >= object_count)
        return rejected("Invalid object_id");

    if (auto error = read_entry_axes(t, "position", entry.position))
        return rejected(std::move(*error));
    PlateAxes rotation;
    if (auto error = read_entry_axes(t, "rotation", rotation))
        return rejected(std::move(*error));
    entry.rotation = rotation.value_or(Vec3d::Zero());

    const auto scale = t.find("scale");
    if (scale != t.end()) {
        if (!scale->is_object())
            return rejected(std::string("scale must be an object {x, y, z} or {uniform}, not ") + scale->type_name());
        const auto uniform = scale->find("uniform");
        if (uniform != scale->end()) {
            if (!uniform->is_number())
                return rejected(not_a_number("scale.uniform", *uniform));
            entry.scale = Vec3d::Constant(uniform->get<double>());
        } else {
            PlateAxes factors;
            if (auto error = read_plate_axes(*scale, "scale.", factors))
                return rejected(std::move(*error));
            entry.scale = factors.value_or(Vec3d::Ones());
        }
        if (!valid_scale_factors(entry.scale))
            return rejected("Scale factors must be positive; use mirror_object to flip an axis");
    }
    return entry;
}
} // namespace

std::optional<std::string> read_plate_axes(const nlohmann::json& object, const std::string& prefix, PlateAxes& out)
{
    static const char* const names[] = {"x", "y", "z"};
    PlateAxes read;
    for (size_t i = 0; i < 3; ++i) {
        const auto it = object.find(names[i]);
        if (it == object.end())
            continue;
        if (!it->is_number())
            return not_a_number(prefix + names[i], *it);
        read.axis[i] = it->get<double>();
    }
    out = read;
    return std::nullopt;
}

std::optional<std::string> transforms_argument_error(const nlohmann::json& params)
{
    const auto it = params.find("transforms");
    if (it == params.end())
        return std::string("transforms is required: an array of {object_id, position, rotation, scale}");
    if (!it->is_array())
        return std::string("transforms must be an array of {object_id, position, rotation, scale}, not ") + it->type_name();
    return std::nullopt;
}

std::vector<TransformEntry> read_transform_entries(const nlohmann::json& transforms, size_t object_count)
{
    std::vector<TransformEntry> entries;
    for (const nlohmann::json& t : transforms)
        entries.push_back(read_transform_entry(t, object_count));
    return entries;
}

void rehome_and_report_placement(nlohmann::json& result, int object_id, bool moved)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    Model& model = plater->model();
    if (object_id < 0 || object_id >= int(model.objects.size()))
        return;
    result["changed"] = moved;
    if (!moved) {
        report_placement(result, object_id);
        return;
    }

    // Re-home first: a transform can have carried the object onto a different plate, and the plate
    // lists only learn that from notify_instance_update. report_placement then reads the result.
    plater->get_partplate_list().notify_object_instances_update(object_id, /*is_new=*/true);

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

} // namespace

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

void transform_instances_in_plate_frame(ModelObject& object, const Transform3d& world_transform)
{
    // Each instance turns about its own pre-transform centre: its box (instance_box) is read before
    // this instance is touched, and only this instance is touched.
    for (size_t i = 0; i < object.instances.size(); ++i)
        if (ModelInstance* instance = object.instances[i])
            transform_instance_about_box(*instance, world_transform, instance_box(object, i));
    object.invalidate_bounding_box();
}

bool should_drop_to_bed(double min_z_before, double min_z_after)
{
    // GLCanvas3D::do_scale's condition, word for word: "leave sinking instances as sinking".
    return (min_z_before >= SINKING_Z_THRESHOLD || min_z_after > SINKING_Z_THRESHOLD) && min_z_after != 0.0;
}

void transform_instances_on_bed(ModelObject& object, const Transform3d& world_transform)
{
    // The instance's box (instance_box: one walk over the mesh unless it is cached), which gives both
    // the pivot and the lowest point before; one walk over the convex hull for the lowest point after.
    // The GUI reads the same two. An instance with no model part has no lowest point to keep, so it is
    // not dropped.
    for (size_t i = 0; i < object.instances.size(); ++i) {
        ModelInstance* instance = object.instances[i];
        if (instance == nullptr)
            continue;
        const BoundingBoxf3 before = instance_box(object, i);
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
            here.box.merge(instance_box(object, i));
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

bool printable_changes(const ModelObject& object, bool printable)
{
    return std::any_of(object.instances.begin(), object.instances.end(),
                       [printable](const ModelInstance* instance) { return instance->printable != printable; });
}

bool brim_ears_change(const std::vector<BrimPoint>& current, const std::vector<BrimPoint>& given, bool append)
{
    return append ? !given.empty() : given != current;
}

std::vector<std::string> object_overrides_to_reset(const std::vector<std::string>& object_keys)
{
    std::vector<std::string> reset;
    std::copy_if(object_keys.begin(), object_keys.end(), std::back_inserter(reset),
                 [](const std::string& key) { return key != "extruder"; });
    return reset;
}

void mark_object_plates_unsliced(PartPlateList& plates, int object_index)
{
    const Model& model = wxGetApp().model();
    if (object_index < 0 || size_t(object_index) >= model.objects.size())
        return;
    const size_t instances = model.objects[size_t(object_index)]->instances.size();
    for (int p = 0; p < plates.get_plate_count(); ++p) {
        PartPlate* plate = plates.get_plate(p);
        for (size_t i = 0; plate != nullptr && i < instances; ++i)
            if (plate->contain_instance(object_index, int(i))) {
                plate->update_slice_result_valid_state(false);
                break;
            }
    }
}

void mark_plate_unsliced(PartPlateList& plates, int plate_index)
{
    if (PartPlate* plate = plate_index >= 0 && plate_index < plates.get_plate_count() ? plates.get_plate(plate_index) : nullptr)
        plate->update_slice_result_valid_state(false);
}

std::vector<ModelObject*> objects_on_plate(PartPlate& plate)
{
    std::vector<ModelObject*> objects;
    for (ModelObject* object : plate.get_objects_on_this_plate())
        if (std::find(objects.begin(), objects.end(), object) == objects.end())
            objects.push_back(object);
    return objects;
}

int model_object_index(const ModelObject* object)
{
    return model_object_index(wxGetApp().model(), object);
}

int model_object_index(const Model& model, const ModelObject* object)
{
    if (object == nullptr)
        return -1;
    const ModelObjectPtrs& objects = model.objects;
    for (size_t i = 0; i < objects.size(); ++i)
        if (objects[i] == object || objects[i]->id() == object->id())
            return int(i);
    return -1;
}

namespace {

// One description of `object`: its identity, `box` for bounding_box and position (the box's centre,
// which stays accurate after any transform), and instance `instance_idx`'s rotation and scale --
// the instance's own, which the MCP transforms write to as the GUI's gizmos do.
nlohmann::json object_summary_json(const ModelObject& object, int object_index, const BoundingBoxf3& box, size_t instance_idx,
                                   const MeshHealth& health)
{
    const Vec3d center = box.center();
    const Vec3d size   = box.size();

    nlohmann::json summary = {
        {"name", object.name},
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
    add_object_identity(summary, object, object_index);
    add_mesh_warning(summary, health);
    return summary;
}

} // namespace

nlohmann::json model_object_summary_json(const ModelObject& object, int object_index)
{
    return model_object_summary_json(object, object_index, object_mesh_health(object));
}

nlohmann::json model_object_summary_json(const ModelObject& object, int object_index, const MeshHealth& health)
{
    return object_summary_json(object, object_index, object_world_box(object), 0, health);
}

nlohmann::json model_object_summary_json(const ModelObject& object, int object_index, const InstancesOnPlate& here)
{
    return model_object_summary_json(object, object_index, here, object_mesh_health(object));
}

nlohmann::json model_object_summary_json(const ModelObject& object, int object_index, const InstancesOnPlate& here,
                                         const MeshHealth& health)
{
    // The first copy on this plate: instance 0 may stand on another plate, turned or scaled otherwise.
    const size_t   first   = here.ids.empty() ? 0 : size_t(here.ids.front());
    nlohmann::json summary = object_summary_json(object, object_index, plate_box_of(object, here), first, health);
    summary["instances_on_plate"] = here.ids;
    return summary;
}

void add_object_identity(nlohmann::json& out, const ModelObject& object, int object_index)
{
    out["object_id"]    = object_index;
    out["object_index"] = object_index;
    out["internal_id"]  = std::to_string(object.id().id);
}

nlohmann::json plate_slicing_json(int plate_index, bool slice_result_valid, std::optional<int> percent, nlohmann::json gcode_check)
{
    return {{"plate_index", plate_index},
            {"index", plate_index},
            {"slice_result_valid", slice_result_valid},
            {"percent", percent ? nlohmann::json(*percent) : nlohmann::json(nullptr)},
            {"gcode_check", std::move(gcode_check)}};
}

PrinterSetup printer_setup()
{
    PrinterSetup setup;
    if (wxApp::GetInstance() == nullptr || wxGetApp().preset_bundle == nullptr) // no app: a unit test, the CLI
        return setup;
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    const PresetCollection& printers = bundle->printers;
    setup.default_selected           = printers.get_selected_preset().is_default;
    if (setup.default_selected)
        for (const Preset& printer : printers.get_presets())
            if (!printer.is_default && printer.is_visible) {
                setup.installed_printer = printer.name;
                break;
            }
    return setup;
}

// Helper to get active warnings as JSON object (always includes count, even if 0)
nlohmann::json get_active_warnings_json(Plater* plater) {
    nlohmann::json result;
    nlohmann::json warnings_array = nlohmann::json::array();

    // First: until a printer is selected, the app's own warnings (a validation error on the default printer's
    // settings) point at the wrong fix.
    if (auto no_printer = no_printer_message(printer_setup()))
        warnings_array.push_back({{"level", "serious_warning"}, {"message", *no_printer}, {"type", "NoPrinter"}});
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
    if (auto open_dialog = open_dialog_warning(current_modal_state()))
        warnings_array.push_back(std::move(*open_dialog));
    if (auto quit_failed = quit_failed_warning())
        warnings_array.push_back(std::move(*quit_failed));
    if (const SliceAllEndedEarly* ended = plater != nullptr ? plater->slice_all_ended_early() : nullptr)
        warnings_array.push_back({{"level", "warning"}, {"message", slice_all_ended_early_text(*ended)}, {"type", "SliceAllEndedEarly"}});
    // Told once, by the first response after the safety net cancelled a slice.
    if (auto cancelled = plater != nullptr ? plater->take_slice_cancelled_by_free() : std::nullopt)
        warnings_array.push_back({{"level", "warning"}, {"message", *cancelled}, {"type", "SliceCancelled"}});

    result["count"] = warnings_array.size();
    result["warnings"] = warnings_array;
    return result;
}

void add_warnings(nlohmann::json& active_warnings, const nlohmann::json& entries)
{
    for (const nlohmann::json& entry : entries)
        active_warnings["warnings"].push_back(entry);
    active_warnings["count"] = active_warnings["warnings"].size();
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
