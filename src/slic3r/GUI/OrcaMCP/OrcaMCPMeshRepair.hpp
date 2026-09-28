// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshRepair.hpp
#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "OrcaMCPSliceProgress.hpp"

// repair_mesh's decisions and answer, apart from the app, so they are tested without it
// (tests/slic3rutils/test_mesh_repair.cpp). The repair is the object list's own (FixModelByCgal.hpp);
// the tool (OrcaMCPMeshTools.cpp) runs it in three steps: capture on the main thread, plan on the
// HTTP thread, apply on the main thread.

namespace Slic3r {
class ModelObject;
class CgalRepairPlan;
namespace GUI { namespace OrcaMCP {

// Why repair_mesh must not start, or change the object, now: the slicing pipeline is busy (the
// repair would restart it), an arrange or orient job runs, or the app's own Repair runs, whose
// progress dialog lets calls through while it works out its plan.
std::optional<std::string> repair_refusal(const PipelineState& pipeline, bool ui_job_running, bool repair_dialog_running);

// What is wrong with `volume_id` for object `object_id` of `volumes` volumes, or nothing: any value
// outside 0 to volumes - 1, negative ones included. None given: the whole object.
std::optional<std::string> repair_volume_error(int object_id, std::size_t volumes, std::optional<int> volume_id);

// What a plan leads to once its object is found as it was captured. Only `apply` changes the object,
// and only it takes an undo snapshot.
enum class RepairStep { apply, nothing_to_repair, timed_out, failed, leaves_nothing };
RepairStep repair_step(const CgalRepairPlan& plan, bool past_wait_cap);

// Why a step other than `apply` changed nothing, for volume `volume_id` (-1: the whole object) of
// object `object_id`. `before` is repair_facts_json of the object; `wait_cap_s` how long the call
// could wait.
std::string repair_step_message(RepairStep step, const CgalRepairPlan& plan, int object_id, int volume_id, double wait_cap_s,
                                const nlohmann::json& before);

// An object as repair_mesh reports it before and after: get_mesh_health's numbers for the object
// with its mesh_warning, `volumes` (how many), `painted` (any volume has painting of any kind) and
// `position` (the centre of its box, plate mm).
nlohmann::json repair_facts_json(const ModelObject& object);

// {split, dropped, repaired}: the parts the plan's splits made (0: no volume had several shells), the
// parts with no volume it deleted, and the parts whose holes it closed.
nlohmann::json repair_parts_json(const CgalRepairPlan& plan);

// The answer to a repair that was applied, before the tool adds the volume rows and the placement.
// `volume_id` -1: the whole object. `keep_painting_given`: the call said, rather than the app's setting.
nlohmann::json repair_answer_json(int object_id, const std::string& object_name, int volume_id, bool keep_painting,
                                  bool keep_painting_given, const nlohmann::json& before, const nlohmann::json& after,
                                  const CgalRepairPlan& plan);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
