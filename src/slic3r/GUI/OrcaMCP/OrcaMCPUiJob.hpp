// src/slic3r/GUI/OrcaMCP/OrcaMCPUiJob.hpp
#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

#include <functional>
#include <vector>

#include "slic3r/GUI/Jobs/Job.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"

// The UI worker's jobs a tool starts -- arrange_objects' and clone_object's arrange, auto_orient's and
// flatten_object's orient, fill_bed_with_instances' fill -- and the tool's wait for them.
//
// The job runs on the UI worker's thread and is applied by its finalize, on the main thread's next
// idle. The tools used to answer as soon as it was queued, so an answer (and its preview) showed the
// placement from before, and the next call could land before the finalize: a rotate that the orient
// then overwrote, with one undo step for both. Now a tool pushes its job wrapped in a ReportingJob,
// which records how it ended, and waits for that on the HTTP thread -- never inside main-thread work
// -- before it reads the result. The calls are served one at a time, so while one waits no other MCP
// call can reach the scene the job is about to change.
//
// The wait polls the outcome and the request's cancel check (this_thread_cancelled(), the gate
// OrcaMCPServer::shut_down closes), so a quit releases it within one poll. It is bounded by the cap
// the bridge sends in params._meta (tool_wait_cap_from): past it, the tool says the job is still
// running and get_slicing_status's ui_job says when it has ended.
//
// Unit-tested in tests/slic3rutils/test_mcp_ui_job.cpp.

namespace Slic3r { class Model; class ModelObject; }
namespace Slic3r { namespace GUI { class Plater; class Worker; } }

namespace Slic3r { namespace GUI { namespace OrcaMCP {

enum class UiJobKind { arrange, orient, fill_bed };

// How one job MCP started ended. Written on the main thread (the job's finalize, or its destruction),
// read on the HTTP thread while it waits.
class UiJobOutcome
{
public:
    enum class State { pending, finished, cancelled, failed, dropped };

    explicit UiJobOutcome(UiJobKind kind) : m_kind(kind) {}

    UiJobKind kind() const { return m_kind; }
    State     state() const { return m_state.load(std::memory_order_acquire); }
    // Why it failed; empty unless state() is failed.
    std::string error() const;

    // Records how it ended; only the first call counts.
    void end(State state, const std::string& error = {});

private:
    const UiJobKind    m_kind;
    std::atomic<State> m_state{State::pending};
    mutable std::mutex m_mutex;
    std::string        m_error;
};

// A job as MCP pushes it to the UI worker: `job`, unchanged, whose end is recorded in `outcome` --
// finished, cancelled or failed by its finalize, dropped when it is destroyed without one (cancel_all
// cleared it from the queue before it started).
class ReportingJob : public Job
{
public:
    ReportingJob(std::unique_ptr<Job> job, std::shared_ptr<UiJobOutcome> outcome);
    ~ReportingJob() override;

    void process(Ctl& ctl) override;
    void finalize(bool canceled, std::exception_ptr& eptr) override;

private:
    std::unique_ptr<Job>          m_job;
    std::shared_ptr<UiJobOutcome> m_outcome;
};

enum class UiJobWait { finished, cancelled, failed, dropped, timed_out, quitting };

// HTTP thread. Waits until `outcome` has ended, `cap` has passed, or the app quits, polling every
// `poll` and calling `nudge` each turn. Never runs anything on the main thread; the app's nudge wakes
// its idle handler (wxWakeUpIdle), which delivers the finalize: the worker queues it after the last
// wake-up the job's own process gave, so in an app nobody touches nothing else would deliver it.
UiJobWait wait_for_ui_job(const UiJobOutcome&          outcome,
                          std::chrono::milliseconds    cap,
                          std::chrono::milliseconds    poll  = std::chrono::milliseconds(50),
                          const std::function<void()>& nudge = {});

// HTTP thread. The same wait for anything `done` says has happened -- finished, timed_out or quitting --
// asking it every `poll`: a bed fill's finalize starts the plate's arrange, which no outcome reports, and
// its end is the UI worker's going idle.
UiJobWait wait_until(const std::function<bool()>& done,
                     std::chrono::milliseconds    cap,
                     std::chrono::milliseconds    poll  = std::chrono::milliseconds(50),
                     const std::function<void()>& nudge = {});

// Main thread. Delivers what `worker` has sent -- a job's finalize among it, which may start another job,
// as a bed fill's starts the plate's arrange -- and says whether it is done: nothing queued, running or
// undelivered. A wait for a chained job asks this rather than the plater's idle handler to have run.
bool worker_done(Worker& worker);

// A tool's answer when its job did not finish: cancelled or dropped (nothing moved), failed (why),
// still running past the cap after `waited_s` seconds (how to tell when it has ended), or cut short
// by the app's quit.
nlohmann::json ui_job_unfinished_json(UiJobKind kind, UiJobWait wait, double waited_s, const std::string& error);

// A tool's refusal to start a job while another one holds the UI worker.
std::string ui_job_busy_message(const std::string& tool);

// get_slicing_status's ui_job: null while the worker is idle; "arranging", "orienting" or "filling_bed"
// while the job MCP started last is still pending; "other" for a job MCP did not start (the GUI's, or
// the arrange a bed fill starts when it is applied). Main thread.
nlohmann::json ui_job_json(bool worker_idle, const UiJobOutcome* last_started);
void                          note_started_ui_job(const std::shared_ptr<UiJobOutcome>& outcome);
std::shared_ptr<UiJobOutcome> last_started_ui_job();

// How long a tool waits for its job: params._meta["orcamcp/wait_cap_s"], which the bridge sends from
// its ORCAMCP_TIMEOUT (wait_for_slice's cap), within 0 and an hour; 105 s, the cap at the bridge's
// default timeout, when it sends none (or not a number).
std::chrono::milliseconds tool_wait_cap_from(const nlohmann::json& params);

// The cap of the tools/call being served on this thread, from its params, for the scope of the call.
class ScopedToolWaitCap
{
public:
    explicit ScopedToolWaitCap(const nlohmann::json& params);
    ~ScopedToolWaitCap();
    ScopedToolWaitCap(const ScopedToolWaitCap&)            = delete;
    ScopedToolWaitCap& operator=(const ScopedToolWaitCap&) = delete;

private:
    std::chrono::milliseconds m_previous;
};
std::chrono::milliseconds tool_wait_cap();

// The objects' ids in the model's order, before a job that can re-sort them: an arrange sorts every
// object of the model by its arrange order (PartPlateList::rebuild_plates_after_arrangement), those on
// plates it did not arrange too.
std::vector<ObjectID> object_order(const Model& model);

// The index of the object whose id is `id` in `model`, or -1.
int object_index_of(const Model& model, const ObjectID& id);

// Every object whose index in `model` differs from its index in `before`, in its new order:
// [{object_id, previous_object_id, name}]; empty when none moved. An object deleted since is left out.
nlohmann::json object_id_changes(const std::vector<ObjectID>& before, const Model& model);

// ---- The app side (OrcaMCPUiJobApp.cpp) ----------------------------------------------------------

// Main thread. What Plater::arrange, Plater::orient and Plater::fill_bed_with_instances do -- an undo
// step ("Arrange" / "Orient"; a caller that took its own passes `take_snapshot` false), then the job, in
// `prepare_state` (Job::PREPARE_STATE_MENU: the current plate; DEFAULT: an orient's selection, an
// arrange's every plate; a fill reads neither: it fills the selected instance's plate) -- with the job
// wrapped so its end is reported. A fill's finalize starts the plate's arrange (Plater::arrange), under
// no undo step of its own: the call's one step undoes both. The caller has checked the UI worker is idle.
std::shared_ptr<UiJobOutcome> start_ui_job(Plater& plater, UiJobKind kind, int prepare_state, bool take_snapshot = true);

// An object's instance transforms before a job, by the object's id, so its placement can be found
// and told changed or not afterwards whatever the job (or the GUI meanwhile) did to the object list,
// and its index then: an arrange sorts the objects (PartPlateList::rebuild_plates_after_arrangement).
struct ObjectTransforms
{
    ObjectID                 id;
    int                      object_id = -1;
    std::vector<Transform3d> instances;
};
ObjectTransforms              transforms_of(const ModelObject& object);
// Every object with an instance on the current plate: what the plate-wide arrange and orient act on.
std::vector<ObjectTransforms> current_plate_objects(Plater& plater);
// Every object in the scene: what the arrange of every plate acts on.
std::vector<ObjectTransforms> scene_objects(Plater& plater);

// Main thread, after the job: the object's placement as the transform tools report it -- object_id,
// name, position, rotation_degrees, scale, changed, plate_index, on_bed, and previous_object_id when the
// job moved it in the object list -- or, for one deleted since, its old id with deleted: true.
nlohmann::json placement_after_job(const ObjectTransforms& before);

// HTTP thread. Waits for the job (tool_wait_cap), then answers with `report`, run on the main thread;
// a job that did not finish is answered with what stopped it (ui_job_unfinished_json). With
// `until_worker_idle`, a finished job is followed by the UI worker's going idle, within the same cap:
// a bed fill's finalize starts the plate's arrange.
nlohmann::json answer_after_ui_job(const UiJobOutcome& outcome, const std::function<nlohmann::json()>& report,
                                   bool until_worker_idle = false);

// arrange_objects and auto_orient: the current plate's objects arranged or oriented, answered with
// every one's placement once the job has been applied, and a preview of it when asked.
nlohmann::json run_plate_ui_job(const std::string& tool, UiJobKind kind, bool include_preview, const std::string& preview_hint);

}}} // namespace Slic3r::GUI::OrcaMCP
