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

int model_part_facets(const ModelObject& object)
{
    int facets = 0;
    for (const ModelVolume* volume : object.volumes)
        if (volume->is_model_part())
            facets += int(volume->mesh().facets_count());
    return facets;
}

// The stats-derived numbers, and what the list shows for the same row (vol_idx -1: the object).
MeshHealth mesh_health(const ModelObject& object, int vol_idx, const TriangleMeshStats& stats, int facets)
{
    MeshHealth health;
    health.facets          = facets;
    health.shells          = stats.number_of_parts;
    health.open_edges      = stats.open_edges;
    health.repaired_errors = stats.repaired_errors;
    health.errors_repaired = object.get_repaired_errors_count(vol_idx);

    const MeshErrorsInfo tooltip = mesh_errors_info(object, vol_idx);
    health.warning               = !tooltip.warning_icon_name.empty();
    if (health.warning) {
        wxString sidebar;
        mesh_errors_info(object, vol_idx, &sidebar);
        health.tooltip = into_u8(tooltip.tooltip);
        health.reason  = one_line(sidebar);
    }
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

// mesh_warning, tooltip and mesh_warning_reason: the list's row, as get_mesh_health reports it.
void add_row_state(nlohmann::json& out, const MeshHealth& health)
{
    out["mesh_warning"]        = health.warning;
    out["tooltip"]             = health.tooltip;
    out["mesh_warning_reason"] = health.reason;
}

nlohmann::json volume_json(const ModelObject& object, int volume_idx)
{
    const ModelVolume& volume = *object.volumes[std::size_t(volume_idx)];
    const MeshHealth   health = volume_mesh_health(object, volume_idx);

    nlohmann::json out = {{"volume_id", volume_idx}, {"name", volume.name}, {"type", volume_type_name(volume.type())}};
    out.update(mesh_numbers_json(health));
    add_row_state(out, health);
    return out;
}

} // namespace

MeshHealth object_mesh_health(const ModelObject& object)
{
    return mesh_health(object, -1, object.get_object_stl_stats(), model_part_facets(object));
}

MeshHealth volume_mesh_health(const ModelObject& object, int volume_idx)
{
    const TriangleMesh& mesh = object.volumes[std::size_t(volume_idx)]->mesh();
    return mesh_health(object, volume_idx, mesh.stats(), int(mesh.facets_count()));
}

nlohmann::json mesh_numbers_json(const MeshHealth& health)
{
    return {{"facets", health.facets},
            {"shells", health.shells},
            {"open_edges", health.open_edges},
            {"manifold", health.manifold()},
            {"repaired", health.repaired()},
            {"errors_repaired", health.errors_repaired},
            {"repaired_errors", repaired_errors_json(health.repaired_errors)}};
}

nlohmann::json mesh_features_json(const ModelObject& object)
{
    nlohmann::json features = mesh_numbers_json(object_mesh_health(object));
    features["volume_mm3"]  = object.get_object_stl_stats().volume;
    return features;
}

MeshHealthReport mesh_health_report(const ModelObject& object, int object_id)
{
    MeshHealthReport report;
    nlohmann::json&  r = report.response;
    r = {{"status", "success"}, {"object_id", object_id}, {"object_name", object.name}};
    add_row_state(r, object_mesh_health(object));
    r["summary"] = mesh_features_json(object);

    nlohmann::json volumes = nlohmann::json::array();
    for (std::size_t i = 0; i < object.volumes.size(); ++i) {
        volumes.push_back(volume_json(object, int(i)));
        // The shells get_object_components lists: model parts only, and only when there is more
        // than one -- a single shell is the whole part, and the flood fill is the slow half.
        const ModelVolume& volume = *object.volumes[i];
        if (volume.is_model_part() && volume.mesh().stats().number_of_parts > 1)
            report.shell_jobs.push_back({i, volume.mesh_ptr(), volume_to_plate(object, volume, 0)});
    }
    r["volumes"] = std::move(volumes);
    return report;
}

void add_shell_lists(MeshHealthReport& report, int max_listed)
{
    for (const ShellListJob& job : report.shell_jobs) {
        const std::vector<ComponentInfo> shells = shells_largest_first(job.mesh->its, job.to_plate);
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
            {"note", "Largest first, at most " + std::to_string(max_listed) +
                         ". get_object_components lists every shell; its ids are these, the ones "
                         "paint_object {selection: \"component\"} takes."}};
    }
}

}}} // namespace Slic3r::GUI::OrcaMCP
