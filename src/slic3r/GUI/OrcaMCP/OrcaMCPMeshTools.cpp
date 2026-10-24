// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPMeshHealth.hpp"
#include "OrcaMCPMeshRepair.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPTextureDisplacement.hpp"
#include "OrcaMCPPlateSettings.hpp"
#include "OrcaMCPUiJob.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/FixModelByCgal.hpp"
#include "slic3r/Utils/ThreadCancel.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

using Slic3r::CgalRepairInput;
using Slic3r::CgalRepairPlan;
using Slic3r::CgalRepairResult;
using Slic3r::Model;
using Slic3r::ModelObject;

// A repair_mesh call, as its first step leaves it for the next two.
struct RepairRequest
{
    int             object_id           = -1;
    int             volume_id           = -1; // none given: the whole object
    bool            keep_painting       = false;
    bool            keep_painting_given = false;
    nlohmann::json  before;
    TextureState    texture_before;
    CgalRepairInput input;
};

const char* const TEXTURE_REPAIR_CAUSE = "the repair rebuilt the mesh, and texture paint is not carried across";

// Why repair_mesh must not go on now (repair_refusal), read from the app.
std::optional<std::string> app_repair_refusal(Plater& plater)
{
    return repair_refusal(pipeline_state(plater, plater.get_partplate_list().get_plate_count()),
                          !plater.get_ui_job_worker().is_idle(), Slic3r::cgal_repair_dialog_running());
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

    request.before         = repair_facts_json(*object);
    request.texture_before = texture_state_of(*object);
    request.input  = Slic3r::capture_cgal_repair(*object, request.volume_id, request.keep_painting);
    return nullptr;
}

// What the object list's Repair does after changing an object (ObjectList::fix_through_cgal), with the
// plate bookkeeping every MCP transform uses: every instance re-homed, as a new one
// (rehome_and_report_placement, which writes the placement fields into `placement`), where the list
// re-homes instance 0 only.
void update_after_repair(Plater& plater, int object_id, int volume_id, nlohmann::json& placement)
{
    ModelObject& object = *plater.model().objects[std::size_t(object_id)];
    object.ensure_on_bed();
    plater.changed_mesh(object_id);
    ObjectList* list = wxGetApp().obj_list();
    rehome_and_report_placement(placement, object_id, /*moved=*/true);
    list->update_plate_values_for_items();
    list->update_item_error_icon(object_id, volume_id);
    list->update_info_items(std::size_t(object_id));
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

    // The panel's closing can change paint: read the texture state again.
    if (!closed_tool.empty())
        request.texture_before = texture_state_of(*object);
    // The object list's own undo step: nothing above changed the object.
    Plater::TakeSnapshot snapshot(plater, _u8L("Repairing model object"));
    const bool             painted_before = request.before.value("painted", false);
    const CgalRepairResult result         = Slic3r::apply_cgal_repair(*object, request.keep_painting, plan);
    if (!result.error.empty())
        return guard.report(with_closed_tool(error_response("Repairing object " + std::to_string(object_id) + " changed nothing: " +
                                                            result.error),
                                             closed_tool));
    nlohmann::json placement = nlohmann::json::object();
    update_after_repair(*plater, object_id, volume_id, placement);

    nlohmann::json answer = repair_answer_json(object_id, object->name, volume_id, request.keep_painting,
                                               request.keep_painting_given, request.before, repair_facts_json(*object), plan);
    // As the object list's Repair tells the user when a repair cleared painting.
    if (painted_before && !answer["after"].value("painted", false))
        plater->notify_painting_removed();
    answer.update(placement);
    answer["volumes"]         = mesh_volume_rows_json(*object);
    answer["active_warnings"] = get_active_warnings_json(plater);
    answer = guard.report(with_closed_tool(std::move(answer), closed_tool));
    add_texture_changes(answer, texture_changes_json(request.texture_before, {object}, nullptr), object_id, TEXTURE_REPAIR_CAUSE);
    return answer;
}

// ---- repair_mesh of several objects: a plate's Fix All, or Fix All Plates ----

// A repair of several objects, as its first step leaves it for the next two. `items` and `inputs` run in
// the repair's order (repair_order).
struct BatchRepairRequest
{
    RepairScope                  scope = RepairScope::plate;
    std::optional<int>           plate_index;
    bool                         keep_painting       = false;
    bool                         keep_painting_given = false;
    std::vector<BatchRepairItem> items;
    std::vector<CgalRepairInput> inputs;
    std::vector<TextureState>    texture_before; // parallel to items
};

// The model's object indices the scope names, ascending: the plate's objects (ObjectList::fix_all_through_cgal
// takes the plate's get_objects_on_this_plate), or every object. Empty with `error` set when there are none.
std::vector<int> batch_object_indices(Plater& plater, const BatchRepairRequest& request, std::string& error)
{
    PartPlateList&   plates = plater.get_partplate_list();
    std::vector<int> indices;
    if (request.scope == RepairScope::all_plates) {
        for (int i = 0; i < int(plater.model().objects.size()); ++i)
            indices.push_back(i);
    } else {
        if (const auto plate_error = plate_index_error(*request.plate_index, plates.get_plate_count())) {
            error = *plate_error;
            return {};
        }
        for (const ModelObject* object : objects_on_plate(*plates.get_plate(*request.plate_index)))
            indices.push_back(model_object_index(object));
        indices = repair_order(std::move(indices));
    }
    if (indices.empty())
        error = request.scope == RepairScope::all_plates ? "The project has no objects to repair"
                                                         : "Plate " + std::to_string(*request.plate_index) + " has no objects to repair";
    return indices;
}

// Main thread, the first step: the call checked and every object's repair captured. Null to go on, else the answer.
nlohmann::json start_batch(const nlohmann::json& params, BatchRepairRequest& request)
{
    Plater*     plater = wxGetApp().plater();
    std::string error;
    request.keep_painting_given = params.contains("keep_painting");
    if (request.keep_painting_given && !parse_boolean_param(params.at("keep_painting"), request.keep_painting))
        return error_response("keep_painting must be true or false");
    if (!request.keep_painting_given)
        request.keep_painting = wxGetApp().app_config->get_bool("keep_painting");
    if (const auto refusal = app_repair_refusal(*plater))
        return error_response(*refusal);
    const std::vector<int> indices = batch_object_indices(*plater, request, error);
    if (!error.empty())
        return error_response(error);
    for (int index : indices) {
        const ModelObject& object = *plater->model().objects[std::size_t(index)];
        BatchRepairItem    item;
        item.object_id   = index;
        item.object_name = object.name;
        item.before      = repair_facts_json(object);
        request.items.push_back(std::move(item));
        request.texture_before.push_back(texture_state_of(object));
        request.inputs.push_back(Slic3r::capture_cgal_repair(object, -1, request.keep_painting));
    }
    return nullptr;
}

// The HTTP thread, the second step: each captured object planned in turn within the call's one wait, as the
// app's Cancel keeps the plans made so far. `in_time[i]` is false for the plan that was stopped; later
// objects have no plan.
void plan_batch(std::vector<CgalRepairInput>& inputs, std::chrono::steady_clock::time_point deadline,
                std::vector<std::optional<CgalRepairPlan>>& plans, std::vector<bool>& in_time)
{
    using Clock    = std::chrono::steady_clock;
    const auto stop = stop_at(deadline);
    plans.resize(inputs.size());
    in_time.assign(inputs.size(), false);
    for (std::size_t i = 0; i < inputs.size() && !stop(); ++i) {
        plans[i]   = Slic3r::plan_cgal_repair(std::move(inputs[i]), {}, stop);
        in_time[i] = !plans[i]->canceled && Clock::now() <= deadline;
        if (!in_time[i])
            break;
    }
}

// Whether item `item`'s object is still as its plan captured it (found by the plan, not the index, which the
// app may have moved); sets item.object_id to where it is now.
bool object_unchanged(const Model& model, BatchRepairItem& item)
{
    const int index = Slic3r::cgal_repair_object_index(model, *item.plan);
    if (index < 0)
        return false;
    item.object_id = index;
    return item.plan->applies_to(*model.objects[std::size_t(index)]);
}

// Whether the item's object is still in the model at all.
bool object_exists(const Model& model, const BatchRepairItem& item)
{
    return Slic3r::cgal_repair_object_index(model, *item.plan) >= 0;
}

// Whether the item's plan is to be applied: a repair, or a partly failed one, whose other volumes are repaired.
bool applies(const BatchRepairItem& item)
{
    return item.result == BatchRepairResult::repaired || (item.result == BatchRepairResult::failed && !item.plan->changes_nothing());
}

// Applies one item's plan, as the object list's Repair does after its snapshot. A plan the repair refuses
// at the last step is a failure that changed nothing on its object.
void apply_batch_item(Plater& plater, BatchRepairItem& item, CgalRepairPlan& plan, bool keep_painting, bool& painting_removed)
{
    ModelObject&           object = *plater.model().objects[std::size_t(item.object_id)];
    const CgalRepairResult result = Slic3r::apply_cgal_repair(object, keep_painting, plan);
    if (!result.error.empty()) {
        plan.error  = result.error;
        item.result = BatchRepairResult::failed;
        return;
    }
    if (result.volumes_changed == 0)
        return;
    item.changed = true;
    update_after_repair(plater, item.object_id, -1, item.placement);
    item.after = repair_facts_json(object);
    painting_removed = painting_removed || (item.before.value("painted", false) && !item.after.value("painted", false));
}

// The texture paint the repaired objects lost: texture_changes is a list of {object_id, ...}, one per object that
// lost some, with the info_messages lines and the next steps of each.
void add_batch_texture_changes(nlohmann::json& answer, const BatchRepairRequest& request)
{
    Model&                                   model = wxGetApp().plater()->model();
    std::vector<std::pair<int, nlohmann::json>> lost;
    for (std::size_t i = 0; i < request.items.size(); ++i) {
        const BatchRepairItem& item = request.items[i];
        if (!item.changed || item.object_id < 0 || std::size_t(item.object_id) >= model.objects.size())
            continue;
        // The repaired volumes keep their ModelVolume, with a new id: texture_changes_json finds them by pointer.
        lost.emplace_back(item.object_id, texture_changes_json(request.texture_before[i], {model.objects[std::size_t(item.object_id)]}, nullptr));
    }
    add_objects_texture_changes(answer, lost, TEXTURE_REPAIR_CAUSE);
}

// Main thread, the last step: every plan made in time applied under one undo snapshot, if its object is still
// as captured; the rest left as they are, each with its result.
nlohmann::json finish_batch(BatchRepairRequest& request, std::vector<std::optional<CgalRepairPlan>>& plans,
                            const std::vector<bool>& in_time, std::chrono::steady_clock::time_point call_start,
                            std::chrono::steady_clock::duration wait_cap)
{
    Plater*                   plater = wxGetApp().plater();
    McpDialogSuppressionGuard guard;
    if (const auto refusal = app_repair_refusal(*plater))
        return error_response(*refusal);

    const Model& model = plater->model();
    for (std::size_t i = 0; i < request.items.size(); ++i) {
        BatchRepairItem& item = request.items[i];
        item.plan             = plans[i] ? &*plans[i] : nullptr;
        const bool exists = item.plan == nullptr || object_exists(model, item);
        item.result = batch_repair_result(item.plan, in_time[i], item.plan != nullptr && object_unchanged(model, item), exists);
    }

    std::string closed_tool;
    if (std::any_of(request.items.begin(), request.items.end(), applies)) {
        closed_tool = close_open_toolbar_tool(*plater);
        if (!closed_tool.empty() && toolbar_tool_open(*plater))
            return guard.report(with_closed_tool(error_response("The toolbar tool " + closed_tool +
                                                                " is open in the app and did not close, so nothing was repaired"),
                                                 closed_tool));
        // Closing the tool may have changed an object: such a plan is no longer applied.
        for (BatchRepairItem& item : request.items)
            if (applies(item) && !object_unchanged(model, item))
                item.result = object_exists(model, item) ? BatchRepairResult::changed_meanwhile : BatchRepairResult::deleted_meanwhile;
    }

    // The panel's closing can change paint: read the texture state of what is applied again.
    if (!closed_tool.empty())
        for (std::size_t i = 0; i < request.items.size(); ++i)
            if (applies(request.items[i]))
                request.texture_before[i] = texture_state_of(*model.objects[std::size_t(request.items[i].object_id)]);

    bool painting_removed = false;
    if (std::any_of(request.items.begin(), request.items.end(), applies)) {
        Plater::TakeSnapshot snapshot(plater, _u8L("Repairing model object"));
        for (std::size_t i = 0; i < request.items.size(); ++i) {
            if (!applies(request.items[i]))
                continue;
            // Applying runs on the main thread, past the wait the plans were held to: stop before the bridge's timeout.
            if (!batch_apply_time_left(std::chrono::steady_clock::now() - call_start, wait_cap)) {
                request.items[i].result = BatchRepairResult::timed_out;
                continue;
            }
            apply_batch_item(*plater, request.items[i], *plans[i], request.keep_painting, painting_removed);
        }
    }
    if (painting_removed)
        plater->notify_painting_removed();

    const double wait_cap_s = std::chrono::duration<double>(wait_cap).count();
    nlohmann::json answer = batch_repair_answer_json(request.scope, request.plate_index, request.keep_painting, request.keep_painting_given,
                                                     request.items, wait_cap_s);
    std::vector<int> timed_out;
    for (const BatchRepairItem& item : request.items)
        if (item.result == BatchRepairResult::timed_out)
            timed_out.push_back(item.object_id);
    add_next_steps(answer, repair_timed_out_next_steps(timed_out));
    answer["active_warnings"] = get_active_warnings_json(plater);
    answer                    = guard.report(with_closed_tool(std::move(answer), closed_tool));
    add_batch_texture_changes(answer, request);
    return answer;
}

// repair_mesh for a plate or the project: the same three steps as one object's, over every captured object, the
// plans made one after another within the call's one wait.
nlohmann::json repair_objects_tool(const nlohmann::json& params, RepairScope scope, std::optional<int> plate_index)
{
    using Clock                    = std::chrono::steady_clock;
    const auto                     call_start = Clock::now(); // before the capture: the wait cap counts from the call's start
    const auto                     wait_cap   = tool_wait_cap();
    BatchRepairRequest request;
    request.scope       = scope;
    request.plate_index = plate_index;
    const nlohmann::json refusal = run_on_main_thread([&]() -> nlohmann::json { return start_batch(params, request); });
    if (!refusal.is_null())
        return refusal;

    const auto                                 deadline = call_start + wait_cap;
    std::vector<std::optional<CgalRepairPlan>> plans;
    std::vector<bool>                          in_time;
    plan_batch(request.inputs, deadline, plans, in_time);
    return run_on_main_thread([&]() -> nlohmann::json { return finish_batch(request, plans, in_time, call_start, wait_cap); });
}

// repair_one_object_tool: capture on the main thread, the repair on this (the HTTP) thread, apply on the main
// thread. The main thread never waits for the repair, and only it changes the model. The work stops,
// within a volume too, when the app quits (the gate closes, and the next run_on_main_thread refuses with
// -32002, nothing applied) or when this call's wait runs out.
nlohmann::json repair_one_object_tool(const nlohmann::json& params)
{
    RepairRequest        request;
    const nlohmann::json refusal = run_on_main_thread([&params, &request]() -> nlohmann::json { return start_repair(params, request); });
    if (!refusal.is_null())
        return refusal;

    using Clock                = std::chrono::steady_clock;
    const auto     wait_cap    = tool_wait_cap();
    const auto     deadline    = Clock::now() + wait_cap;
    CgalRepairPlan plan        = Slic3r::plan_cgal_repair(std::move(request.input), {},
                                                          stop_at(deadline));
    const bool     past_cap    = Clock::now() > deadline;
    const double   wait_cap_s  = std::chrono::duration<double>(wait_cap).count();
    return run_on_main_thread([&]() -> nlohmann::json { return finish_repair(request, plan, past_cap, wait_cap_s); });
}

// repair_mesh: one object (object_id, with volume_id for one volume), or every object of a plate or of the
// project, as the plate menu's Fix All and Fix All Plates.
nlohmann::json repair_mesh_tool(const nlohmann::json& params)
{
    std::string                  error;
    const std::optional<int>     plate_index = read_plate_index(params, error, "give object_id or all_plates instead");
    const bool                   all_plates  = error.empty() && read_flag(params, "all_plates", error).value_or(false);
    if (!error.empty())
        return error_response(error);
    if (const auto scope_error = repair_scope_error(params.contains("object_id"), params.contains("volume_id"), plate_index.has_value(), all_plates))
        return error_response(*scope_error);
    if (params.contains("object_id"))
        return repair_one_object_tool(params);
    return repair_objects_tool(params, all_plates ? RepairScope::all_plates : RepairScope::plate, plate_index);
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
        "Repair an object's mesh with the app's own repair -- the object list's Repair (Fix Model), on every "
        "platform: each volume is repaired in place, its cracks welded, its shells oriented consistently, its holes "
        "(open edges, non-manifold) filled, its self-intersections removed and its shells of fewer than 4 facets "
        "removed. No volume is split or deleted; a closed stray shell stays in its volume (get_object_components "
        "lists shells). A volume from which no solid can be built (flat or empty) is left as it is, and the answer "
        "says so. The object is then dropped onto the bed. A repaired volume's painting is cleared unless "
        "keep_painting keeps it. One undo step. When there is a repair to apply, an open toolbar tool "
        "(gizmo) is closed first, as the Repair needs, and the answer names it (closed_toolbar_tool); closing "
        "a painting tool records its own undo step, as when the user closes it. Nothing changes when there "
        "is nothing to repair, when the call is "
        "refused or fails, or when the repair takes longer than this call may wait (a little under the "
        "bridge's ORCAMCP_TIMEOUT). The answer: get_mesh_health's numbers before and after, `repair` (volumes "
        "repaired, vertices welded, triangles removed, shells reversed, holes filled, shells removed, "
        "self-intersections removed), whether painting was kept, and the object's placement. Refused while a "
        "slice, an export or an upload runs (wait_for_slice first), or an arrange or orient. A large mesh can "
        "take minutes. Give exactly one of object_id (one object; volume_id for one of its volumes), plate_index "
        "(every object with an instance on that plate, as the plate's Fix All) or all_plates: true (every object in "
        "the project, as Fix All Plates; locked plates are not skipped, as the app does not skip them). For several "
        "objects: they are repaired one after another within this call's one wait; those finished in time are applied "
        "together in one undo step, and the rest are left as they are. The answer's `objects` gives each one's result "
        "-- repaired, nothing_to_repair, failed (some volume no solid can be built from; the others are still "
        "repaired), changed_meanwhile (changed in the app during the repair: left as it is), deleted_meanwhile (deleted in "
        "the app during it) or timed_out (not reached within the wait, or not applied within 10 s past it; next_steps "
        "names repair_mesh for it) -- `counts` as the app counts them, and `repair` summed.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0},
                               {"description", "Object index (0-based): repair this one object. Give object_id, "
                                               "plate_index or all_plates"}}},
                {"plate_index", {{"type", "integer"}, {"minimum", 0},
                                 {"description", "Repair every object with an instance on this plate (0-based, "
                                                 "get_scene_info's plate_index), as the plate's Fix All does. Instead of "
                                                 "object_id; the current plate stays as it is"}}},
                {"all_plates", {{"type", "boolean"},
                                {"description", "true: repair every object in the project -- on every plate, locked "
                                                "ones included, and on none -- as Fix All Plates does. Instead of "
                                                "object_id"}}},
                {"volume_id", {{"type", "integer"}, {"minimum", 0},
                               {"description", "Repair only this volume (0-based, as get_mesh_health's volumes list "
                                               "them), as the object list's Repair does for a selected part. Left out: "
                                               "every volume. Only with object_id"}}},
                {"keep_painting", {{"type", "boolean"},
                                   {"description", "Remap the object's painting (colour, supports, seams, fuzzy skin) "
                                                   "onto the repaired mesh instead of clearing it; the app calls this "
                                                   "experimental. Left out: the app's \"Keep painted feature after mesh "
                                                   "change\" setting, which the answer's keep_painting_from names"}}}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json { return repair_mesh_tool(params); }
    });
}
