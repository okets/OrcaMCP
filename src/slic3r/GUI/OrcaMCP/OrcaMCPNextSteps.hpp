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

// What set_instance_count's added instances lead to: arrange_objects of every plate when some of them
// (`on_no_plate`) stand on no plate -- a plate's arrange takes only the instances it holds, so only the
// arrange of every plate places them -- else arrange_objects of plate `plate_index` when some overlap
// another object or instance there (`crowded`) or stand partly off it (`partly_off`): the GUI's Add
// instance puts each one a small step from the last, not where there is room. Nothing when every added
// one stands clear.
std::vector<NextStep> added_instances_next_steps(int object_id, int plate_index, const std::vector<int>& crowded,
                                                 const std::vector<int>& partly_off, const std::vector<int>& on_no_plate);

// What fill_bed_with_instances' instances on no plate (`on_no_plate`, of the object's `instance_count`)
// lead to: the fill's estimate added more than its plate's arrange fit. arrange_objects of every plate
// puts them on plates of their own; or they are removed -- set_instance_count when they are the last
// ones, else delete_object of the highest (instance ids after a deleted one shift down).
std::vector<NextStep> unplaced_instances_next_steps(int object_id, const std::vector<int>& on_no_plate, int instance_count);

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

// What export_gcode's answer leads to: wait_for_slice while the file is still being written past the call's own wait
// (export_started, finished false), which returns once the export is over; nothing for an export that did not start,
// or that the call saw end.
std::vector<NextStep> export_next_steps(bool export_started);

// What cancel_slice's answer leads to: wait_for_slice while the cancelled slice is still stopping (its
// completion not taken in yet), which returns once it has; nothing once the run is over.
std::vector<NextStep> cancel_slice_next_steps(bool still_stopping);

// What add_layer_gcode's or delete_layer_gcode's change on plate `plate_index` leads to: slice_all, which
// slices that plate again (plates still sliced are kept); nothing for a call that changed nothing.
std::vector<NextStep> layer_gcode_next_steps(int plate_index, bool changed);

// What show_view's switch to the Preview tab leads to when it started a slice of the selected plate
// (`slice_started`): wait_for_slice, which returns once it is over.
std::vector<NextStep> show_view_next_steps(bool slice_started);

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

// What printer_control's answer to a set_* action (filtration, fans, print speed, Z offset) leads to:
// get_printer_status, whose printer.controls reads back what the printer now reports -- it takes a moment
// to apply a command, as the Device page shows by waiting for the next status.
std::vector<NextStep> printer_control_next_steps();

// What get_printer_status's answer, or printer_control's refusal, leads to when the printer's report has
// job and control numbers no reading can be (PrinterStatus::implausible_telemetry, named in `why`):
// get_printer_status again, a few seconds later. None when there are none.
std::vector<NextStep> untrusted_status_next_steps(const std::vector<std::string>& implausible_telemetry);

// What match_project_to_printer's answer leads to when the printer holds filament in material-station
// slots the project has no filament slot for (`missing_slots`, 1-based): add_filament_slot, which adds one
// slot per call, on a printer that takes more slots (`slots_can_be_added`); nothing on one whose slots
// follow its extruders.
// What install_presets' answer leads to when it installed printers (`printers`, their names): select_preset
// of the first, since an install enables them without selecting one -- the Setup Wizard selects the new
// printer, and a switch changes the filament slots and their colours, so it is left to its own call.
std::vector<NextStep> installed_printer_next_steps(const std::vector<std::string>& printers);

// What a refusal for the built-in default printer leads to (slice_all's no_printer, export_gcode's): with
// no printer installed, get_presets of the printers that can be, which install_presets installs by name
// (and its answer leads to select_preset); with one installed, select_preset of it. Nothing for a real one.
std::vector<NextStep> printer_setup_next_steps(const PrinterSetup& setup);

// What add_filament_slot's and delete_filament_slot's answers lead to when they renumbered slots or moved
// objects (`renumbered`): get_scene_info, whose filaments_used shows each object's slots now -- and after any
// undo, which would bring back the objects' old slot numbers without the slots (undo_warning).
std::vector<NextStep> slot_change_next_steps(bool renumbered);

std::vector<NextStep> missing_slot_next_steps(const std::vector<int>& missing_slots, bool slots_can_be_added);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
