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
// flatten_object's orient -- and the tool's wait for them.
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

namespace Slic3r { class ModelObject; }
namespace Slic3r { namespace GUI { class Plater; } }

namespace Slic3r { namespace GUI { namespace OrcaMCP {

enum class UiJobKind { arrange, orient };

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

// A tool's answer when its job did not finish: cancelled or dropped (nothing moved), failed (why),
// still running past the cap after `waited_s` seconds (how to tell when it has ended), or cut short
// by the app's quit.
nlohmann::json ui_job_unfinished_json(UiJobKind kind, UiJobWait wait, double waited_s, const std::string& error);

// A tool's refusal to start a job while another one holds the UI worker.
std::string ui_job_busy_message(const std::string& tool);

// get_slicing_status's ui_job: null while the worker is idle; "arranging" or "orienting" while the job
// MCP started last is still pending; "other" for a job MCP did not start (the GUI's). Main thread.
nlohmann::json ui_job_json(bool worker_idle, const UiJobOutcome* last_started);
void                          note_started_ui_job(const std::shared_ptr<UiJobOutcome>& outcome);
std::shared_ptr<UiJobOutcome> last_started_ui_job();

// How long a tool waits for its job: params._meta["orcamcp/wait_cap_s"], which the bridge sends from
// its ORCAMCP_TIMEOUT (wait_for_slice's cap), or 105 s, the cap at the bridge's default timeout.
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

// ---- The app side (OrcaMCPUiJobApp.cpp) ----------------------------------------------------------

// Main thread. What Plater::arrange / Plater::orient do -- an undo step ("Arrange" / "Orient"), then
// the job, in `prepare_state` (Job::PREPARE_STATE_MENU: the current plate; DEFAULT: the selection) --
// with the job wrapped so its end is reported. The caller has checked the UI worker is idle.
std::shared_ptr<UiJobOutcome> start_ui_job(Plater& plater, UiJobKind kind, int prepare_state);

// An object's instance transforms before a job, by the object's id, so its placement can be found
// and told changed or not afterwards whatever the job (or the GUI meanwhile) did to the object list.
struct ObjectTransforms
{
    ObjectID                 id;
    std::vector<Transform3d> instances;
};
ObjectTransforms              transforms_of(const ModelObject& object);
// Every object with an instance on the current plate: what the plate-wide arrange and orient act on.
std::vector<ObjectTransforms> current_plate_objects(Plater& plater);

// Main thread, after the job: the object's placement as the transform tools report it -- object_id,
// name, position, rotation_degrees, scale, changed, plate_index, on_bed -- or, for one deleted since,
// its old id with deleted: true.
nlohmann::json placement_after_job(const ObjectTransforms& before);

// HTTP thread. Waits for the job (tool_wait_cap), then answers with `report`, run on the main thread;
// a job that did not finish is answered with what stopped it (ui_job_unfinished_json).
nlohmann::json answer_after_ui_job(const UiJobOutcome& outcome, const std::function<nlohmann::json()>& report);

// arrange_objects and auto_orient: the current plate's objects arranged or oriented, answered with
// every one's placement once the job has been applied, and a preview of it when asked.
nlohmann::json run_plate_ui_job(const std::string& tool, UiJobKind kind, bool include_preview, const std::string& preview_hint);

}}} // namespace Slic3r::GUI::OrcaMCP
