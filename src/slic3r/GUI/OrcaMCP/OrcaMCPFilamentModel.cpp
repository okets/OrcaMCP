#include "OrcaMCPFilamentModel.hpp"

#include <algorithm>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

int own_filament_of(const ModelVolume& volume)
{
    const ConfigOption* opt = volume.config.option("extruder");
    return opt ? opt->getInt() : 0;
}

// The volumes that lay down filament: the only ones whose "extruder" means anything, and the
// only ones PartPlate::get_extruders counts (via ModelVolume::get_extruders).
bool prints_filament(const ModelVolume& volume)
{
    return volume.is_model_part() || volume.is_modifier();
}

void sort_unique(std::vector<int>& slots)
{
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
}

} // namespace

std::string volume_type_name(ModelVolumeType type)
{
    switch (type) {
    case ModelVolumeType::MODEL_PART:         return "part";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::NEGATIVE_VOLUME:    return "negative_volume";
    case ModelVolumeType::SUPPORT_BLOCKER:    return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER:   return "support_enforcer";
    default:                                  return "invalid";
    }
}

std::vector<VolumeFilament> describe_volume_filaments(const ModelObject& object)
{
    std::vector<VolumeFilament> out;
    out.reserve(object.volumes.size());
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume& v = *object.volumes[i];
        VolumeFilament f;
        f.volume_id          = int(i);
        f.name               = v.name;
        f.type               = volume_type_name(v.type());
        f.own_filament       = own_filament_of(v);
        f.effective_filament = v.extruder_id();
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<int> effective_object_filaments(const ModelObject& object)
{
    std::vector<int> slots;
    // ModelVolume::get_extruders is empty for volumes that print nothing and already folds in
    // painted facets, so this is exactly the per-volume half of PartPlate::get_extruders.
    for (const ModelVolume* v : object.volumes) {
        const std::vector<int> vs = v->get_extruders();
        slots.insert(slots.end(), vs.begin(), vs.end());
    }
    for (const auto& range : object.layer_config_ranges) {
        const ConfigOption* opt = range.second.option("extruder");
        if (opt && opt->getInt() > 0)
            slots.push_back(opt->getInt());
    }
    sort_unique(slots);
    return slots;
}

int volume_filament_override_count(const ModelObject& object)
{
    int count = 0;
    for (const ModelVolume* v : object.volumes)
        if (prints_filament(*v) && own_filament_of(*v) > 0)
            ++count;
    return count;
}

std::vector<ClearedOverride> clear_volume_filament_overrides(ModelObject& object, bool include_modifiers)
{
    std::vector<ClearedOverride> cleared;
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        ModelVolume& v = *object.volumes[i];
        const bool eligible = v.is_model_part() || (include_modifiers && v.is_modifier());
        if (!eligible || !v.config.has("extruder"))
            continue;
        ClearedOverride c;
        c.volume_id    = int(i);
        c.name         = v.name;
        c.type         = volume_type_name(v.type());
        c.was_filament = own_filament_of(v);
        v.config.erase("extruder");
        cleared.push_back(std::move(c));
    }
    return cleared;
}

std::vector<int> other_volume_filaments(const ModelObject& object, int object_filament)
{
    std::vector<int> slots;
    for (const ModelVolume* v : object.volumes) {
        if (!prints_filament(*v))
            continue;
        const int own = own_filament_of(*v);
        if (own > 0 && own != object_filament)
            slots.push_back(own);
    }
    sort_unique(slots);
    return slots;
}

}}} // namespace Slic3r::GUI::OrcaMCP
