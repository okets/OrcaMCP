// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshHealth.hpp
#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "OrcaMCPPaintSelect.hpp"

namespace Slic3r {
class Model;
class ModelObject;
namespace GUI { namespace OrcaMCP {

// What the object list says about a mesh -- its warning icon and the icon's tooltip -- and the
// numbers behind it, for an object's row or one of its volumes' rows. Read from the model alone,
// through the list's own mesh_errors_info (GUI_ObjectList.hpp), so it needs no GUI and never
// disagrees with the list. The tooltip and reason are in the app's language; the numbers are not.
struct MeshHealth
{
    int                facets          = 0;     // an object's: its model parts' facets
    int                shells          = 0;     // connected pieces; an object's: its model parts'
    int                open_edges      = 0;     // edges with one facet; an object's: every volume's, as the list counts
    RepairedMeshErrors repaired_errors;         // recorded repairs; an object's: every volume's
    int                errors_repaired = 0;     // their sum, the number the tooltip states
    bool               warning         = false; // the list shows its warning icon
    std::string        tooltip;                 // the icon's tooltip, exactly; empty without the icon
    std::string        reason;                  // the list's sidebar line, on one line; empty without the icon

    bool manifold() const { return open_edges == 0; }
    bool repaired() const { return repaired_errors.repaired(); }
};

// The object's row in the object list.
MeshHealth object_mesh_health(const ModelObject& object);
// The row of volume `volume_idx` (an index into object.volumes).
MeshHealth volume_mesh_health(const ModelObject& object, int volume_idx);

// The numbers every report shares: {facets, shells, open_edges, manifold, repaired, errors_repaired,
// repaired_errors: {edges_fixed, degenerate_facets, facets_removed, facets_reversed, backwards_edges}}.
nlohmann::json mesh_numbers_json(const MeshHealth& health);

// get_scene_info's `features` and get_mesh_health's `summary`: the object's numbers plus volume_mm3,
// its model parts' volume at the first instance's scale (the list's sidebar figure; only approximate
// while the mesh has open edges).
nlohmann::json mesh_features_json(const ModelObject& object);

// active_warnings' mesh entries: {level: "warning", type: "MeshErrors", object_id, object_name,
// message: <the icon's tooltip>}, one for every object of `model` whose warning icon shows. The icon
// is no pop-up notification, so the notification manager never reports it.
nlohmann::json mesh_warning_entries(const Model& model);

// A model part get_mesh_health lists the shells of (one with more than one): its mesh -- the
// shared_ptr keeps it alive if the model drops it -- and the transform that puts it on the plate.
struct ShellListJob
{
    std::size_t                         volume_idx = 0;  // its entry in response["volumes"]
    std::shared_ptr<const TriangleMesh> mesh;
    Transform3d                         to_plate = Transform3d::Identity();
};

// get_mesh_health's response before the shell lists, and the parts that still need one. Built on
// the GUI thread; add_shell_lists does the flood fill, which can take seconds, off it.
struct MeshHealthReport
{
    nlohmann::json            response;
    std::vector<ShellListJob> shell_jobs;
};

MeshHealthReport mesh_health_report(const ModelObject& object, int object_id);

// Each job's part gets `shell_list`: {total, listed, coordinate_frame: "plate", instance_id: 0,
// shells: [component_json...] (the largest `max_listed`, largest first), note}.
constexpr int k_listed_shells = 10;
void add_shell_lists(MeshHealthReport& report, int max_listed = k_listed_shells);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
