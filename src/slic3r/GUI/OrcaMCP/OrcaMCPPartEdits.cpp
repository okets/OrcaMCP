// src/slic3r/GUI/OrcaMCP/OrcaMCPPartEdits.cpp
#include "OrcaMCPPartEdits.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPFilamentModel.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPaintSelect.hpp"

#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

const std::vector<std::pair<std::string, ModelVolumeType>>& named_volume_types()
{
    static const std::vector<std::pair<std::string, ModelVolumeType>> types = {
        {"part", ModelVolumeType::MODEL_PART},
        {"negative_volume", ModelVolumeType::NEGATIVE_VOLUME},
        {"modifier", ModelVolumeType::PARAMETER_MODIFIER},
        {"support_blocker", ModelVolumeType::SUPPORT_BLOCKER},
        {"support_enforcer", ModelVolumeType::SUPPORT_ENFORCER},
    };
    return types;
}

std::string object_text(int object_id) { return "Object " + std::to_string(object_id); }

std::string volume_text(int object_id, int volume_id)
{
    return "Volume " + std::to_string(volume_id) + " of object " + std::to_string(object_id);
}

bool is_support_volume_type(ModelVolumeType type)
{
    return type == ModelVolumeType::SUPPORT_BLOCKER || type == ModelVolumeType::SUPPORT_ENFORCER;
}

Transform3d instance_matrix(const ModelObject& object, std::size_t instance_idx)
{
    return instance_idx < object.instances.size() ? object.instances[instance_idx]->get_matrix() : Transform3d::Identity();
}

// The highest point of instance 0's solid parts: Selection::ensure_not_below_bed keeps it above the
// bed after a part moves.
double instance_top_z(const ModelObject& object)
{
    double top = std::numeric_limits<double>::lowest();
    for (std::size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i]->is_model_part())
            top = std::max(top, volume_world_box(object, i).max.z());
    return top;
}

const std::set<std::string>& region_setting_keys()
{
    static const std::set<std::string> keys = [] {
        const t_config_option_keys all = PrintRegionConfig().keys();
        return std::set<std::string>(all.begin(), all.end());
    }();
    return keys;
}

const std::set<std::string>& object_setting_keys()
{
    static const std::set<std::string> keys = [] {
        const t_config_option_keys all = PrintObjectConfig().keys();
        return std::set<std::string>(all.begin(), all.end());
    }();
    return keys;
}

} // namespace

// ---- Volumes ----

const std::vector<std::string>& volume_type_names()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const auto& [name, type] : named_volume_types())
            out.push_back(name);
        return out;
    }();
    return names;
}

std::optional<ModelVolumeType> volume_type_from_name(const std::string& name)
{
    for (const auto& [known, type] : named_volume_types())
        if (known == name)
            return type;
    return std::nullopt;
}

std::optional<std::string> volume_id_error(int object_id, const ModelObject& object, int volume_id)
{
    if (volume_id >= 0 && std::size_t(volume_id) < object.volumes.size())
        return std::nullopt;
    return object_text(object_id) + " has no volume " + std::to_string(volume_id) + ": its volumes are 0 to " +
           std::to_string(int(object.volumes.size()) - 1) + ", as get_object_info lists them";
}

std::size_t model_part_count(const ModelObject& object)
{
    return std::size_t(std::count_if(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* v) { return v->is_model_part(); }));
}

BoundingBoxf3 volume_world_box(const ModelObject& object, std::size_t volume_idx, std::size_t instance_idx)
{
    const ModelVolume& volume = *object.volumes[volume_idx];
    return volume.mesh().transformed_bounding_box(instance_matrix(object, instance_idx) * volume.get_matrix());
}

nlohmann::json volume_rows_json(const ModelObject& object)
{
    nlohmann::json rows = nlohmann::json::array();
    for (const VolumeFilament& v : describe_volume_filaments(object)) {
        const BoundingBoxf3 box    = volume_world_box(object, std::size_t(v.volume_id));
        const Vec3d         centre = box.center();
        rows.push_back({{"volume_id", v.volume_id},
                        {"name", v.name},
                        {"type", v.type},
                        {"own_filament", v.own_filament > 0 ? nlohmann::json(v.own_filament) : nlohmann::json(nullptr)},
                        {"effective_filament", v.effective_filament},
                        {"bounding_box", bbox_json(box)},
                        {"position", {{"x", centre.x()}, {"y", centre.y()}, {"z", centre.z()}}}});
    }
    return rows;
}

nlohmann::json volume_row_json(const ModelObject& object, std::size_t volume_idx)
{
    return volume_rows_json(object).at(volume_idx);
}

// ---- Cut objects ----

std::vector<int> cut_siblings(const Model& model, int object_id)
{
    std::vector<int> siblings;
    if (object_id < 0 || std::size_t(object_id) >= model.objects.size() || !model.objects[std::size_t(object_id)]->is_cut())
        return siblings;
    // CutObjectBase::is_equal is not const: compare through a copy, as ObjectList does with its own.
    CutObjectBase cut = model.objects[std::size_t(object_id)]->cut_id;
    for (std::size_t i = 0; i < model.objects.size(); ++i)
        if (cut.is_equal(model.objects[i]->cut_id))
            siblings.push_back(int(i));
    return siblings;
}

std::string cut_refusal(int object_id, const std::vector<int>& siblings, const std::string& what)
{
    return object_text(object_id) + " is part of a cut, with objects " + listed_ids(siblings) + ", which the app keeps together: " +
           what + ". invalidate_cut_info {\"object_id\": " + std::to_string(object_id) +
           "} ends that for every piece of the cut, as the object list's Invalidate cut info does; then call this again";
}

// ---- split_object ----

bool splits_to_several_objects(const ModelObject& object)
{
    if (object.volumes.size() == 1)
        return object.volumes.front()->is_model_part() && object.volumes.front()->is_splittable();
    const auto solid = std::count_if(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* v) {
        return v->is_model_part() && v->mesh().facets_count() >= 3;
    });
    return solid > 1;
}

std::vector<int> volumes_dropped_by_split_to_objects(const ModelObject& object)
{
    std::vector<int> dropped;
    for (std::size_t i = 0; i < object.volumes.size(); ++i)
        if (!object.volumes[i]->is_model_part())
            dropped.push_back(int(i));
    return dropped;
}

std::optional<int> split_volume_of(const ModelObject& object, std::optional<int> volume_id)
{
    if (volume_id)
        return volume_id;
    if (object.volumes.size() == 1)
        return 0;
    return std::nullopt;
}

std::optional<std::string> split_refusal(int object_id, const ModelObject& object, SplitTarget target,
                                         std::optional<int> volume_id, bool keep_height_given)
{
    if (target == SplitTarget::objects) {
        if (volume_id)
            return std::string("A split to objects splits the whole object: omit volume_id (to split one part into parts, "
                               "pass to: \"parts\" with its volume_id)");
        if (splits_to_several_objects(object))
            return std::nullopt;
        if (object.volumes.size() == 1)
            return object_text(object_id) + " is one part with one shell, so a split leaves one object: there is nothing to split";
        return object_text(object_id) + " has one solid part, and a split to objects makes one object per solid part: split that "
                                        "part into parts first (to: \"parts\" with its volume_id), then split the object";
    }
    if (keep_height_given)
        return std::string("keep_height answers the question a split to objects asks about floating pieces; a split to parts "
                           "keeps every piece where it is: leave keep_height out");
    const std::optional<int> volume = split_volume_of(object, volume_id);
    if (!volume)
        return object_text(object_id) + " has " + std::to_string(object.volumes.size()) +
               " volumes: name the one to split with volume_id (get_object_components lists each part's shells)";
    if (const auto error = volume_id_error(object_id, object, *volume))
        return error;
    if (!object.volumes[std::size_t(*volume)]->is_splittable())
        return volume_text(object_id, *volume) + " is one shell: there is nothing to split";
    return std::nullopt;
}

// ---- set_volume_type ----

std::optional<std::string> volume_type_change_refusal(int object_id, const ModelObject& object, int volume_id, ModelVolumeType to)
{
    if (const auto error = volume_id_error(object_id, object, volume_id))
        return error;
    const ModelVolume& volume = *object.volumes[std::size_t(volume_id)];
    if (to != ModelVolumeType::MODEL_PART && volume.is_model_part() && model_part_count(object) == 1)
        return volume_text(object_id, volume_id) + " is the object's last solid part, whose type cannot be changed (the object "
                                                   "list refuses it too): add a part first (add_volume), or change another volume";
    if (is_support_volume_type(to) && (volume.is_text() || volume.is_svg()))
        return volume_text(object_id, volume_id) + " is a " + (volume.is_text() ? "text" : "SVG") +
               " volume, which cannot become a support blocker or enforcer: add one with add_volume instead";
    return std::nullopt;
}

// ---- delete_object with volume_id ----

std::optional<std::string> volume_delete_refusal(int object_id, const ModelObject& object, int volume_id,
                                                 const std::vector<int>& cut_siblings)
{
    if (const auto error = volume_id_error(object_id, object, volume_id))
        return error;
    const ModelVolume& volume = *object.volumes[std::size_t(volume_id)];
    if (volume.is_model_part() && model_part_count(object) == 1)
        return volume_text(object_id, volume_id) + " is the object's last solid part, which is not deleted on its own (the object "
                                                   "list refuses it too): delete_object without volume_id deletes the whole object";
    if (!cut_siblings.empty() && (volume.is_model_part() || volume.is_negative_volume()))
        return cut_refusal(object_id, cut_siblings, "its solid parts, negative volumes and connectors are not deleted on their own");
    return std::nullopt;
}

std::vector<std::string> settings_moving_to_object(const ModelObject& object, int volume_id)
{
    if (object.volumes.size() != 2 || volume_id < 0 || volume_id > 1)
        return {};
    return object.volumes[volume_id == 0 ? 1 : 0]->config.keys();
}

// ---- Transforms of one volume ----

std::optional<std::string> volume_transform_refusal(int object_id, const ModelObject& object, int volume_id,
                                                    const std::vector<int>& cut_siblings)
{
    if (const auto error = volume_id_error(object_id, object, volume_id))
        return error;
    if (object.volumes.size() == 1)
        return object_text(object_id) + " has one volume, which moves with the object: transform the whole object (omit volume_id)";
    const ModelVolume& volume = *object.volumes[std::size_t(volume_id)];
    if (!cut_siblings.empty() && (volume.is_model_part() || volume.is_cut_connector()))
        return cut_refusal(object_id, cut_siblings, "its solid parts and connectors are not moved, rotated, scaled or mirrored on their own");
    return std::nullopt;
}

void transform_volume_in_plate_frame(ModelObject& object, std::size_t volume_idx, const Transform3d& world_transform)
{
    // Selection::transform_volume_relative, world coordinates: the plate-frame transform brought into
    // the object's frame through instance 0, about the volume's own centre, on the volume's matrix.
    const Transform3d instance = instance_matrix(object, 0);
    const Vec3d       pivot    = volume_world_box(object, volume_idx).center();
    const Transform3d about    = Geometry::translation_transform(pivot) * world_transform * Geometry::translation_transform(-pivot);
    ModelVolume&      volume   = *object.volumes[volume_idx];
    volume.set_transformation(Geometry::Transformation(instance.inverse() * about * instance * volume.get_matrix()));
    object.invalidate_bounding_box();
}

void translate_volume_in_plate_frame(ModelObject& object, std::size_t volume_idx, const Vec3d& displacement)
{
    transform_volume_in_plate_frame(object, volume_idx, Geometry::translation_transform(displacement));
    // Selection::ensure_not_below_bed: a move never takes every solid part below the bed.
    if (object.volumes[volume_idx]->is_model_part()) {
        const double lift = SINKING_MIN_Z_THRESHOLD - instance_top_z(object);
        if (lift > 0.)
            transform_volume_in_plate_frame(object, volume_idx, Geometry::translation_transform(Vec3d(0., 0., lift)));
    }
}

std::vector<double> instances_lowest_z(const ModelObject& object)
{
    std::vector<double> lowest;
    for (std::size_t i = 0; i < object.instances.size(); ++i)
        lowest.push_back(instance_min_z(object, i));
    return lowest;
}

std::vector<double> drop_after_volume_change(ModelObject& object, const std::vector<double>& lowest_before, VolumeChange change)
{
    std::vector<double> dropped(object.instances.size(), 0.);
    for (std::size_t i = 0; i < object.instances.size(); ++i) {
        ModelInstance& instance = *object.instances[i];
        const double   after    = instance_min_z(object, i);
        if (!instance.auto_drop || after == std::numeric_limits<double>::max())
            continue;
        // GLCanvas3D::do_move drops a floating instance and leaves a sinking one; do_rotate and do_scale
        // also leave one that was sinking before, unless the change lifted it clear of the bed.
        const bool drop = change == VolumeChange::move ? after > SINKING_Z_THRESHOLD && after != 0.
                                                       : i < lowest_before.size() && should_drop_to_bed(lowest_before[i], after);
        if (!drop)
            continue;
        instance.set_offset(instance.get_offset() - Vec3d(0., 0., after));
        dropped[i] = after;
    }
    object.invalidate_bounding_box();
    return dropped;
}

// ---- Settings on objects and parts ----

std::optional<std::string> setting_key_refusal(const std::string& key, SettingsHolder holder)
{
    if (key == "extruder") {
        if (holder == SettingsHolder::object)
            return std::nullopt;
        return std::string("extruder: a part's filament is set with set_object_filament and volume_id");
    }
    if (region_setting_keys().count(key) > 0)
        return std::nullopt;
    if (object_setting_keys().count(key) > 0) {
        if (holder == SettingsHolder::object)
            return std::nullopt;
        return key + " is set per object, not per part: omit volume_id to set it on the object";
    }
    return key + " is not a per-object setting: it is a printer, filament or whole-print setting, never read per " +
           (holder == SettingsHolder::object ? "object" : "part") + "; apply_config sets it for the whole print";
}

std::optional<std::string> volume_settings_refusal(int object_id, int volume_id, const ModelVolume& volume)
{
    if (volume.is_model_part() || volume.is_modifier())
        return std::nullopt;
    const std::string type = volume_type_name(volume.type());
    return volume_text(object_id, volume_id) + " is a " + type +
           ": the slicer reads settings from parts and modifiers only, so a " + type +
           "'s would never be used. Set them on the object (omit volume_id), or make it a modifier (set_volume_type)";
}

// ---- assemble_objects and merge_parts ----

std::optional<std::string> assemble_refusal(const Model& model, const std::vector<int>& object_ids)
{
    std::set<int> seen;
    for (std::size_t i = 0; i < object_ids.size(); ++i) {
        const int id = object_ids[i];
        if (id < 0 || std::size_t(id) >= model.objects.size())
            return "object_ids[" + std::to_string(i) + "] is " + std::to_string(id) + ", which is not an object: there are " +
                   std::to_string(model.objects.size()) + " (0 to " + std::to_string(int(model.objects.size()) - 1) + ")";
        if (!seen.insert(id).second)
            return object_text(id) + " is listed twice in object_ids: list each object once";
    }
    if (seen.size() < 2)
        return std::string("assemble_objects takes two or more objects: object_ids has ") + std::to_string(seen.size());
    for (const int id : object_ids)
        if (const std::vector<int> siblings = cut_siblings(model, id); !siblings.empty())
            return cut_refusal(id, siblings, "the object list does not assemble a piece of a cut into another object");
    return std::nullopt;
}

std::optional<std::string> merge_parts_refusal(int object_id, const ModelObject& object)
{
    if (object.volumes.size() > 1 || (object.volumes.size() == 1 && object.volumes.front()->is_splittable()))
        return std::nullopt;
    return object_text(object_id) + " is one part with one shell: there is nothing to merge";
}

// ---- rename_object ----

std::optional<std::string> name_refusal(const std::string& name)
{
    if (name.empty())
        return std::string("new_name is empty: give the name to set");
    if (Plater::has_illegal_filename_characters(name))
        return std::string("Invalid name, the following characters are not allowed: <>:/\\|?*\"");
    return std::nullopt;
}

}}} // namespace Slic3r::GUI::OrcaMCP
