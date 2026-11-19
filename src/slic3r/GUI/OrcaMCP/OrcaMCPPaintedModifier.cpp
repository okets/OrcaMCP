// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintedModifier.cpp
#include "OrcaMCPPaintedModifier.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPaintModel.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartEdits.hpp"

#include "libslic3r/Model.hpp"

#include <algorithm>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// What a volume is, in the words the part tools name its kind by.
std::string volume_kind(const ModelVolume& volume)
{
    switch (volume.type()) {
    case ModelVolumeType::MODEL_PART:         return "a part";
    case ModelVolumeType::NEGATIVE_VOLUME:    return "a negative volume";
    case ModelVolumeType::PARAMETER_MODIFIER: return "a modifier";
    case ModelVolumeType::SUPPORT_BLOCKER:    return "a support blocker";
    case ModelVolumeType::SUPPORT_ENFORCER:   return "a support enforcer";
    case ModelVolumeType::PAINTED_MODIFIER:   return "a painted modifier";
    default:
        return volume.is_precise_seam() ? "a precise seam modifier" : "another kind of volume";
    }
}

// add_volume's shape arguments: a painted modifier shares the host's mesh, so it takes none of them.
std::optional<std::string> painted_modifier_shape_refusal(const nlohmann::json& params)
{
    for (const char* key : {"shape", "size", "file_path"})
        if (params.contains(key))
            return std::string("a painted modifier has no shape of its own: it shares the mesh of the part it is painted on, "
                               "so ") +
                   key + " is not one of its arguments";
    return std::nullopt;
}

// The first model part's volume_id -- the object list's Add Painted Modifier paints the part under the
// cursor, and the first part is what an agent's call defaults to. -1 when the object has none.
int first_model_part_id(const ModelObject& object)
{
    for (std::size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i]->is_model_part())
            return int(i);
    return -1;
}

} // namespace

std::vector<int> painted_modifier_ids(const ModelObject& object)
{
    std::vector<int> ids;
    for (std::size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i]->is_painted_modifier())
            ids.push_back(int(i));
    return ids;
}

int host_volume_id(const ModelObject& object, const ModelVolume& painted_modifier)
{
    const ModelVolume* host = object.painted_modifier_host(painted_modifier);
    if (host == nullptr)
        return -1;
    for (std::size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i] == host)
            return int(i);
    return -1;
}

bool ensure_painted_modifiers_synced(ModelObject& object)
{
    if (!object.has_painted_modifiers())
        return false;
    return object.sync_painted_modifiers();
}

std::optional<std::string> painted_modifier_target_refusal(int object_id, const ModelObject& object, std::optional<int> volume_id)
{
    if (!volume_id) {
        const std::vector<int> ids = painted_modifier_ids(object);
        if (ids.empty())
            return object_text(object_id) + " has no painted modifier: add_volume {object_id: " + std::to_string(object_id) +
                   ", type: painted_modifier} adds one on a model part, and mode painted_modifier paints it";
        return object_text(object_id) + " has painted modifiers on volumes " + listed_ids(ids) +
               ": mode painted_modifier needs volume_id naming one of them";
    }
    if (const auto error = volume_id_error(object_id, object, *volume_id))
        return error;
    const ModelVolume& volume = *object.volumes[std::size_t(*volume_id)];
    if (!volume.is_painted_modifier())
        return volume_text(object_id, *volume_id) + " is " + volume_kind(volume) +
               ", not a painted modifier: mode painted_modifier addresses a painted modifier (add_volume with type "
               "painted_modifier adds one on a part)";
    if (host_volume_id(object, volume) < 0)
        return volume_text(object_id, *volume_id) + ": The part this modifier was painted on has been removed. delete_object "
                                                  "with volume_id removes it";
    return std::nullopt;
}

std::optional<std::string> read_depth_mm(const nlohmann::json& params, std::optional<float>& out)
{
    out = std::nullopt;
    if (!params.contains("depth_mm"))
        return std::nullopt;
    const nlohmann::json& value = params.at("depth_mm");
    if (value.is_number()) {
        const double depth = value.get<double>();
        if (depth >= 0. && depth <= 1000.) {
            out = float(depth);
            return std::nullopt;
        }
    }
    // Never clamped: a caller who meant 5 mm and sent 5000 would get a modifier that reaches across
    // the whole part. The refusal is the gizmo's own field range.
    return std::string("depth_mm must be a number from 0 to 1000: 0 reaches across the part to its nearest unpainted "
                       "surface; got ") +
           value.dump();
}

std::variant<PaintedModifierRequest, std::string> read_painted_modifier_request(int object_id, const ModelObject& object,
                                                                                const nlohmann::json& params)
{
    if (const auto refusal = painted_modifier_shape_refusal(params))
        return *refusal;
    int host_id = -1;
    if (params.contains("host_volume_id")) {
        if (!parse_integer_param(params.at("host_volume_id"), host_id) || host_id < 0)
            return "host_volume_id must be a whole number 0 or more, as get_object_info lists the volumes; got " +
                   params.at("host_volume_id").dump();
        if (const auto error = volume_id_error(object_id, object, host_id))
            return *error;
        // Upstream's rule: painted modifiers go on model parts only (ModelObject::add_painted_modifier).
        if (!object.volumes[std::size_t(host_id)]->is_model_part())
            return "host_volume_id must name a model part: " + volume_text(object_id, host_id) + " is " +
                   volume_kind(*object.volumes[std::size_t(host_id)]) + ", and a painted modifier is painted on a part's surface";
    } else {
        host_id = first_model_part_id(object);
        if (host_id < 0)
            return object_text(object_id) + " has no model part to paint a modifier on: add_volume adds a part first";
    }
    std::optional<float> depth_mm;
    if (const auto refusal = read_depth_mm(params, depth_mm))
        return *refusal;
    return PaintedModifierRequest{host_id, depth_mm};
}

std::optional<std::string> painted_modifier_argument_refusal(const std::string& type_name, const nlohmann::json& params)
{
    if (type_name == "painted_modifier")
        return std::nullopt;
    for (const char* key : {"host_volume_id", "depth_mm"})
        if (params.contains(key))
            return std::string(key) + " belongs to type painted_modifier alone: a " + type_name +
                   " stands on its own, it is not painted on a part";
    return std::nullopt;
}

nlohmann::json painted_modifier_row_fields(const ModelObject& object, std::size_t volume_idx)
{
    if (volume_idx >= object.volumes.size() || !object.volumes[volume_idx]->is_painted_modifier())
        return nlohmann::json::object();
    const ModelVolume& volume = *object.volumes[volume_idx];
    const int          host   = host_volume_id(object, volume);
    nlohmann::json     settings = nlohmann::json::array();
    t_config_option_keys keys = volume.config.keys();
    std::sort(keys.begin(), keys.end());
    for (const std::string& key : keys)
        settings.push_back(key);
    return {{"host_volume_id", host >= 0 ? nlohmann::json(host) : nlohmann::json()},
            {"depth_mm", volume.painted_modifier_depth},
            {"painted", !volume.painted_modifier_facets.empty()},
            {"settings", std::move(settings)}};
}

nlohmann::json painted_modifier_coverage_json(const ModelVolume& painted_modifier)
{
    int    painted_facets   = 0;
    double coverage_percent = 0.;
    for (const PaintedStateInfo& info : read_volume_paint(painted_modifier, PaintMode::PaintedModifier))
        if (info.state == int(EnforcerBlockerType::ENFORCER)) {
            painted_facets   = info.facet_count;
            coverage_percent = info.area_ratio * 100.;
            break;
        }
    return {{"painted_facets", painted_facets}, {"coverage_percent", coverage_percent}};
}

std::vector<int> painted_modifiers_orphaned_by_type_change(const ModelObject& object, int volume_id, ModelVolumeType to)
{
    // Turning a part into a part changes nothing, and a painted modifier named by another volume's id
    // is that volume's own; only a host leaving MODEL_PART orphans one (PrintApply reads the region of
    // a painted modifier through the part it paints).
    if (to == ModelVolumeType::MODEL_PART || volume_id < 0 || std::size_t(volume_id) >= object.volumes.size())
        return {};
    const ObjectID host_id = object.volumes[std::size_t(volume_id)]->id();
    std::vector<int> ids;
    for (std::size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume& volume = *object.volumes[i];
        if (volume.is_painted_modifier() && volume.painted_modifier_host == host_id)
            ids.push_back(int(i));
    }
    return ids;
}

}}} // namespace Slic3r::GUI::OrcaMCP
