// src/slic3r/GUI/OrcaMCP/OrcaMCPNextSteps.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "OrcaMCPMeshHealth.hpp"
#include "OrcaMCPSliceProgress.hpp"

namespace Slic3r {
class Model;
class ModelObject;
namespace GUI { namespace OrcaMCP {

// What a response suggests calling next, when what it found implies a follow-up: a loaded object with
// a stray shell, a slice that runs in the background. One shape everywhere, `next_steps`:
//
//   [{"tool": "<a tool name>", "arguments": {...}, "why": "<the facts from this response>"}]
//
// `arguments`, left out when the tool needs none, is accepted by that tool's schema. At most one step
// per tool: `arguments` names the first object it applies to and `why` names them all, so a scene of
// many flagged objects does not bury the rest of the response. The tests check every `tool` is a real
// tool and every `arguments` passes its schema (test_mcp_next_steps.cpp).
struct NextStep
{
    std::string    tool;
    std::string    why;
    nlohmann::json arguments; // null: the call needs none
};

// {tool, arguments (unless null), why}.
nlohmann::json next_step_json(const NextStep& step);

// Sets response["next_steps"] to `steps`; a response with none gets no field.
void add_next_steps(nlohmann::json& response, const std::vector<NextStep>& steps);

// "0", "0 and 3", "0, 3 and 7"; past `max_listed` ids, "... and N more".
std::string listed_ids(const std::vector<int>& ids, size_t max_listed = 10);

// The objects `object_indices` names (indices into model.objects) that need a closer look:
// get_mesh_health for one whose object-list row shows the warning icon (`health`, by object index:
// model_mesh_health), get_object_components for one with a model part made of more than one shell --
// a loose part or a stray fragment, which leaves no warning icon when it is closed. load_model asks
// about the objects it added, get_scene_info about every object.
std::vector<NextStep> mesh_next_steps(const Model& model, const std::vector<int>& object_indices, const std::vector<MeshHealth>& health);
// The same for every object of `model`.
std::vector<NextStep> mesh_next_steps(const Model& model, const std::vector<MeshHealth>& health);

// What get_mesh_health's answer about object `object_id` leads to: repair_mesh when its row has open
// edges. Nothing for a closed object, even one whose warning shows repairs recorded at load: a repair
// leaves a closed one-shell mesh, and that record, as they are.
std::vector<NextStep> mesh_repair_next_steps(const ModelObject& object, int object_id, const MeshHealth& health);

// What split_object's split to parts of volume `volume_id` leads to: get_object_components, which gives
// each of the `pieces` new parts' facets and box, to find a fragment (delete_object with volume_id
// deletes one).
std::vector<NextStep> split_parts_next_steps(int object_id, int volume_id, const std::vector<int>& pieces);

// What set_instance_count's added instances lead to: arrange_objects of plate `plate_index` when some of
// them (`crowded`) overlap another object or instance there, or stand off every plate (`off_plate`): the
// GUI's Add instance puts each one a small step from the last, not where there is room. Nothing when
// every added one stands clear. A plate_index below 0 (the object is on no plate) arranges the current
// plate.
std::vector<NextStep> added_instances_next_steps(int object_id, int plate_index, const std::vector<int>& crowded,
                                                 const std::vector<int>& off_plate);

// What set_plate_settings' print_sequence "by object" on plate `plate_index` leads to: arrange_objects of
// that plate, which spaces objects for printing one after another, as the app's notice suggests.
std::vector<NextStep> print_by_object_next_steps(int plate_index);

// What turning a plate's spiral vase off leads to: reset_object_config of the objects (`object_ids`) that
// still carry the object settings the vase gave them (`first_keys`: the first one's), which a plate
// without the vase prints thin-walled and open. Nothing when none carries them.
std::vector<NextStep> vase_settings_next_steps(const std::vector<int>& object_ids, const std::vector<std::string>& first_keys);

// What a new or retyped volume `volume_id` of type `type_name` leads to: move_object with volume_id when
// it stands `beside_object` (add_volume's primitive, at the object's right-front corner), and, for a
// modifier, set_object_config with volume_id: a modifier changes only the settings it is given.
std::vector<NextStep> new_volume_next_steps(int object_id, int volume_id, const std::string& type_name, bool beside_object);

// What assemble_objects' answer leads to: get_object_info of the new object, its volumes listed.
std::vector<NextStep> assembled_next_steps(int object_id);

// What slice_all's answer leads to: wait_for_slice for a slice that started, or for a busy pipeline
// (then slice_all again); get_slicing_status while an arrange or an orient holds the app (its ui_job;
// wait_for_slice does not wait for those); get_print_estimate of `sliced_plate` for plates already
// sliced -- by its plate_index, since the selected plate may be an empty one with no result, and none
// when no plate has one. A refusal the app explains (invalid, nothing_to_slice, unknown) leads to no
// tool: its message says what to fix.
std::vector<NextStep> slice_start_next_steps(const SliceStartReport& report, std::optional<int> sliced_plate);

// What export_gcode's answer leads to: wait_for_slice while the file is written (export_started), which
// returns once the export is over; nothing for an export that did not start.
std::vector<NextStep> export_next_steps(bool export_started);

// What a render_plate_view view whose picture came out one flat colour leads to (uniform_image_hint
// says why in words): get_scene_info when nothing on plate `plate_index` was drawn (`drawn` 0: no model
// volume in the 3D view, or none printable on that plate), render_plate_view of that plate without
// views, fitted to it, when its objects were drawn but the camera looked elsewhere.
std::vector<NextStep> uniform_image_next_steps(size_t model_volumes, size_t drawn, int plate_index);

// What paint_object's support paint leads to: only when the object has painted enforcers and
// enable_support is off for it, set_object_config turns support on for that object with the (manual)
// variant of its support_type (`support_type`, the object's effective value), which generates support
// only where painted; enable_support with an (auto) type would support the whole object. Blockers
// alone, or erased paint, lead nowhere: turning support on is the opposite of what a blocker asks.
std::vector<NextStep> support_paint_next_steps(int object_id, bool support_enabled, bool enforcers_painted,
                                               const std::string& support_type);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
