// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshRepair.cpp
#include "OrcaMCPMeshRepair.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPMeshHealth.hpp"
#include "OrcaMCPUiJob.hpp"

#include "slic3r/Utils/FixModelByCgal.hpp"
#include "libslic3r/Model.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

std::string object_label(int object_id) { return "object " + std::to_string(object_id); }

std::string subject(int object_id, int volume_id)
{
    return volume_id < 0 ? object_label(object_id) : "volume " + std::to_string(volume_id) + " of " + object_label(object_id);
}

std::string without_final_period(std::string text)
{
    while (!text.empty() && (text.back() == '.' || text.back() == ' '))
        text.pop_back();
    return text;
}

std::string nothing_to_repair(int object_id, int volume_id, const nlohmann::json& before)
{
    std::string message = volume_id < 0 ? "Nothing to repair: " + subject(object_id, volume_id) +
                                              " has no open edges, each of its volumes is one shell, and none is flat or empty, "
                                              "so nothing was changed."
                                        : "Nothing to repair: " + subject(object_id, volume_id) +
                                              " has no open edges, is one shell, and is not flat or empty, so nothing was changed.";
    const int recorded = before.value("errors_repaired", 0);
    if (volume_id < 0 && before.value("mesh_warning", false) && recorded > 0)
        message += " Its mesh_warning comes from the " + std::to_string(recorded) + (recorded == 1 ? " repair" : " repairs") +
                   " recorded when the mesh was loaded: the mesh is closed and prints as it is, and a repair leaves that "
                   "record as it is.";
    return message;
}

std::string timed_out(int object_id, int volume_id, double wait_cap_s, const nlohmann::json& before)
{
    std::string message = "Repairing " + subject(object_id, volume_id) + " took longer than this call may wait (" +
                          std::to_string(int(std::lround(wait_cap_s))) +
                          " s), so nothing was changed. That wait is set a little under the MCP bridge's ORCAMCP_TIMEOUT: "
                          "a larger mesh needs a larger ORCAMCP_TIMEOUT.";
    if (volume_id < 0 && before.value("volumes", 0) > 1)
        message += " volume_id repairs one volume at a time.";
    return message;
}

std::string failed(const CgalRepairPlan& plan, int object_id)
{
    const std::string where = plan.error_volume >= 0 ? " on volume " + std::to_string(plan.error_volume) : std::string();
    return "Repairing " + object_label(object_id) + " failed" + where + ": " + without_final_period(plan.error) +
           ". Nothing was changed.";
}

std::string leaves_nothing(int object_id, int volume_id)
{
    const std::string why = volume_id < 0 ? "Every part of " + subject(object_id, volume_id) + " is flat or empty (no volume)"
                                          : "Volume " + std::to_string(volume_id) + " of " + object_label(object_id) +
                                                " is flat or empty (no volume), and the object has no other part to print";
    return why + ", so repairing it would leave nothing to print. Nothing was changed; delete_object removes the object.";
}

bool any_volume_painted(const ModelObject& object)
{
    return std::any_of(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* volume) { return volume->is_any_painted(); });
}

} // namespace

std::optional<std::string> repair_refusal(const PipelineState& pipeline, bool ui_job_running, bool repair_dialog_running)
{
    if (pipeline_busy(pipeline) != PipelineBusy::idle)
        return pipeline_busy_text(pipeline) + ", so nothing was repaired: call wait_for_slice, then repair_mesh again";
    if (ui_job_running)
        return ui_job_busy_message("repair_mesh");
    if (repair_dialog_running)
        return std::string("The app is running its own mesh repair, so nothing was repaired: call repair_mesh again once it has finished");
    return std::nullopt;
}

std::optional<std::string> repair_volume_error(int object_id, std::size_t volumes, int volume_id)
{
    if (volume_id < 0 || std::size_t(volume_id) < volumes)
        return std::nullopt;
    const std::string range = volumes == 1 ? "1 volume, volume_id 0" :
                                             std::to_string(volumes) + " volumes, volume_id 0 to " + std::to_string(volumes - 1);
    return "volume_id " + std::to_string(volume_id) + " is out of range: " + object_label(object_id) + " has " + range;
}

RepairStep repair_step(const CgalRepairPlan& plan, bool past_wait_cap)
{
    if (plan.canceled || past_wait_cap)
        return RepairStep::timed_out;
    if (!plan.error.empty())
        return RepairStep::failed;
    if (plan.leaves_no_model_part())
        return RepairStep::leaves_nothing;
    if (plan.changes_nothing())
        return RepairStep::nothing_to_repair;
    return RepairStep::apply;
}

std::string repair_step_message(RepairStep step, const CgalRepairPlan& plan, int object_id, int volume_id, double wait_cap_s,
                                const nlohmann::json& before)
{
    switch (step) {
    case RepairStep::nothing_to_repair: return nothing_to_repair(object_id, volume_id, before);
    case RepairStep::timed_out: return timed_out(object_id, volume_id, wait_cap_s, before);
    case RepairStep::failed: return failed(plan, object_id);
    case RepairStep::leaves_nothing: return leaves_nothing(object_id, volume_id);
    case RepairStep::apply: break;
    }
    return {};
}

nlohmann::json repair_facts_json(const ModelObject& object)
{
    const MeshHealth health = object_mesh_health(object);
    nlohmann::json   facts  = mesh_numbers_json(health);
    facts["mesh_warning"]   = health.warning;
    facts["volumes"]        = object.volumes.size();
    facts["painted"]        = any_volume_painted(object);
    const Vec3d centre      = object_world_box(object).center();
    facts["position"]       = {{"x", centre.x()}, {"y", centre.y()}, {"z", centre.z()}};
    return facts;
}

nlohmann::json repair_parts_json(const CgalRepairPlan& plan)
{
    return {{"split", plan.parts_split}, {"dropped", plan.parts_dropped}, {"repaired", plan.parts_repaired}};
}

nlohmann::json repair_answer_json(int object_id, const std::string& object_name, int volume_id, bool keep_painting,
                                  bool keep_painting_given, const nlohmann::json& before, const nlohmann::json& after,
                                  const CgalRepairPlan& plan)
{
    nlohmann::json answer = {{"status", "success"}, {"changed", true}, {"object_id", object_id}, {"object_name", object_name}};
    if (volume_id >= 0)
        answer["volume_id"] = volume_id;
    answer["keep_painting"]      = keep_painting;
    answer["keep_painting_from"] = keep_painting_given ? "argument" : "app_setting";
    answer["volumes_before"]     = before.value("volumes", 0);
    answer["volumes_after"]      = after.value("volumes", 0);
    answer["parts"]              = repair_parts_json(plan);
    answer["before"]             = before;
    answer["after"]              = after;
    return answer;
}

}}} // namespace Slic3r::GUI::OrcaMCP
