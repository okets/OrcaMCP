// The app side of OrcaMCPUiJob.hpp: starting the UI worker's jobs a tool waits for, and reading the
// placement they left.
#include "OrcaMCPUiJob.hpp"
#include "OrcaMCPCommon.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Jobs/ArrangeJob.hpp"
#include "slic3r/GUI/Jobs/OrientJob.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// The object whose id is `id`, and its index, or nullptr.
ModelObject* find_object(Model& model, const ObjectID& id, int& index)
{
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id() == id) {
            index = int(i);
            return model.objects[i];
        }
    return nullptr;
}

bool transforms_differ(const ObjectTransforms& before, const ObjectTransforms& after)
{
    if (before.instances.size() != after.instances.size())
        return true;
    for (size_t i = 0; i < before.instances.size(); ++i)
        if (!before.instances[i].isApprox(after.instances[i]))
            return true;
    return false;
}

} // namespace

std::shared_ptr<UiJobOutcome> start_ui_job(Plater& plater, UiJobKind kind, int prepare_state)
{
    auto outcome = std::make_shared<UiJobOutcome>(kind);
    plater.take_snapshot(kind == UiJobKind::arrange ? _u8L("Arrange") : _u8L("Orient"));
    plater.set_prepare_state(prepare_state);
    std::unique_ptr<Job> job;
    if (kind == UiJobKind::arrange)
        job = std::make_unique<ArrangeJob>();
    else
        job = std::make_unique<OrientJob>();
    queue_job(plater.get_ui_job_worker(), std::make_shared<ReportingJob>(std::move(job), outcome));
    note_started_ui_job(outcome);
    return outcome;
}

ObjectTransforms transforms_of(const ModelObject& object)
{
    ObjectTransforms out{object.id(), {}};
    for (const ModelInstance* instance : object.instances)
        out.instances.push_back(instance->get_matrix());
    return out;
}

std::vector<ObjectTransforms> current_plate_objects(Plater& plater)
{
    std::vector<ObjectTransforms> objects;
    PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (plate == nullptr)
        return objects;
    for (const ModelObject* object : objects_on_plate(*plate))
        objects.push_back(transforms_of(*object));
    return objects;
}

nlohmann::json placement_after_job(const ObjectTransforms& before)
{
    Model&       model  = wxGetApp().plater()->model();
    int          index  = -1;
    ModelObject* object = find_object(model, before.id, index);
    if (object == nullptr)
        return {{"object_id", nullptr}, {"deleted", true}};

    const Vec3d    center   = object_world_box(*object).center();
    const Vec3d    rotation = object->instances.empty() ? Vec3d::Zero() : object->instances[0]->get_rotation();
    const Vec3d    scale    = object->instances.empty() ? Vec3d::Ones() : object->instances[0]->get_scaling_factor();
    nlohmann::json result   = {
        {"object_id", index},
        {"name", object->name},
        {"position", {{"x", center.x()}, {"y", center.y()}, {"z", center.z()}}},
        {"rotation_degrees",
         {{"x", Geometry::rad2deg(rotation.x())}, {"y", Geometry::rad2deg(rotation.y())}, {"z", Geometry::rad2deg(rotation.z())}}},
        {"scale", {{"x", scale.x()}, {"y", scale.y()}, {"z", scale.z()}}}};
    // The plate it stands on now, as a transform tool re-homes and reports it.
    rehome_and_report_placement(result, index, transforms_differ(before, transforms_of(*object)));
    return result;
}

nlohmann::json answer_after_ui_job(const UiJobOutcome& outcome, const std::function<nlohmann::json()>& report)
{
    const auto      started = std::chrono::steady_clock::now();
    const UiJobWait waited  = wait_for_ui_job(outcome, tool_wait_cap(), std::chrono::milliseconds(50), [] { wxWakeUpIdle(); });
    const double    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (waited != UiJobWait::finished)
        return ui_job_unfinished_json(outcome.kind(), waited, seconds, outcome.error());
    try {
        return run_on_main_thread(report);
    } catch (const McpShuttingDown&) {
        // The job was applied; only its answer was cut short.
        return ui_job_unfinished_json(outcome.kind(), UiJobWait::quitting, seconds, {});
    }
}

nlohmann::json run_plate_ui_job(const std::string& tool, UiJobKind kind, bool include_preview, const std::string& preview_hint)
{
    std::shared_ptr<UiJobOutcome> outcome;
    std::vector<ObjectTransforms> scope;
    const nlohmann::json          refusal = run_on_main_thread([&]() -> nlohmann::json {
        Plater* plater = wxGetApp().plater();
        if (!plater->get_ui_job_worker().is_idle())
            return error_response(ui_job_busy_message(tool));
        scope   = current_plate_objects(*plater);
        outcome = start_ui_job(*plater, kind, Job::PREPARE_STATE_MENU);
        return nullptr;
    });
    if (!refusal.is_null())
        return refusal;
    return answer_after_ui_job(*outcome, [scope, include_preview, preview_hint]() -> nlohmann::json {
        nlohmann::json objects = nlohmann::json::array();
        for (const ObjectTransforms& before : scope)
            objects.push_back(placement_after_job(before));
        nlohmann::json result = {{"status", "success"},
                                 {"objects", objects},
                                 {"active_warnings", get_active_warnings_json(wxGetApp().plater())}};
        if (include_preview) {
            add_turntable_preview_if_requested(result, true);
            result["preview_hint"] = preview_hint;
        }
        return result;
    });
}

}}} // namespace Slic3r::GUI::OrcaMCP
