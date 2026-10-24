// src/slic3r/GUI/OrcaMCP/OrcaMCPTextureBakeTools.cpp
//
// bake_texture_displacement and prepare_texture_mesh: the two tools that change an object's mesh for texture
// displacement. Both run the texture panel's own work as functions over the model (Jobs/TextureDisplacement*Job.hpp) in
// the repair_mesh pattern (CLAUDE.md, "Mesh repair"): capture on the main thread, plan on the HTTP thread from copies
// only (stopping when the app quits or the call's wait runs out), apply on the main thread when the object is still as
// captured. Their decisions apart from the app are OrcaMCPTextureDisplacement.cpp's.
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPMeshRepair.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPPartTools.hpp"
#include "OrcaMCPSliceProgress.hpp"
#include "OrcaMCPTextureBakeTools.hpp"
#include "OrcaMCPTextureDisplacement.hpp"
#include "OrcaMCPUiJob.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPrepareJob.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/TextureDisplacementObject.hpp"
#include "slic3r/Utils/FixModelByCgal.hpp"
#include "slic3r/Utils/ThreadCancel.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

using Clock = std::chrono::steady_clock;
using Slic3r::ModelObject;
using Slic3r::ModelVolume;
using Slic3r::ObjectID;
using Slic3r::Transform3d;

// Why a texture mesh tool must not go on now: a slice, an export or an upload runs, an arrange or orient runs (which
// includes the panel's own bake and prepare jobs), or the app's own mesh repair does.
std::optional<std::string> texture_job_refusal(Plater& plater, const std::string& tool)
{
    const PipelineState state = pipeline_state(plater, plater.get_partplate_list().get_plate_count());
    if (pipeline_busy(state) != PipelineBusy::idle)
        return pipeline_busy_text(state) + ", so nothing was changed: call wait_for_slice, then " + tool + " again";
    if (const auto refusal = edit_job_refusal(!plater.get_ui_job_worker().is_idle(), tool))
        return refusal;
    if (Slic3r::cgal_repair_dialog_running())
        return "The app is running its own mesh repair, so nothing was changed: call " + tool + " again once it has finished";
    return std::nullopt;
}

int volume_index_of(const ModelObject& object, const ObjectID& id)
{
    for (std::size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i]->id() == id)
            return int(i);
    return -1;
}

const ModelVolume* volume_by_id(const ModelObject& object, const ObjectID& id)
{
    const int index = volume_index_of(object, id);
    return index < 0 ? nullptr : object.volumes[std::size_t(index)];
}

// The captured object's index now (an object deleted meanwhile may have moved it); false when it is gone.
template<class Run> bool rebind_object(Plater& plater, Run& run)
{
    const Slic3r::ModelObjectPtrs& objects = plater.model().objects;
    const auto                     it      = std::find(objects.begin(), objects.end(), run.object);
    if (it == objects.end())
        return false;
    run.object_id = int(it - objects.begin());
    return true;
}

// A plan that ended past the call's wait, or an apply that comes later than the reserve past it, is not applied.
bool past_apply_window(Clock::time_point deadline)
{
    return Clock::now() > deadline + batch_apply_reserve;
}

// What the object list and the plates are owed after a mesh changed under `object`.
void update_object_list(Plater& plater, const ModelObject& object)
{
    const Slic3r::ModelObjectPtrs& objects = plater.model().objects;
    const auto                     it      = std::find(objects.begin(), objects.end(), &object);
    if (it != objects.end())
        wxGetApp().obj_list()->update_info_items(std::size_t(it - objects.begin()));
}

// The panel's closing of an open toolbar tool, right before a change. Null to go on, else the error answer.
std::optional<nlohmann::json> close_tool_before(Plater& plater, int object_id, std::size_t volumes, std::string& closed_tool)
{
    return close_toolbar_tool_for_change(plater, object_id, volumes, closed_tool);
}

// Paint an open toolbar tool (the texture panel) has not written into the model yet: when no part is painted and a tool is
// open, closes it (its own undo step, `closed_tool` names it) and looks again. Null to go on, else the error answer.
std::optional<nlohmann::json> close_when_unpainted(Plater& plater, ModelObject& object, int object_id, std::string& closed_tool,
                                                   const std::string& nothing_painted)
{
    if (!Slic3r::GUI::painted_texture_parts(object).empty())
        return std::nullopt;
    if (toolbar_tool_open(plater))
        if (const auto failed = close_tool_before(plater, object_id, object.volumes.size(), closed_tool))
            return failed;
    if (Slic3r::GUI::painted_texture_parts(object).empty())
        return with_closed_tool(error_response(nothing_painted), closed_tool);
    return std::nullopt;
}

// The undo step's name in the app's language (the strings the panel's own steps use).
std::string translated_snapshot_name(const PrepareRequest& request)
{
    switch (request.operation) {
    case PrepareOperation::remesh: return _u8L("Remesh model for texture displacement");
    case PrepareOperation::smooth: return _u8L("Smooth texture displacement");
    case PrepareOperation::subdivide: break;
    }
    return request.uniform() ? _u8L("Subdivide model for texture displacement") : _u8L("Adaptive subdivide for texture displacement");
}

// ---- bake_texture_displacement ----

// A bake_texture_displacement call, as its capture leaves it for the plan and the apply.
struct BakeRun
{
    BakeRequest                        request;
    int                                object_id = -1; // the object's index when captured; finish finds it again
    const ModelObject*                 object    = nullptr;
    Slic3r::TextureDisplacementOptions stored, used; // the stack's options now, and with the call applied
    TextureDisplacementBakeInput       captured;     // the model as it was: what applies_to compares with
    TextureDisplacementBakeInput       work;         // what the bake runs on: the captured input with the used options
    TextureDisplacementPrepareInput    prepare;      // classic, first phase: the preparation that comes first
    std::vector<int>                   volume_ids;   // the painted parts' place in the object
    std::vector<std::string>           names;
    std::vector<std::size_t>           facets_before;
    Clock::time_point                  deadline;
    bool                               classic() const { return !used.pipeline_v2; }
};

// Whether the resolution this bake used (the stack's, or the automatic one of its coarsest part) carries its texture.
ResolutionAdvice bake_resolution_advice(const BakeRun& run)
{
    std::vector<AdvicePart> parts;
    for (const TextureDisplacementBakeVolume& volume : run.work.volumes)
        parts.push_back({&volume.base_mesh, volume.volume_to_world, volume.model_uv.empty() ? nullptr : &volume.model_uv});
    std::vector<Slic3r::TextureDisplacementLayer> layers;
    if (!run.work.volumes.empty())
        layers = run.work.volumes.front().layers;
    return resolution_advice(layers, parts, run.used.v2_refine_mm);
}

// What the plan made, for the apply.
struct BakePlanned
{
    TextureBakePlan                bake;
    TextureDisplacementPreparePlan prepare;
    bool                           timed_out  = false;
    bool                           paint_lost = false;
};

// Main thread, the first step: the call checked and what the bake reads captured. Null to go on, else the answer.
// A classic bake runs in two phases, as the panel's Standard Bake does: the preparation (`bake_phase` false) is
// planned and applied first, then the bake reads the model as the preparation left it (`bake_phase` true).
nlohmann::json capture_bake(const nlohmann::json& params, BakeRun& run, bool bake_phase, std::string& closed_tool)
{
    Plater*     plater = wxGetApp().plater();
    std::string error;
    ModelObject* object = resolve_object_id(params, plater->model(), run.object_id, error);
    if (object == nullptr)
        return error_response(error);
    if (const auto refusal = texture_job_refusal(*plater, "bake_texture_displacement"))
        return error_response(*refusal);
    const ModelVolume* owner = Slic3r::GUI::texture_stack_owner(*object);
    if (owner == nullptr)
        return error_response("Object " + std::to_string(run.object_id) + " has no model part, so it holds no texture layers");
    const bool has_image = std::any_of(owner->texture_displacement_layers.begin(), owner->texture_displacement_layers.end(),
                                       [](const Slic3r::TextureDisplacementLayer& layer) { return !layer.empty(); });
    if (!has_image)
        return error_response("Object " + std::to_string(run.object_id) + " has no texture layer with an image, so there is nothing to bake: "
                              "set_texture_displacement adds a layer");
    // Paint an open panel has not written into the model yet: close it, then look again.
    if (const auto failed = close_when_unpainted(*plater, *object, run.object_id, closed_tool, bake_nothing_painted_message(run.object_id)))
        return *failed;

    run.object = object;
    run.stored = owner->texture_displacement_options;
    run.used   = bake_options(run.stored, run.request);

    const Slic3r::TextureColorSettings color = bake_color_settings(*plater, owner->texture_displacement_layers, run.used);
    TextureDisplacementBakeInput captured = capture_texture_bake(*object, color, /*take_snapshot=*/false);
    if (captured.volumes.empty())
        return error_response(bake_nothing_painted_message(run.object_id));
    run.volume_ids.clear();
    run.names.clear();
    run.facets_before.clear();
    for (const TextureDisplacementBakeVolume& volume : captured.volumes) {
        const int index = volume_index_of(*object, volume.volume_id);
        run.volume_ids.push_back(index);
        run.names.push_back(index >= 0 ? object->volumes[std::size_t(index)]->name : std::string());
        run.facets_before.push_back(volume.base_mesh.indices.size());
    }
    if (run.classic() && !bake_phase) {
        // The preparation first (remesh, adaptive subdivision, step cut). Only it is planned now: the bake is captured
        // afterwards, from the model as the preparation leaves it (it re-homes the object on the bed).
        run.prepare = capture_texture_prepare(*object, Slic3r::GUI::GLGizmoTextureDisplacement::standard_prepare_params(classic_bake_budget_k(run.request)),
                                              TexturePrepareScope::PaintedParts, color, std::string());
        return nullptr;
    }
    run.captured = std::move(captured);
    run.work     = run.captured;
    for (TextureDisplacementBakeVolume& volume : run.work.volumes)
        volume.options = run.used;
    return nullptr;
}

// HTTP thread: the preparation of a classic bake, or the displacement of every part, from copies only.
BakePlanned plan_bake(BakeRun& run, bool bake_phase, const std::function<bool()>& stop)
{
    BakePlanned planned;
    const auto  progress = [&stop](int) { return !stop(); };
    if (run.classic() && !bake_phase) {
        planned.prepare = plan_texture_prepare(run.prepare, progress);
        planned.timed_out = planned.prepare.canceled;
        for (const Slic3r::TextureDisplacementPrepareResult& result : planned.prepare.results)
            planned.paint_lost |= result.paint_lost;
    } else {
        planned.bake      = plan_texture_bake(run.work, progress);
        planned.timed_out = planned.bake.canceled;
    }
    planned.timed_out |= Clock::now() > run.deadline;
    return planned;
}

std::vector<std::size_t> parts_to_commit(const BakeRun& run, const BakePlanned& planned)
{
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < planned.bake.results.size(); ++i)
        if (texture_bake_moved(run.work, planned.bake, i))
            out.push_back(i);
    return out;
}

// What both phases check first on the main thread. Null to go on, else the answer (closed_tool carried by the caller).
std::optional<nlohmann::json> bake_apply_refusal(BakeRun& run, bool timed_out, double wait_s)
{
    Plater* plater = wxGetApp().plater();
    if (timed_out || past_apply_window(run.deadline)) {
        nlohmann::json answer = error_response(bake_timed_out_message(wait_s, run.classic()));
        answer["timed_out"]   = true;
        return answer;
    }
    if (const auto refusal = texture_job_refusal(*plater, "bake_texture_displacement"))
        return error_response(*refusal);
    if (!rebind_object(*plater, run))
        return error_response("Object " + std::to_string(run.object_id) + " was deleted in the app during the bake, so nothing was changed");
    return std::nullopt;
}

std::string bake_changed_meanwhile(const BakeRun& run)
{
    return "Object " + std::to_string(run.object_id) + " changed in the app during the bake, so nothing was changed: call bake_texture_displacement again";
}

// Main thread, a classic bake's first phase: the preparation applied when the object is still as captured. The undo step
// of the whole bake opens here (the bake that follows commits into it, as the panel's does). `proceed` is set when the
// bake may follow; `applied` says the preparation changed the model. Otherwise the answer; `replan` asks for a new plan.
nlohmann::json finish_bake_prepare(BakeRun& run, BakePlanned& planned, double wait_s, bool may_close, std::string& closed_tool, bool& replan,
                                   bool& proceed, bool& applied)
{
    Plater*                   plater = wxGetApp().plater();
    McpDialogSuppressionGuard guard;
    const auto                finish = [&](nlohmann::json result) { return guard.report(with_closed_tool(std::move(result), closed_tool)); };
    if (const auto refusal = bake_apply_refusal(run, planned.timed_out, wait_s))
        return finish(*refusal);
    ModelObject& object = *const_cast<ModelObject*>(run.object);
    if (!texture_prepare_applies_to(object, run.prepare, TexturePrepareScope::PaintedParts))
        return finish(error_response(bake_changed_meanwhile(run)));
    if (planned.paint_lost)
        return finish(error_response(prepare_paint_lost_message()));
    const bool any = std::any_of(planned.prepare.results.begin(), planned.prepare.results.end(),
                                 [](const Slic3r::TextureDisplacementPrepareResult& r) { return !r.mesh.indices.empty(); });
    if (!any) {
        proceed = true; // the mesh is fine as it is: the bake alone
        return nlohmann::json();
    }
    if (may_close) {
        if (const auto failed = close_tool_before(*plater, run.object_id, object.volumes.size(), closed_tool))
            return guard.report(with_closed_tool(*failed, closed_tool));
        if (!closed_tool.empty() && !texture_prepare_applies_to(object, run.prepare, TexturePrepareScope::PaintedParts)) {
            replan = true;
            return nlohmann::json();
        }
    }
    plater->take_snapshot(_u8L("Bake texture displacement"));
    // The options the bake uses are the stack's from now on, as the panel's controls leave them.
    Slic3r::GUI::texture_stack_owner(object)->texture_displacement_options = run.used;
    const TextureDisplacementPrepareApplied result = apply_texture_prepare(*plater, run.prepare, planned.prepare);
    if (result.outcome != TextureDisplacementPrepareOutcome::Committed)
        return finish(error_response("The app did not apply the preparation, so nothing was changed"));
    applied = proceed = true;
    return nlohmann::json();
}

// Main thread: the bake applied when the object is still as captured, under one undo step (the one a classic bake's
// preparation opened, `snapshot_taken`, else its own). `may_close` lets an open toolbar tool close first (only now that the
// call changes something); closing flushes its paint into the model, so when that changed what was planned `replan` is set
// and nothing is applied.
nlohmann::json finish_bake(BakeRun& run, BakePlanned& planned, double wait_s, bool may_close, std::string& closed_tool, bool& replan,
                           bool snapshot_taken)
{
    Plater*                   plater = wxGetApp().plater();
    McpDialogSuppressionGuard guard;
    const auto                finish = [&](nlohmann::json result) {
        if (snapshot_taken && result.value("status", "") == "error") {
            // The classic preparation was applied by the first phase: say what is still true.
            result["preparation_applied"] = true;
            result["message"]             = result.value("message", std::string()) +
                                            " The classic preparation (remesh and refinement) was already applied; undo reverts it.";
        }
        return guard.report(with_closed_tool(std::move(result), closed_tool));
    };
    if (const auto refusal = bake_apply_refusal(run, planned.timed_out, wait_s))
        return finish(*refusal);
    if (!texture_bake_applies_to(plater->model(), run.captured))
        return finish(error_response(bake_changed_meanwhile(run)));

    std::vector<std::size_t> commit = parts_to_commit(run, planned);
    if (commit.empty()) {
        nlohmann::json answer = error_response(bake_no_displacement_message());
        answer["changed"]     = false;
        return finish(std::move(answer));
    }

    ModelObject& object = *const_cast<ModelObject*>(run.object);
    if (may_close) {
        if (const auto failed = close_tool_before(*plater, run.object_id, object.volumes.size(), closed_tool))
            return guard.report(with_closed_tool(*failed, closed_tool));
        if (!closed_tool.empty() && !texture_bake_applies_to(plater->model(), run.captured)) {
            replan = true;
            return nlohmann::json();
        }
    }

    std::vector<bool> colored(planned.bake.results.size(), false);
    for (std::size_t i = 0; i < colored.size(); ++i)
        colored[i] = !planned.bake.results[i].triangle_color.empty();

    TextureBakeOutcome outcome;
    if (snapshot_taken) {
        outcome = apply_texture_bake(*plater, run.work, planned.bake);
    } else {
        Plater::TakeSnapshot snapshot(plater, _u8L("Bake texture displacement"));
        // The options the bake used are the stack's from now on, as the panel's controls leave them.
        Slic3r::GUI::texture_stack_owner(object)->texture_displacement_options = run.used;
        outcome = apply_texture_bake(*plater, run.work, planned.bake);
    }

    // The answer, from what the bake committed.
    const bool changed = !outcome.committed.empty();
    nlohmann::json answer = {{"status", "success"}, {"object_id", run.object_id}, {"pipeline", run.used.pipeline_v2 ? "default" : "classic"},
                             {"options", stack_options_json(run.used)}};
    const nlohmann::json inactive = bake_inactive_settings(run.request, run.classic());
    if (!inactive.empty())
        answer["inactive_settings"] = inactive;
    nlohmann::json parts = nlohmann::json::array(), unmoved = nlohmann::json::array();
    std::size_t    committed_next = 0;
    for (std::size_t i = 0; i < run.work.volumes.size(); ++i) {
        BakePartReport report;
        report.volume_id     = run.volume_ids[i];
        report.name          = run.names[i];
        report.facets_before = run.facets_before[i];
        report.facets_after  = run.work.volumes[i].base_mesh.indices.size();
        const bool moved     = std::find(commit.begin(), commit.end(), i) != commit.end();
        if (moved && committed_next < outcome.committed.size()) {
            report.baked   = true;
            report.colored = colored[i];
            if (const ModelVolume* volume = volume_by_id(object, outcome.committed[committed_next]))
                report.facets_after = volume->mesh().its.indices.size();
            ++committed_next;
        } else {
            unmoved.push_back(report.volume_id);
        }
        parts.push_back(bake_part_json(report));
    }
    answer["parts"]          = parts;
    answer["unmoved_parts"]  = unmoved;
    answer["mixed_filaments"] = bake_mixed_filaments_json(outcome.mixed_filaments, outcome.filaments_before);
    const int slots_added    = static_cast<int>(std::count_if(outcome.mixed_filaments.begin(), outcome.mixed_filaments.end(),
                                                             [&](int slot) { return slot >= outcome.filaments_before; }));
    answer["filaments_added"] = outcome.filaments_after - outcome.filaments_before;
    answer["budget_limited"]  = bake_budget_json(outcome.refined, outcome.budget);
    const ResolutionAdvice advice = bake_resolution_advice(run);
    nlohmann::json         warnings = nlohmann::json::array();
    if (const std::string text = bake_budget_warning(outcome.refined, outcome.budget); !text.empty())
        warnings.push_back(text);
    if (const std::string text = resolution_alias_warning(advice); !text.empty()) {
        answer["recommended_resolution_mm"] = recommended_resolution_json(advice);
        warnings.push_back(text);
    }
    if (!warnings.empty())
        answer["bake_warnings"] = warnings;
    if (slots_added > 0)
        answer["undo_warning"] = bake_undo_warning(slots_added);
    const bool needs_bake = needs_bake_fast(object, Slic3r::GUI::texture_stack_owner(object));
    answer["needs_bake"]  = needs_bake;
    rehome_and_report_placement(answer, run.object_id, /*moved=*/true);
    answer["changed"]         = changed; // what the bake committed, not the re-homing's flag
    answer["active_warnings"] = get_active_warnings_json(plater);
    add_next_steps(answer, baked_texture_next_steps(run.object_id, needs_bake, slots_added, answer.contains("plate_index") && answer["plate_index"].is_number_integer() ?
                                                                                                std::optional<int>(answer["plate_index"].get<int>()) : std::nullopt));
    append_next_steps(answer, budget_limited_next_steps(run.object_id, outcome.refined, outcome.budget, advice.used_mm));
    add_turntable_preview_if_requested(answer, run.request.include_preview);
    return finish(std::move(answer));
}

// A classic bake's bake phase: capture again after the preparation, plan, apply. Up to two rounds, the second only when
// closing an open toolbar tool changed what the first read.
nlohmann::json bake_after_prepare(const nlohmann::json& params, const BakeRequest& request, Clock::time_point deadline, double wait_s,
                                  std::string& closed_tool, bool snapshot_taken, const std::vector<std::size_t>& facets_before)
{
    for (int round = 0; round < 2; ++round) {
        BakeRun run;
        run.request  = request;
        run.deadline = deadline;
        const nlohmann::json refusal = run_on_main_thread([&]() { return capture_bake(params, run, /*bake_phase=*/true, closed_tool); });
        if (!refusal.is_null())
            return with_closed_tool(refusal, closed_tool);
        if (facets_before.size() == run.facets_before.size())
            run.facets_before = facets_before; // before the preparation
        BakePlanned planned = plan_bake(run, /*bake_phase=*/true, stop_at(deadline));
        bool        replan  = false;
        nlohmann::json answer = run_on_main_thread([&]() {
            return finish_bake(run, planned, wait_s, /*may_close=*/round == 0, closed_tool, replan, snapshot_taken);
        });
        if (!replan)
            return answer;
    }
    return error_response("The model changed twice while the bake was planned, so nothing more was changed: call bake_texture_displacement again");
}

nlohmann::json bake_texture_displacement(const nlohmann::json& params)
{
    std::string error;
    const auto  request = read_bake_request(params, error);
    if (!request)
        return error_response(error);

    const auto   wait_cap = tool_wait_cap();
    const auto   deadline = Clock::now() + wait_cap;
    const double wait_s   = std::chrono::duration<double>(wait_cap).count();
    std::string  closed_tool;

    for (int round = 0; round < 2; ++round) {
        BakeRun run;
        run.request  = *request;
        run.deadline = deadline;
        const nlohmann::json refusal = run_on_main_thread([&]() { return capture_bake(params, run, /*bake_phase=*/false, closed_tool); });
        if (!refusal.is_null())
            return with_closed_tool(refusal, closed_tool);

        BakePlanned planned = plan_bake(run, /*bake_phase=*/false, stop_at(deadline));
        bool        replan  = false;
        if (!run.classic()) {
            nlohmann::json answer = run_on_main_thread([&]() { return finish_bake(run, planned, wait_s, /*may_close=*/round == 0, closed_tool, replan, false); });
            if (!replan)
                return answer;
            continue;
        }
        bool proceed = false, applied = false;
        nlohmann::json answer = run_on_main_thread([&]() {
            return finish_bake_prepare(run, planned, wait_s, /*may_close=*/round == 0, closed_tool, replan, proceed, applied);
        });
        if (replan)
            continue;
        if (!proceed)
            return answer;
        return bake_after_prepare(params, *request, deadline, wait_s, closed_tool, applied, run.facets_before);
    }
    return error_response("The model changed twice while the bake was planned, so nothing was changed: call bake_texture_displacement again");
}

// ---- prepare_texture_mesh ----

// What a uniform subdivide reads of one part, copied on the main thread.
struct UniformPart
{
    ObjectID                      id;
    int                           volume_id = -1;
    std::string                   name;
    indexed_triangle_set  its;
    indexed_triangle_set  result;
};

struct PrepareRun
{
    PrepareRequest                  request;
    int                             object_id = -1;
    const ModelObject*              object    = nullptr;
    TexturePrepareScope             scope     = TexturePrepareScope::AllParts;
    TextureDisplacementPrepareInput input;    // remesh, adaptive subdivide
    std::vector<UniformPart>        uniform;  // uniform subdivide
    std::vector<int>                volume_ids;
    std::vector<std::string>        names;
    std::vector<std::size_t>        facets_before;
    bool                            had_texture_paint = false;
    Clock::time_point               deadline;
};

nlohmann::json capture_prepare(const nlohmann::json& params, PrepareRun& run, std::string& closed_tool)
{
    Plater*      plater = wxGetApp().plater();
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), run.object_id, error);
    if (object == nullptr)
        return error_response(error);
    if (const auto refusal = texture_job_refusal(*plater, "prepare_texture_mesh"))
        return error_response(*refusal);
    const ModelVolume* owner = Slic3r::GUI::texture_stack_owner(*object);
    if (owner == nullptr)
        return error_response("Object " + std::to_string(run.object_id) + " has no model part");
    run.object = object;

    const PrepareRequest& request = run.request;
    if (request.operation == PrepareOperation::subdivide && request.adaptive.value_or(true))
        if (const auto failed = close_when_unpainted(*plater, *object, run.object_id, closed_tool,
                                                     "Object " + std::to_string(run.object_id) + " has no texture paint, and adaptive subdivision only refines where it is "
                                                     "painted: paint_object mode texture paints a layer (adaptive: false subdivides every part)"))
            return *failed;
    run.had_texture_paint = needs_bake_fast(*object, owner);

    const Slic3r::PresetBundle& bundle = *wxGetApp().preset_bundle;
    const auto color_for = [&](const Slic3r::TextureDisplacementOptions& options) {
        return Slic3r::GUI::GLGizmoTextureDisplacement::color_settings(
            owner->texture_displacement_layers, options,
            Slic3r::GUI::GLGizmoTextureDisplacement::palette_inputs(options, bundle, plater->get_extruders_colors()), bundle.project_config);
    };

    if (request.operation == PrepareOperation::remesh) {
        run.scope = TexturePrepareScope::AllParts;
        run.input = capture_texture_prepare(*object, remesh_prepare_params(request, default_remesh_edge_mm(owner->mesh().its)), run.scope,
                                            Slic3r::TextureColorSettings{}, translated_snapshot_name(request));
    } else if (request.operation == PrepareOperation::subdivide && !request.uniform()) {
        run.scope = TexturePrepareScope::PaintedParts;
        const Slic3r::TextureDisplacementPrepareParams params = subdivide_prepare_params(request, default_subdivide_edge_mm(owner->mesh().its));
        run.input = capture_texture_prepare(*object, params, run.scope, params.subdiv_color_edge_mm > 0.f ? color_for(owner->texture_displacement_options) : Slic3r::TextureColorSettings{},
                                            translated_snapshot_name(request));
    } else if (request.uniform()) {
        std::size_t facets = 0;
        for (const ModelVolume* part : Slic3r::GUI::texture_parts(*object))
            facets += part->mesh().its.indices.size();
        if (const auto refusal = uniform_subdivision_refusal(facets, *request.steps))
            return error_response(*refusal);
        for (ModelVolume* part : Slic3r::GUI::texture_parts(*object)) {
            UniformPart copy;
            copy.id        = part->id();
            copy.volume_id = volume_index_of(*object, part->id());
            copy.name      = part->name;
            copy.its       = part->mesh().its;
            run.uniform.push_back(std::move(copy));
        }
    }
    for (const TextureDisplacementPrepareVolume& volume : run.input.volumes) {
        const int index = volume_index_of(*object, volume.volume_id);
        run.volume_ids.push_back(index);
        run.names.push_back(index >= 0 ? object->volumes[std::size_t(index)]->name : std::string());
        run.facets_before.push_back(volume.base_mesh.indices.size());
    }
    for (const UniformPart& part : run.uniform) {
        run.volume_ids.push_back(part.volume_id);
        run.names.push_back(part.name);
        run.facets_before.push_back(part.its.indices.size());
    }
    return nullptr;
}

struct PreparePlanned
{
    TextureDisplacementPreparePlan prepare;
    bool                           timed_out  = false;
};

PreparePlanned plan_prepare(PrepareRun& run, const std::function<bool()>& stop)
{
    PreparePlanned planned;
    if (run.request.uniform()) {
        for (UniformPart& part : run.uniform) {
            if (stop()) {
                planned.timed_out = true;
                return planned;
            }
            part.result = Slic3r::subdivide_mesh_uniform(part.its, 0.f, *run.request.steps, [&stop](int) { return !stop(); });
            if (part.result.indices.empty() && !part.its.indices.empty()) { // only a cancel returns no triangles
                planned.timed_out = true;
                return planned;
            }
        }
        return planned;
    }
    planned.prepare   = plan_texture_prepare(run.input, [&stop](int) { return !stop(); });
    planned.timed_out = planned.prepare.canceled;
    return planned;
}

// The report of each part, before and after.
nlohmann::json prepare_parts_json(const PrepareRun& run, const std::vector<std::size_t>& facets_after)
{
    nlohmann::json parts = nlohmann::json::array();
    for (std::size_t i = 0; i < run.volume_ids.size(); ++i)
        parts.push_back({{"volume_id", run.volume_ids[i]}, {"name", run.names[i]}, {"facets_before", run.facets_before[i]},
                         {"facets_after", facets_after[i]}});
    return parts;
}

nlohmann::json prepare_answer_base(const PrepareRun& run, bool changed)
{
    return {{"status", "success"}, {"changed", changed}, {"object_id", run.object_id},
            {"operation", run.request.operation == PrepareOperation::remesh ? "remesh" : run.request.operation == PrepareOperation::smooth ? "smooth" : "subdivide"}};
}

// The change is made: placement, warnings, next steps.
nlohmann::json prepare_answer_end(nlohmann::json answer, const PrepareRun& run, Plater& plater)
{
    const ModelObject& object = *run.object;
    const bool         changed = answer.value("changed", true);
    rehome_and_report_placement(answer, run.object_id, /*moved=*/true);
    answer["changed"] = changed;
    const bool needs_bake = needs_bake_fast(object, Slic3r::GUI::texture_stack_owner(object));
    answer["needs_bake"]  = needs_bake;
    answer["active_warnings"] = get_active_warnings_json(&plater);
    add_next_steps(answer, bake_next_steps(run.object_id, needs_bake));
    return answer;
}

// What both finishers check first on the main thread: a plan that ran out of time, a busy app, an object deleted meanwhile
// (otherwise its index is found again). Null to go on.
std::optional<nlohmann::json> prepare_apply_refusal(PrepareRun& run, bool timed_out, double wait_s)
{
    Plater* plater = wxGetApp().plater();
    if (timed_out || past_apply_window(run.deadline)) {
        nlohmann::json answer = error_response(prepare_timed_out_message(wait_s));
        answer["timed_out"]   = true;
        return answer;
    }
    if (const auto refusal = texture_job_refusal(*plater, "prepare_texture_mesh"))
        return error_response(*refusal);
    if (!rebind_object(*plater, run))
        return error_response("Object " + std::to_string(run.object_id) + " was deleted in the app during the preparation, so nothing was changed");
    return std::nullopt;
}

// Remesh and adaptive subdivide on the main thread: the plan applied when the object is still as captured.
nlohmann::json finish_job_prepare(PrepareRun& run, PreparePlanned& planned, double wait_s, bool may_close, std::string& closed_tool, bool& replan)
{
    Plater*                   plater = wxGetApp().plater();
    McpDialogSuppressionGuard guard;
    const auto                finish = [&](nlohmann::json result) { return guard.report(with_closed_tool(std::move(result), closed_tool)); };

    if (const auto refusal = prepare_apply_refusal(run, planned.timed_out, wait_s))
        return finish(*refusal);
    ModelObject& object = *const_cast<ModelObject*>(run.object);
    const std::string meanwhile = "Object " + std::to_string(run.object_id) + " changed in the app during the preparation, so nothing was changed: "
                                  "call prepare_texture_mesh again";
    if (!texture_prepare_applies_to(object, run.input, run.scope))
        return finish(error_response(meanwhile));
    for (const Slic3r::TextureDisplacementPrepareResult& result : planned.prepare.results)
        if (result.paint_lost)
            return finish(error_response(prepare_paint_lost_message()));
    const bool any = std::any_of(planned.prepare.results.begin(), planned.prepare.results.end(),
                                 [](const Slic3r::TextureDisplacementPrepareResult& r) { return !r.mesh.indices.empty(); });
    if (!any) {
        nlohmann::json answer = prepare_answer_base(run, false);
        answer["message"]     = prepare_unchanged_message(run.request);
        return finish(std::move(answer));
    }
    if (may_close) {
        if (const auto failed = close_tool_before(*plater, run.object_id, object.volumes.size(), closed_tool))
            return guard.report(with_closed_tool(*failed, closed_tool));
        if (!closed_tool.empty() && !texture_prepare_applies_to(object, run.input, run.scope)) {
            replan = true;
            return nlohmann::json();
        }
    }

    const std::vector<bool> prepared = [&] {
        std::vector<bool> out;
        for (const auto& r : planned.prepare.results)
            out.push_back(!r.mesh.indices.empty());
        return out;
    }();
    const TextureDisplacementPrepareApplied applied = apply_texture_prepare(*plater, run.input, planned.prepare);
    if (applied.outcome != TextureDisplacementPrepareOutcome::Committed)
        return finish(error_response("The preparation changed nothing: the app did not apply it"));

    std::vector<std::size_t> after = run.facets_before;
    std::size_t              next  = 0;
    for (std::size_t i = 0; i < after.size(); ++i)
        if (prepared[i] && next < applied.committed.size())
            if (const ModelVolume* volume = volume_by_id(object, applied.committed[next++]))
                after[i] = volume->mesh().its.indices.size();
    nlohmann::json answer = prepare_answer_base(run, true);
    answer["parts"]       = prepare_parts_json(run, after);
    answer["texture_paint"] = "kept";
    return finish(prepare_answer_end(std::move(answer), run, *plater));
}

// A uniform subdivide on the main thread, as the panel's Subdivide runs it.
nlohmann::json finish_uniform_prepare(PrepareRun& run, PreparePlanned& planned, double wait_s, bool may_close, std::string& closed_tool)
{
    Plater*                   plater = wxGetApp().plater();
    McpDialogSuppressionGuard guard;
    const auto                finish = [&](nlohmann::json result) { return guard.report(with_closed_tool(std::move(result), closed_tool)); };

    if (const auto refusal = prepare_apply_refusal(run, planned.timed_out, wait_s))
        return finish(*refusal);
    ModelObject& object = *const_cast<ModelObject*>(run.object);
    const auto   unchanged = [&]() {
        for (const UniformPart& part : run.uniform) {
            const ModelVolume* volume = volume_by_id(object, part.id);
            if (volume == nullptr || volume->mesh().its.indices != part.its.indices || volume->mesh().its.vertices != part.its.vertices)
                return false;
        }
        return Slic3r::GUI::texture_parts(object).size() == run.uniform.size();
    };
    const std::string meanwhile = "Object " + std::to_string(run.object_id) + " changed in the app during the subdivision, so nothing was changed: "
                                  "call prepare_texture_mesh again";
    if (!unchanged())
        return finish(error_response(meanwhile));
    if (may_close) {
        if (const auto failed = close_tool_before(*plater, run.object_id, object.volumes.size(), closed_tool))
            return guard.report(with_closed_tool(*failed, closed_tool));
        if (!closed_tool.empty() && !unchanged())
            return finish(error_response("Closing the toolbar tool " + closed_tool + " changed object " + std::to_string(run.object_id) +
                                         ", so nothing was changed: call prepare_texture_mesh again"));
    }

    std::vector<std::size_t> after;
    {
        Plater::TakeSnapshot snapshot(plater, translated_snapshot_name(run.request), Slic3r::UndoRedo::SnapshotType::GizmoAction);
        for (UniformPart& part : run.uniform) {
            ModelVolume* volume = const_cast<ModelVolume*>(volume_by_id(object, part.id));
            // The panel's dance: the four paint channels follow the new triangles, the texture masks do not and are dropped.
            std::optional<Slic3r::TriangleSelector::SavedPainting> saved = volume->save_painting();
            volume->set_mesh(Slic3r::TriangleMesh(std::move(part.result)));
            volume->set_new_unique_id();
            volume->calculate_convex_hull();
            volume->restore_painting(saved);
            after.push_back(volume->mesh().its.indices.size());
        }
        update_object_list(*plater, object);
        plater->changed_object(object);
    }
    nlohmann::json answer = prepare_answer_base(run, true);
    answer["parts"]       = prepare_parts_json(run, after);
    answer["texture_paint"] = "dropped";
    answer["texture_paint_dropped"] = run.had_texture_paint;
    return finish(prepare_answer_end(std::move(answer), run, *plater));
}

// Smooth, entirely on the main thread as the panel runs it: the relaxation is linear in the facets, and capped
// (SMOOTH_MAX_FACETS) so the app is not held for long.
nlohmann::json smooth_texture_mesh(const nlohmann::json& params, const PrepareRequest& request)
{
    Plater*                   plater = wxGetApp().plater();
    McpDialogSuppressionGuard guard;
    PrepareRun                run;
    run.request = request;
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), run.object_id, error);
    if (object == nullptr)
        return guard.report(error_response(error));
    if (const auto refusal = texture_job_refusal(*plater, "prepare_texture_mesh"))
        return guard.report(error_response(*refusal));
    ModelVolume* owner = Slic3r::GUI::texture_stack_owner(*object);
    if (owner == nullptr)
        return guard.report(error_response("Object " + std::to_string(run.object_id) + " has no model part"));
    run.object = object;

    const std::size_t                        facets = owner->mesh().its.indices.size();
    const Slic3r::TextureDisplacementOptions used   = smooth_options(owner->texture_displacement_options, request.smooth);
    if (const auto refusal = smooth_refusal(facets))
        return guard.report(error_response(*refusal));
    if (used.smooth_strength <= 0.f || used.smooth_iterations <= 0) {
        nlohmann::json answer = prepare_answer_base(run, false);
        answer["message"]     = "Nothing to smooth: the stack's smoothing strength or pass count is 0; give strength and passes";
        return guard.report(std::move(answer));
    }

    // Paint an open panel holds is read first: it is closed (its own undo step) before the vertices to relax are worked out, once.
    std::string closed_tool;
    if (toolbar_tool_open(*plater))
        if (const auto failed = close_tool_before(*plater, run.object_id, object->volumes.size(), closed_tool))
            return guard.report(with_closed_tool(*failed, closed_tool));
    Slic3r::GUI::TextureSmoothSkip              why      = Slic3r::GUI::TextureSmoothSkip::None;
    std::optional<indexed_triangle_set> smoothed = Slic3r::GUI::smoothed_baked_mesh(*owner, used, &why);
    if (!smoothed && why == Slic3r::GUI::TextureSmoothSkip::NothingPainted)
        return guard.report(with_closed_tool(error_response("Object " + std::to_string(run.object_id) + " has no texture paint, and smoothing only touches the painted "
                                                            "part of the model: paint_object mode texture paints a layer"),
                                             closed_tool));
    if (!smoothed) {
        nlohmann::json answer = prepare_answer_base(run, false);
        answer["message"]     = "Nothing to smooth - the painted area is only one triangle deep, so with ignore_outer_ring on there are no interior "
                                "vertices to relax.";
        return guard.report(with_closed_tool(std::move(answer), closed_tool));
    }

    {
        Plater::TakeSnapshot snapshot(plater, translated_snapshot_name(request), Slic3r::UndoRedo::SnapshotType::GizmoAction);
        owner->texture_displacement_options = used;
        // Only vertices move: every paint channel still means what it did, so the texture masks are put back as they were.
        std::optional<Slic3r::TriangleSelector::SavedPainting> saved_painting = owner->save_painting();
        std::array<Slic3r::TriangleSelector::TriangleSplittingData, Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS> saved_texture;
        for (int i = 0; i < int(Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
            saved_texture[std::size_t(i)] = owner->texture_displacement_facet(i).get_data();
        owner->set_mesh(Slic3r::TriangleMesh(std::move(*smoothed)));
        owner->set_new_unique_id();
        owner->calculate_convex_hull();
        owner->restore_painting(saved_painting);
        for (int i = 0; i < int(Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
            owner->texture_displacement_facet(i).set_data(std::move(saved_texture[std::size_t(i)]));
        update_object_list(*plater, *object);
        plater->changed_object(*object);
    }
    run.volume_ids    = {volume_index_of(*object, owner->id())};
    run.names         = {owner->name};
    run.facets_before = {facets};
    nlohmann::json answer = prepare_answer_base(run, true);
    answer["parts"]       = prepare_parts_json(run, {facets});
    answer["texture_paint"] = "kept";
    answer["options"]       = stack_options_json(used);
    return guard.report(with_closed_tool(prepare_answer_end(std::move(answer), run, *plater), closed_tool));
}

nlohmann::json prepare_texture_mesh(const nlohmann::json& params)
{
    std::string error;
    const auto  request = read_prepare_request(params, error);
    if (!request)
        return error_response(error);
    if (request->operation == PrepareOperation::smooth)
        return run_on_main_thread([&]() { return smooth_texture_mesh(params, *request); });

    const auto   wait_cap = tool_wait_cap();
    const auto   deadline = Clock::now() + wait_cap;
    const double wait_s   = std::chrono::duration<double>(wait_cap).count();
    std::string  closed_tool;

    for (int round = 0; round < 2; ++round) {
        PrepareRun run;
        run.request  = *request;
        run.deadline = deadline;
        const nlohmann::json refusal = run_on_main_thread([&]() { return capture_prepare(params, run, closed_tool); });
        if (!refusal.is_null())
            return with_closed_tool(refusal, closed_tool);

        PreparePlanned planned = plan_prepare(run, stop_at(deadline));
        planned.timed_out |= Clock::now() > deadline;
        bool           replan  = false;
        nlohmann::json answer  = run_on_main_thread([&]() {
            return run.request.uniform() ? finish_uniform_prepare(run, planned, wait_s, /*may_close=*/true, closed_tool) :
                                           finish_job_prepare(run, planned, wait_s, /*may_close=*/round == 0, closed_tool, replan);
        });
        if (!replan)
            return answer;
    }
    return error_response("The model changed twice while the preparation was planned, so nothing was changed: call prepare_texture_mesh again");
}

// ---- Schemas ----

nlohmann::json bounded_number_property(const SettingBound* bound, const char* type, const std::string& description)
{
    nlohmann::json out = {{"type", type}, {"description", description}};
    if (bound != nullptr) {
        out["minimum"] = bound->min;
        out["maximum"] = bound->max;
    }
    return out;
}

nlohmann::json bake_schema()
{
    const auto b = [](const char* key) { return bake_setting_bound(key); };
    return {
        {"type", "object"},
        {"properties", {
            {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
            {"pipeline", {{"type", "string"}, {"enum", {"default", "classic"}},
                          {"description", "default: refine, displace and simplify in one run. classic: remesh and refine first (classic_budget_k), then "
                                          "move the existing vertices, keeping the topology. Left out: the stack's"}}},
            {"resolution_mm", {{"type", {"number", "null"}},
                               {"description", "Default pipeline: the refinement's edge length, 0.02 to 2 mm; 0 or null is automatic. Left out: the stack's"}}},
            {"budget_k", {{"type", {"integer", "null"}},
                          {"description", "Default pipeline: the triangle budget in thousands, 0 to 4000 (0 skips simplification); -1 or null is "
                                           "automatic. Left out: the stack's"}}},
            {"classic_budget_k", bounded_number_property(b("classic_budget_k"), "integer",
                                                         "Classic pipeline: the subdivision budget in thousands of triangles (default 750). Not stored")},
            {"displace_border", {{"type", "boolean"},
                                 {"description", "Let the relief run to the edge of the painted area (default true, as the Standard bake does)"}}},
            {"smooth", {{"type", "object"}, {"additionalProperties", false},
                        {"description", "Relax the displaced surface after the texture is applied (default off, as the Standard bake does)"},
                        {"properties", {
                            {"enabled", {{"type", "boolean"}, {"description", "Smooth the result"}}},
                            {"strength", bounded_number_property(b("strength"), "number", "Percent a vertex moves toward its neighbours' average each pass")},
                            {"passes", bounded_number_property(b("passes"), "integer", "Passes")},
                            {"ignore_outer_ring", {{"type", "boolean"}, {"description", "Hold the painted patch's rim out of the smoothing"}}}
                        }}}},
            {"color_mix", {{"type", "boolean"}, {"description", "Stack-wide: interleave filament pairs to reach colours between them"}}},
            {"color_mix_count", bounded_number_property(b("color_mix_count"), "integer", "Stack-wide: the most mixed colors the palette may offer")},
            {"color_denoise", bounded_number_property(b("color_denoise"), "integer", "Stack-wide: passes that clear colour specks")},
            {"include_preview", {{"type", "boolean"}, {"description", "Add a turntable preview of the result"}}}
        }},
        {"required", {"object_id"}}
    };
}

nlohmann::json prepare_schema()
{
    const auto b = [](const char* key) { return prepare_setting_bound(key); };
    return {
        {"type", "object"},
        {"properties", {
            {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
            {"operation", {{"type", "string"}, {"enum", {"remesh", "subdivide", "smooth"}},
                           {"description", "remesh: rebuild every model part with an even triangle size. subdivide: add triangles, adaptively where "
                                           "painted or uniformly. smooth: relax the painted area's baked vertices"}}},
            {"target_edge_mm", bounded_number_property(b("target_edge_mm"), "number", "remesh: the triangle edge length (default: the first part's mean edge)")},
            {"keep_sharp_edges", {{"type", "boolean"}, {"description", "remesh: hold hard edges in place (default true)"}}},
            {"sharp_angle_deg", bounded_number_property(b("sharp_angle_deg"), "number", "remesh: how sharp a fold must be to be kept (default 40)")},
            {"adaptive", {{"type", "boolean"}, {"description", "subdivide: refine the painted area only (default true); false splits every triangle of every part by steps"}}},
            {"steps", bounded_number_property(b("steps"), "integer", "subdivide with adaptive false: how many times every triangle is split in four (required then)")},
            {"max_edge_mm", bounded_number_property(b("max_edge_mm"), "number", "adaptive subdivide: no painted triangle stays larger (default: half the first part's mean edge)")},
            {"detail_mm", bounded_number_property(b("detail_mm"), "number", "adaptive subdivide with follow_texture: how far the mesh may sit from the texture's shape (default 0.05)")},
            {"min_edge_mm", bounded_number_property(b("min_edge_mm"), "number", "adaptive subdivide with follow_texture: the smallest triangle (default 0.1)")},
            {"edge_detail_mm", bounded_number_property(b("edge_detail_mm"), "number", "adaptive subdivide: triangle size along the painted area's outline, 0 off (default 0.4)")},
            {"color_detail_mm", bounded_number_property(b("color_detail_mm"), "number", "adaptive subdivide: triangle size where two colours meet, 0 off (default 0.3)")},
            {"follow_texture", {{"type", "boolean"}, {"description", "adaptive subdivide: spend triangles where the texture bends (default false)"}}},
            {"budget_k", bounded_number_property(b("budget_k"), "integer", "adaptive subdivide: triangles to add, in thousands (default 750)")},
            {"strength", bounded_number_property(b("strength"), "number", "smooth: percent a vertex moves toward its neighbours' average each pass (default: the stack's)")},
            {"passes", bounded_number_property(b("passes"), "integer", "smooth: passes (default: the stack's)")},
            {"ignore_outer_ring", {{"type", "boolean"}, {"description", "smooth: hold the painted patch's rim out (default: the stack's)"}}}
        }},
        {"required", {"object_id", "operation"}}
    };
}

} // namespace

void OrcaMCPServer::register_texture_bake_tools()
{
    register_tool({
        "bake_texture_displacement",
        ToolCategory::TextureDisplacement,
        "Bake texture layers into the mesh",
        "Turn an object's painted texture layers into real geometry, as the texture panel's Bake does, on every painted model part: "
        "each part's mesh is rebuilt with the relief (classic: remeshed and refined first, then displaced), its other paint carried "
        "across, a colouring layer's colours written as colour paint (adding mixed filament slots when its mixes need them), the baked "
        "layers' paint cleared and the layers kept, and the object dropped onto the bed. The options are stored in the stack, as the "
        "panel's controls store them; left out, displace_border is true and smooth off (the Standard bake), the rest as the stack has "
        "them, and get_texture_displacement's stack_options are accepted back as they are (a null resolution_mm or budget_k is "
        "automatic). One undo step; the mixed filament slots a colour bake adds are not undone with it (undo_warning says so, "
        "delete_mixed_filament removes one). Answers each part's facets before and after, the options used, the mixed slots, "
        "budget_limited when the triangle budget cut detail, unmoved_parts (a part the bake did not move keeps its paint) and the "
        "object's placement; the next step is slice_all. Runs in the call's wait (a little under the bridge's ORCAMCP_TIMEOUT): past it "
        "nothing changes. A classic bake applies its preparation first and then bakes the result into the same undo step: if the bake then fails the preparation stays (preparation_applied). A bake that moves nothing changes nothing. A plan that ends past the wait is never applied. Refused while a slice, an export, an arrange or orient, or the "
        "app's own bake or repair runs, or when nothing is painted; an open toolbar tool is closed first, only when the bake will change something.",
        bake_schema(),
        [](const nlohmann::json& params) -> nlohmann::json {
            return bake_texture_displacement(params);
        }
    });

    register_tool({
        "prepare_texture_mesh",
        ToolCategory::TextureDisplacement,
        "Remesh, subdivide or smooth for textures",
        "Get an object's mesh ready for texture displacement, as the texture panel's Pro mode tools do. remesh rebuilds every model part "
        "with triangles near target_edge_mm (the painted area is carried onto the new triangles). subdivide with adaptive (default) "
        "refines the painted area only, to max_edge_mm and, with follow_texture, to the texture's detail within the budget_k; with "
        "adaptive false it splits every triangle of every part in four, steps times (more than 1 million triangles after the split is refused: the paint is remapped on the main thread), "
        "which drops the texture paint (texture_paint says so). smooth relaxes the baked vertices of the painted area with the "
        "stack's smoothing (strength, passes, ignore_outer_ring, stored when given); it runs on the app's main thread, so a part of more than 1 million triangles is refused, and an open panel is closed first. Runs in the call's wait (a little under the "
        "bridge's ORCAMCP_TIMEOUT): past it nothing changes. Answers each part's facets before and after. One undo step named as "
        "the panel names it; nothing to change is changed: false with the panel's words. Refused while a slice, an export, an "
        "arrange or orient, or the app's own bake or repair runs; an open toolbar tool is closed first, only when the call will change something.",
        prepare_schema(),
        [](const nlohmann::json& params) -> nlohmann::json {
            return prepare_texture_mesh(params);
        }
    });
}

namespace Slic3r { namespace GUI { namespace OrcaMCP {

TextureColorSettings bake_color_settings(Plater& plater, const std::vector<TextureDisplacementLayer>& layers, const TextureDisplacementOptions& options)
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    return GLGizmoTextureDisplacement::color_settings(
        layers, options, GLGizmoTextureDisplacement::palette_inputs(options, bundle, plater.get_extruders_colors()), bundle.project_config);
}

bool texture_panel_open_on(Plater& plater, int object_id)
{
    GLCanvas3D* canvas = plater.get_view3D_canvas3D();
    return canvas != nullptr && canvas->get_gizmos_manager().get_current_type() == GLGizmosManager::TextureDisplacement &&
           canvas->get_selection().get_object_idx() == object_id;
}

int object_plate_index(Plater& plater, int object_id)
{
    return plater.get_partplate_list().find_instance(object_id, 0);
}

}}} // namespace Slic3r::GUI::OrcaMCP
