// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshHealth.cpp
#include "OrcaMCPMeshHealth.hpp"

#include "OrcaMCPFilamentModel.hpp"
#include "OrcaMCPPaintModel.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libslic3r/Model.hpp"

#include <algorithm>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// The list's sidebar line joins its two halves with a newline; a reason is one line.
std::string one_line(const wxString& text)
{
    std::string line = into_u8(text);
    std::replace(line.begin(), line.end(), '\n', ' ');
    return line;
}

MeshHealth health_of(const TriangleMeshStats& stats)
{
    MeshHealth health;
    health.stats   = stats;
    health.warning = !get_warning_icon_name(stats).empty();
    return health;
}

nlohmann::json repaired_errors_json(const RepairedMeshErrors& errors)
{
    return {{"edges_fixed", errors.edges_fixed},
            {"degenerate_facets", errors.degenerate_facets},
            {"facets_removed", errors.facets_removed},
            {"facets_reversed", errors.facets_reversed},
            {"backwards_edges", errors.backwards_edges}};
}

// mesh_warning, and tooltip and mesh_warning_reason when it is true: a row of get_mesh_health.
void add_row_state(nlohmann::json& out, const MeshHealth& health)
{
    out["mesh_warning"] = health.warning;
    if (health.warning) {
        out["tooltip"]             = mesh_warning_tooltip(health);
        out["mesh_warning_reason"] = mesh_warning_reason(health);
    }
}

nlohmann::json volume_json(const ModelVolume& volume, int volume_idx, const MeshHealth& health)
{
    nlohmann::json out = {{"volume_id", volume_idx}, {"name", volume.name}, {"type", volume_type_name(volume.type())}};
    out.update(mesh_numbers_json(health));
    add_row_state(out, health);
    return out;
}

} // namespace

int MeshHealth::errors_repaired() const { return repaired_errors_count(stats.repaired_errors); }

MeshHealth object_mesh_health(const ModelObject& object)
{
    TriangleMeshStats stats = object.get_object_stl_stats();
    stats.number_of_facets  = uint32_t(object.facets_count());
    return health_of(stats);
}

MeshHealth volume_mesh_health(const ModelObject& object, int volume_idx)
{
    return health_of(object.volumes[std::size_t(volume_idx)]->mesh().stats());
}

std::vector<MeshHealth> model_mesh_health(const Model& model)
{
    std::vector<MeshHealth> health;
    health.reserve(model.objects.size());
    for (const ModelObject* object : model.objects)
        health.push_back(object_mesh_health(*object));
    return health;
}

std::string mesh_warning_tooltip(const MeshHealth& health)
{
    return health.warning ? into_u8(mesh_errors_info(health.stats).tooltip) : std::string();
}

std::string mesh_warning_reason(const MeshHealth& health)
{
    if (!health.warning)
        return {};
    wxString sidebar;
    mesh_errors_info(health.stats, &sidebar);
    return one_line(sidebar);
}

nlohmann::json mesh_numbers_json(const MeshHealth& health)
{
    return {{"facets", health.facets()},
            {"shells", health.shells()},
            {"open_edges", health.open_edges()},
            {"manifold", health.manifold()},
            {"repaired", health.repaired()},
            {"errors_repaired", health.errors_repaired()},
            {"repaired_errors", repaired_errors_json(health.stats.repaired_errors)}};
}

nlohmann::json mesh_features_json(const MeshHealth& object_health)
{
    nlohmann::json features = mesh_numbers_json(object_health);
    features["volume_mm3"]  = object_health.stats.volume;
    return features;
}

void add_mesh_warning(nlohmann::json& out, const MeshHealth& object_health)
{
    out["mesh_warning"] = object_health.warning;
    if (object_health.warning)
        out["mesh_warning_reason"] = mesh_warning_reason(object_health);
}

nlohmann::json mesh_warning_entries(const Model& model)
{
    nlohmann::json entries = nlohmann::json::array();
    for (std::size_t i = 0; i < model.objects.size(); ++i) {
        const ModelObject& object = *model.objects[i];
        const MeshHealth   health = object_mesh_health(object);
        if (health.warning)
            entries.push_back({{"level", "warning"},
                               {"type", "MeshErrors"},
                               {"object_id", int(i)},
                               {"object_name", object.name},
                               {"message", mesh_warning_tooltip(health)}});
    }
    return entries;
}

MeshHealthReport mesh_health_report(const ModelObject& object, int object_id)
{
    MeshHealthReport report;
    nlohmann::json&  r = report.response;
    const MeshHealth object_health = object_mesh_health(object);
    r = {{"status", "success"}, {"object_id", object_id}, {"object_name", object.name}};
    add_row_state(r, object_health);
    r["summary"] = mesh_features_json(object_health);

    nlohmann::json volumes = nlohmann::json::array();
    for (std::size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume& volume = *object.volumes[i];
        const MeshHealth   health = volume_mesh_health(object, int(i));
        volumes.push_back(volume_json(volume, int(i), health));
        // The shells get_object_components lists: model parts only, and only when there is more
        // than one -- a single shell is the whole part, and the flood fill is the slow half.
        if (volume.is_model_part() && health.shells() > 1)
            report.shell_jobs.push_back({i, volume.mesh_ptr(), volume_to_plate(object, volume, 0)});
    }
    r["volumes"] = std::move(volumes);
    return report;
}

void add_shell_lists(MeshHealthReport& report, int max_listed)
{
    for (const ShellListJob& job : report.shell_jobs) {
        const std::vector<ComponentInfo> shells = shells_most_facets_first(job.mesh->its, job.to_plate);
        const std::size_t                listed = std::min(shells.size(), std::size_t(std::max(max_listed, 0)));

        nlohmann::json listed_shells = nlohmann::json::array();
        for (std::size_t i = 0; i < listed; ++i)
            listed_shells.push_back(component_json(shells[i]));

        report.response["volumes"][job.volume_idx]["shell_list"] = {
            {"total", int(shells.size())},
            {"listed", int(listed)},
            {"coordinate_frame", "plate"},
            {"instance_id", 0},
            {"shells", std::move(listed_shells)},
            {"note", "Most facets first, at most " + std::to_string(max_listed) +
                         "; area_mm2 and bounding_box say which is small. get_object_components lists "
                         "every shell; its ids are these, the ones paint_object {selection: \"component\"} "
                         "takes."}};
    }
}

}}} // namespace Slic3r::GUI::OrcaMCP
