// The app side of OrcaMCPUiJob.hpp: starting the UI worker's jobs a tool waits for, and reading the
// placement they left.
#include "OrcaMCPUiJob.hpp"
#include "OrcaMCPCommon.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Jobs/ArrangeJob.hpp"
#include "slic3r/GUI/Jobs/FillBedJob.hpp"
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

// A job whose finalize takes no undo step. A bed fill's finalize starts the plate's arrange
// (Plater::arrange), which takes an "Arrange" step of its own: the GUI's Fill bed is two undo steps, the
// fill and then the arrange. The tool takes one, before the fill, and this one is not taken.
class FinalizeWithoutSnapshots : public Job
{
public:
    FinalizeWithoutSnapshots(std::unique_ptr<Job> job, Plater& plater) : m_job(std::move(job)), m_plater(plater) {}
    void process(Ctl& ctl) override { m_job->process(ctl); }
    void finalize(bool canceled, std::exception_ptr& eptr) override
    {
        Plater::SuppressSnapshots one_step(&m_plater);
        m_job->finalize(canceled, eptr);
    }

private:
    std::unique_ptr<Job> m_job;
    Plater&              m_plater;
};

std::unique_ptr<Job> job_of(Plater& plater, UiJobKind kind)
{
    switch (kind) {
    case UiJobKind::orient: return std::make_unique<OrientJob>();
    case UiJobKind::fill_bed: return std::make_unique<FinalizeWithoutSnapshots>(std::make_unique<FillBedJob>(/*instances=*/true), plater);
    case UiJobKind::arrange: break;
    }
    return std::make_unique<ArrangeJob>();
}

} // namespace

std::shared_ptr<UiJobOutcome> start_ui_job(Plater& plater, UiJobKind kind, int prepare_state, bool take_snapshot)
{
    auto outcome = std::make_shared<UiJobOutcome>(kind);
    // Plater::fill_bed_with_instances names its step "Arrange" too.
    if (take_snapshot)
        plater.take_snapshot(kind == UiJobKind::orient ? _u8L("Orient") : _u8L("Arrange"));
    plater.set_prepare_state(prepare_state);
    queue_job(plater.get_ui_job_worker(), std::make_shared<ReportingJob>(job_of(plater, kind), outcome));
    note_started_ui_job(outcome);
    return outcome;
}

ObjectTransforms transforms_of(const ModelObject& object)
{
    ObjectTransforms out{object.id(), model_object_index(&object), {}};
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

std::vector<ObjectTransforms> scene_objects(Plater& plater)
{
    std::vector<ObjectTransforms> objects;
    for (const ModelObject* object : plater.model().objects)
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
    // An arrange sorts the objects (PartPlateList::rebuild_plates_after_arrangement), so an object that
    // did not move can still have another index.
    if (before.object_id >= 0 && before.object_id != index)
        result["previous_object_id"] = before.object_id;
    // The plate it stands on now, as a transform tool re-homes and reports it.
    rehome_and_report_placement(result, index, transforms_differ(before, transforms_of(*object)));
    return result;
}

namespace {

// HTTP thread. The UI worker done -- the job a finalize started finalized too -- asked on the main thread,
// within what is left of `cap`.
UiJobWait wait_for_worker_idle(std::chrono::steady_clock::time_point started, std::chrono::milliseconds cap)
{
    const auto left = cap - std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    const auto idle = [] {
        try {
            return run_on_main_thread([]() -> nlohmann::json { return worker_done(wxGetApp().plater()->get_ui_job_worker()); }).get<bool>();
        } catch (const McpShuttingDown&) {
            return false; // the wait's own cancel check ends it
        }
    };
    return wait_until(idle, std::max(left, std::chrono::milliseconds(0)), std::chrono::milliseconds(50), [] { wxWakeUpIdle(); });
}

// A bed fill applied, with the arrange it started still running past the wait (or cut short by a quit).
nlohmann::json fill_bed_arrange_unfinished_json(UiJobWait wait, double waited_s)
{
    if (wait == UiJobWait::quitting)
        return ui_job_unfinished_json(UiJobKind::fill_bed, wait, waited_s, {});
    return {{"status", "fill_bed_started"},
            {"finished", false},
            {"ui_job", "other"},
            {"message", "The bed fill was applied, and the arrange of its plate that follows it is still running after " +
                            std::to_string(int(waited_s)) + " s: get_slicing_status's ui_job is null once it has finished; then "
                            "get_scene_info reads the result."}};
}

} // namespace

nlohmann::json answer_after_ui_job(const UiJobOutcome& outcome, const std::function<nlohmann::json()>& report, bool until_worker_idle)
{
    const auto      started = std::chrono::steady_clock::now();
    const UiJobWait waited  = wait_for_ui_job(outcome, tool_wait_cap(), std::chrono::milliseconds(50), [] { wxWakeUpIdle(); });
    const auto      elapsed = [started] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); };
    const double    seconds = elapsed();
    if (waited != UiJobWait::finished)
        return ui_job_unfinished_json(outcome.kind(), waited, seconds, outcome.error());
    if (until_worker_idle)
        if (const UiJobWait idle = wait_for_worker_idle(started, tool_wait_cap()); idle != UiJobWait::finished)
            return fill_bed_arrange_unfinished_json(idle, elapsed());
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
