// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPMeshHealth.hpp"
#include "OrcaMCPMeshRepair.hpp"
#include "OrcaMCPUiJob.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/FixModelByCgal.hpp"
#include "slic3r/Utils/ThreadCancel.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"

#include <chrono>
#include <string>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

using Slic3r::CgalRepairInput;
using Slic3r::CgalRepairPlan;
using Slic3r::CgalRepairResult;
using Slic3r::ModelObject;

// A repair_mesh call, as its first step leaves it for the next two.
struct RepairRequest
{
    int             object_id           = -1;
    int             volume_id           = -1; // none given: the whole object
    bool            keep_painting       = false;
    bool            keep_painting_given = false;
    nlohmann::json  before;
    CgalRepairInput input;
};

// Why repair_mesh must not go on now (repair_refusal), read from the app.
std::optional<std::string> app_repair_refusal(Plater& plater)
{
    return repair_refusal(pipeline_state(plater, plater.get_partplate_list().get_plate_count()),
                          !plater.get_ui_job_worker().is_idle(), Slic3r::cgal_repair_dialog_running());
}

// Closes the toolbar tool (gizmo) open in the 3D view, as the user closes it, and says which it was;
// "" when none was open. The object list's Repair refuses while one is open -- its undo snapshot would
// land in the tool's own undo stack -- and an agent does what the user would: close it. New Project
// closes them the same way (Plater::priv::reset). Closing a painting tool (and cut, measure, brim ears,
// text, SVG) records its own undo step, as when the user closes it, so it is closed only for a repair
// that is about to be applied.
std::string close_open_toolbar_tool(Plater& plater)
{
    GLCanvas3D*      canvas = plater.get_view3D_canvas3D();
    GLGizmosManager& gizmos = canvas->get_gizmos_manager();
    if (gizmos.get_current_type() == GLGizmosManager::Undefined)
        return {};
    const GLGizmoBase* current = gizmos.get_current();
    std::string        name    = current != nullptr ? current->get_name(false) : std::string("a toolbar tool");
    canvas->reset_all_gizmos();
    return name;
}

bool toolbar_tool_open(Plater& plater)
{
    return plater.get_view3D_canvas3D()->get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined;
}

// Main thread, the first step: the call read and checked, and what the repair reads captured. Null to
// go on, else the answer.
nlohmann::json start_repair(const nlohmann::json& params, RepairRequest& request)
{
    Plater*      plater = wxGetApp().plater();
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), request.object_id, error);
    if (object == nullptr)
        return error_response(error);
    std::optional<int> volume_id;
    if (params.contains("volume_id")) {
        int given = -1;
        if (!parse_integer_param(params.at("volume_id"), given))
            return error_response("volume_id must be a whole number");
        volume_id = given;
    }
    if (const auto volume_error = repair_volume_error(request.object_id, object->volumes.size(), volume_id))
        return error_response(*volume_error);
    request.volume_id           = volume_id.value_or(-1);
    request.keep_painting_given = params.contains("keep_painting");
    if (request.keep_painting_given && !parse_boolean_param(params.at("keep_painting"), request.keep_painting))
        return error_response("keep_painting must be true or false");
    if (!request.keep_painting_given)
        request.keep_painting = wxGetApp().app_config->get_bool("keep_painting");
    if (const auto refusal = app_repair_refusal(*plater))
        return error_response(*refusal);

    request.before = repair_facts_json(*object);
    request.input  = Slic3r::capture_cgal_repair(*object, request.volume_id, request.keep_painting);
    return nullptr;
}

// What the object list's Repair does after changing an object (ObjectList::fix_through_cgal), with the
// plate bookkeeping every MCP transform uses: every instance re-homed, as a new one
// (rehome_and_report_placement, which writes the placement fields into `placement`), where the list
// re-homes instance 0 only.
void update_after_repair(Plater& plater, int object_id, int volume_id, std::size_t volumes_before, nlohmann::json& placement)
{
    ModelObject& object = *plater.model().objects[std::size_t(object_id)];
    object.ensure_on_bed();
    plater.changed_mesh(object_id);
    ObjectList* list = wxGetApp().obj_list();
    if (object.volumes.size() != volumes_before)
        list->add_volumes_to_object_in_list(std::size_t(object_id));
    rehome_and_report_placement(placement, object_id, /*moved=*/true);
    list->update_plate_values_for_items();
    list->update_item_error_icon(object_id, volume_id);
    list->update_info_items(std::size_t(object_id));
}

nlohmann::json with_closed_tool(nlohmann::json answer, const std::string& closed_tool)
{
    if (!closed_tool.empty())
        answer["closed_toolbar_tool"] = closed_tool;
    return answer;
}

// Main thread, the last step: the plan applied, if the object is still as captured and the plan
// changes it, under one undo snapshot; otherwise an answer saying why nothing changed.
nlohmann::json finish_repair(RepairRequest& request, CgalRepairPlan& plan, bool past_wait_cap, double wait_cap_s)
{
    Plater*                   plater = wxGetApp().plater();
    McpDialogSuppressionGuard guard;
    const RepairStep          step = repair_step(plan, past_wait_cap);
    if (step == RepairStep::timed_out)
        return error_response(repair_step_message(step, plan, request.object_id, request.volume_id, wait_cap_s, request.before));

    // Calls are served one at a time, so only the user changed the scene meanwhile.
    const int object_id = Slic3r::cgal_repair_object_index(plater->model(), plan);
    if (object_id < 0)
        return error_response("Object " + std::to_string(request.object_id) +
                              " was deleted in the app while it was being repaired, so nothing was changed");
    if (const auto refusal = app_repair_refusal(*plater))
        return error_response(*refusal);
    ModelObject* object = plater->model().objects[std::size_t(object_id)];
    if (!plan.applies_to(*object))
        return error_response("Object " + std::to_string(object_id) +
                              " changed in the app while it was being repaired, so nothing was changed: call repair_mesh again");
    // The volume's index now, which an object changed meanwhile may have moved.
    const int volume_id = plan.input().whole_object ? -1 : plan.volume_index_in(*object);

    if (step == RepairStep::nothing_to_repair) {
        nlohmann::json answer = {{"status", "success"}, {"changed", false}, {"object_id", object_id}, {"object_name", object->name}};
        if (volume_id >= 0)
            answer["volume_id"] = volume_id;
        answer["message"] = repair_step_message(step, plan, object_id, volume_id, wait_cap_s, request.before);
        answer["before"]  = request.before;
        report_placement(answer, object_id);
        return guard.report(answer);
    }
    if (step != RepairStep::apply)
        return error_response(repair_step_message(step, plan, object_id, volume_id, wait_cap_s, request.before));

    // A repair to apply: only now is an open toolbar tool closed, as the Repair needs.
    const std::string closed_tool = close_open_toolbar_tool(*plater);
    if (!closed_tool.empty()) {
        const int still_there = Slic3r::cgal_repair_object_index(plater->model(), plan);
        if (toolbar_tool_open(*plater))
            return guard.report(with_closed_tool(error_response("The toolbar tool " + closed_tool +
                                                                " is open in the app and did not close, so nothing was repaired"),
                                                 closed_tool));
        if (still_there < 0 || !plan.applies_to(*plater->model().objects[std::size_t(still_there)]))
            return guard.report(with_closed_tool(error_response("Closing the toolbar tool " + closed_tool + " changed object " +
                                                                std::to_string(object_id) +
                                                                ", so nothing was repaired: call repair_mesh again"),
                                                 closed_tool));
    }

    // The object list's own undo step: nothing above changed the object.
    Plater::TakeSnapshot snapshot(plater, _u8L("Repairing model object"));
    if (!request.keep_painting)
        plater->clear_before_change_mesh(object_id);
    const std::size_t      volumes_before = object->volumes.size();
    const CgalRepairResult result         = Slic3r::apply_cgal_repair(*object, request.keep_painting, plan);
    nlohmann::json         placement      = nlohmann::json::object();
    update_after_repair(*plater, object_id, volume_id < 0 ? -1 : result.first_volume, volumes_before, placement);

    nlohmann::json answer = repair_answer_json(object_id, object->name, volume_id, request.keep_painting,
                                               request.keep_painting_given, request.before, repair_facts_json(*object), plan);
    answer.update(placement);
    answer["volumes"] = mesh_volume_rows_json(*object);
    if (volume_id >= 0) {
        nlohmann::json became = nlohmann::json::array();
        for (int i = result.first_volume; i >= 0 && i <= result.last_volume; ++i)
            became.push_back(i);
        answer["volume_ids_after"] = std::move(became);
    }
    if (!result.error.empty()) {
        // A part the plan had not foreseen failed here: what the loop changed before it stays.
        answer["status"]  = "error";
        answer["message"] = "Repairing object " + std::to_string(object_id) + " stopped partway: " + result.error +
                            " What it changed before that stays; undo reverts the whole repair.";
    }
    answer["active_warnings"] = get_active_warnings_json(plater);
    return guard.report(with_closed_tool(std::move(answer), closed_tool));
}

// repair_mesh: capture on the main thread, the CGAL work on this (the HTTP) thread, apply on the main
// thread. The main thread never waits for CGAL, and only it changes the model. The work stops between
// parts when the app quits (the gate closes, and the next run_on_main_thread refuses with -32002,
// nothing applied) or when this call's wait runs out; a part being repaired runs to its end.
nlohmann::json repair_mesh(const nlohmann::json& params)
{
    RepairRequest        request;
    const nlohmann::json refusal = run_on_main_thread([&params, &request]() -> nlohmann::json { return start_repair(params, request); });
    if (!refusal.is_null())
        return refusal;

    using Clock                = std::chrono::steady_clock;
    const auto     wait_cap    = tool_wait_cap();
    const auto     deadline    = Clock::now() + wait_cap;
    CgalRepairPlan plan        = Slic3r::plan_cgal_repair(std::move(request.input), {},
                                                          [deadline] { return Slic3r::this_thread_cancelled() || Clock::now() > deadline; });
    const bool     past_cap    = Clock::now() > deadline;
    const double   wait_cap_s  = std::chrono::duration<double>(wait_cap).count();
    return run_on_main_thread([&]() -> nlohmann::json { return finish_repair(request, plan, past_cap, wait_cap_s); });
}

} // namespace

void OrcaMCPServer::register_mesh_tools()
{
    register_tool({
        "get_mesh_health",
        ToolCategory::Models,
        "Mesh errors: holes, open edges, repairs",
        "Check an object's mesh for problems: holes and open edges (non-manifold), repaired facets, and "
        "loose parts or stray shells, for one object and each of its volumes -- the errors behind the "
        "object list's warning icon. "
        "Every row -- the object and each volume -- has mesh_warning (whether the icon shows) and, only "
        "when it is true, `tooltip` (the icon's tooltip in the app's language, as the GUI builds it, less "
        "its last line, the GUI's \"click the icon\") and mesh_warning_reason (the sidebar's one line). A "
        "flagged object also gets `advice`: what MCP can do about the mesh, and what slicing does with it; "
        "one with open edges also gets next_steps: repair_mesh, which repairs it. The numbers: facets, "
        "shells, open_edges, manifold, repaired, "
        "errors_repaired and each recorded repair count. An object's open_edges and repairs count every volume, as the "
        "list does; its facets, shells and volume_mm3 count model parts only. Repair counts exist "
        "only for a mesh loaded from a 3MF that recorded them: an STL is repaired silently on import "
        "and records none. A part with more than one shell also gets `shell_list`: the 10 shells with "
        "the most facets, each with its area and plate-millimetre bounding box (instance 0); "
        "get_object_components lists them all.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            MeshHealthReport report;
            nlohmann::json   gate = run_on_main_thread([&params, &report]() -> nlohmann::json {
                int         object_id = -1;
                std::string error;
                const Slic3r::ModelObject* object = resolve_object_id(params, wxGetApp().plater()->model(), object_id, error);
                if (object == nullptr)
                    return error_response(error);
                report = mesh_health_report(*object, object_id);
                return {{"status", "success"}};
            });
            if (gate.value("status", "") != "success")
                return gate;

            // Worker thread: the flood fill over each multi-shell part's captured mesh.
            add_shell_lists(report);
            return std::move(report.response);
        }
    });

    register_tool({
        "repair_mesh",
        ToolCategory::Models,
        "Repair a mesh: close holes, open edges",
        "Repair an object's mesh: close its holes (open edges, non-manifold) with the app's own repair -- the "
        "object list's Repair, CGAL-based, on every platform. Each volume made of several shells is split into "
        "one volume per shell; shells with no volume (flat or empty) are deleted; every shell with open edges "
        "has its holes closed. A closed stray shell is not deleted: it becomes a volume of its own "
        "(get_object_components lists shells). The object is then dropped onto the bed. Painting is cleared "
        "unless keep_painting keeps it. One undo step. When there is a repair to apply, an open toolbar tool "
        "(gizmo) is closed first, as the Repair needs, and the answer names it (closed_toolbar_tool); closing "
        "a painting tool records its own undo step, as when the user closes it. Nothing changes when there "
        "is nothing to repair, when the call is "
        "refused or fails, or when the repair takes longer than this call may wait (a little under the "
        "bridge's ORCAMCP_TIMEOUT). The answer: get_mesh_health's numbers before and after, the volumes before "
        "and after, the parts split, dropped and repaired, whether painting was kept, and the object's "
        "placement. Refused while a slice, an export or an upload runs (wait_for_slice first), or an arrange "
        "or orient. A large mesh can take minutes.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
                {"volume_id", {{"type", "integer"}, {"minimum", 0},
                               {"description", "Repair only this volume (0-based, as get_mesh_health's volumes list "
                                               "them), as the object list's Repair does for a selected part. Left out: "
                                               "every volume"}}},
                {"keep_painting", {{"type", "boolean"},
                                   {"description", "Remap the object's painting (colour, supports, seams, fuzzy skin) "
                                                   "onto the repaired mesh instead of clearing it; the app calls this "
                                                   "experimental. Left out: the app's \"Keep painted feature after mesh "
                                                   "change\" setting, which the answer's keep_painting_from names"}}}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json { return repair_mesh(params); }
    });
}
