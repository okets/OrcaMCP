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

// What the object list says about a mesh -- whether it shows its warning icon -- and the numbers
// behind it, for an object's row or one of its volumes' rows. Read from the model alone, through
// the list's own functions (GUI_ObjectList.hpp), so it needs no GUI and never disagrees with the
// list. Reading one costs a pass over the object's volumes and builds no text: the text below is
// built only for a caller that reports it.
struct MeshHealth
{
    // The stats the list reads for the row: an object's get_object_stl_stats() -- open edges and
    // repairs of every volume, shells and volume of its model parts only -- with number_of_facets
    // (which that leaves 0) filled in from its model parts, or a volume's mesh().stats().
    TriangleMeshStats stats;
    bool              warning = false;  // the list shows its warning icon

    int  facets() const { return int(stats.number_of_facets); }
    int  shells() const { return stats.number_of_parts; }
    int  open_edges() const { return stats.open_edges; }
    bool manifold() const { return stats.manifold(); }
    bool repaired() const { return stats.repaired(); }
    int  errors_repaired() const;  // the repair count the tooltip states
};

// The object's row in the object list.
MeshHealth object_mesh_health(const ModelObject& object);
// The row of volume `volume_idx` (an index into object.volumes).
MeshHealth volume_mesh_health(const ModelObject& object, int volume_idx);
// Every object's row, indexed as model.objects: what a scene-wide report reads once and passes on.
std::vector<MeshHealth> model_mesh_health(const Model& model);

// The icon's tooltip as the list builds it, less its last line, the GUI's "click the icon" (which
// agents passed on to users as an instruction), and the list's sidebar line, on one line. In the
// app's language; empty for a row without the icon.
std::string mesh_warning_tooltip(const MeshHealth& health);
std::string mesh_warning_reason(const MeshHealth& health);

// The numbers every report shares: {facets, shells, open_edges, manifold, repaired, errors_repaired,
// repaired_errors: {edges_fixed, degenerate_facets, facets_removed, facets_reversed, backwards_edges}}.
nlohmann::json mesh_numbers_json(const MeshHealth& health);

// get_scene_info's `features` and get_mesh_health's `summary`, from an object's health: its numbers
// plus volume_mm3, its model parts' volume at the first instance's scale (the list's sidebar figure;
// only approximate while the mesh has open edges).
nlohmann::json mesh_features_json(const MeshHealth& object_health);

// Sets `mesh_warning` on a per-object description -- true when the object list shows the object's
// warning icon -- and, only then, `mesh_warning_reason`, the list's one-line reason.
void add_mesh_warning(nlohmann::json& out, const MeshHealth& object_health);

// What an agent can do about a row with the icon, in place of the tooltip's GUI-only "click the
// icon": repair_mesh for open edges, and what slicing does with the mesh -- never a GUI button to
// send the user to. English; empty without the icon.
std::string mesh_warning_advice(const MeshHealth& health);

// A MeshErrors warning for an object the list flags: {level: "warning", type: "MeshErrors",
// object_id, object_name, message: <the reason, the advice, where the numbers are and, for open edges,
// the repair_mesh call>}. The icon is scene state that stays until someone repairs the mesh (a
// closed mesh's recorded repairs stay even then), so only get_scene_info and load_model report
// these, never every tool's active_warnings.
nlohmann::json mesh_error_warning(const ModelObject& object, int object_id, const MeshHealth& health);
// One for every object of `model` whose icon shows, from its health read once (model_mesh_health)...
nlohmann::json mesh_error_warnings(const Model& model, const std::vector<MeshHealth>& health);
// ...or for the objects `object_indices` names, read here (load_model's newly added objects).
nlohmann::json mesh_error_warnings(const Model& model, const std::vector<int>& object_indices);

// The object_index of every object description in `objects` (model_object_summary_json entries,
// such as loaded_objects) whose mesh_warning is true.
std::vector<int> flagged_object_indices(const nlohmann::json& objects);

// A model part get_mesh_health lists the shells of (one with more than one): its mesh -- the
// shared_ptr keeps it alive if the model drops it -- and the transform that puts it on the plate.
struct ShellListJob
{
    std::size_t                         volume_idx = 0;  // its entry in response["volumes"]
    std::shared_ptr<const TriangleMesh> mesh;
    Transform3d                         to_plate = Transform3d::Identity();
};

// get_mesh_health's response before the shell lists, and the parts that still need one. Built on
// the GUI thread; add_shell_lists does the flood fill, which can take seconds, off it. Every row --
// the object and each volume -- has mesh_warning, and tooltip and mesh_warning_reason only when it
// is true: the one shape every per-object description uses. An object with open edges also gets
// next_steps: repair_mesh (mesh_repair_next_steps).
struct MeshHealthReport
{
    nlohmann::json            response;
    std::vector<ShellListJob> shell_jobs;
};

MeshHealthReport mesh_health_report(const ModelObject& object, int object_id);

// get_mesh_health's `volumes` rows without the shell lists: {volume_id, name, type, the numbers,
// mesh_warning, and tooltip and mesh_warning_reason when it is true}. repair_mesh answers with them.
nlohmann::json mesh_volume_rows_json(const ModelObject& object);

// Each job's part gets `shell_list`: {total, listed, coordinate_frame: "plate", instance_id: 0,
// shells: [component_json...] (the `max_listed` with the most facets, most first), note}.
constexpr int k_listed_shells = 10;
void add_shell_lists(MeshHealthReport& report, int max_listed = k_listed_shells);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
