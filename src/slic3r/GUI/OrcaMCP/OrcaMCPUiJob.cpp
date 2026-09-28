#include "OrcaMCPUiJob.hpp"

#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/Utils/ThreadCancel.hpp"
#include "libslic3r/Model.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

constexpr std::chrono::milliseconds k_default_tool_wait_cap{105000};
constexpr double                    k_max_tool_wait_cap_s = 3600.0;
thread_local std::chrono::milliseconds t_tool_wait_cap{k_default_tool_wait_cap};

// "arrange" / "orient" / "fill_bed", as the tools' statuses spell it.
const char* status_noun(UiJobKind kind)
{
    switch (kind) {
    case UiJobKind::orient: return "orient";
    case UiJobKind::fill_bed: return "fill_bed";
    case UiJobKind::arrange: break;
    }
    return "arrange";
}
// "arrange" / "orient" / "bed fill", as the messages say it.
const char* noun(UiJobKind kind) { return kind == UiJobKind::fill_bed ? "bed fill" : status_noun(kind); }
// "arranging" / "orienting" / "filling_bed", as get_slicing_status's ui_job spells it.
const char* running_name(UiJobKind kind)
{
    switch (kind) {
    case UiJobKind::orient: return "orienting";
    case UiJobKind::fill_bed: return "filling_bed";
    case UiJobKind::arrange: break;
    }
    return "arranging";
}
// The same in words: "filling the bed".
std::string running_words(UiJobKind kind) { return kind == UiJobKind::fill_bed ? "filling the bed" : running_name(kind); }

std::string what(const std::exception_ptr& eptr)
{
    try {
        std::rethrow_exception(eptr);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "an unknown error";
    }
}

std::string seconds_text(double seconds)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << seconds;
    return out.str();
}

std::weak_ptr<UiJobOutcome>& last_started()
{
    static std::weak_ptr<UiJobOutcome> outcome;
    return outcome;
}

} // namespace

std::string UiJobOutcome::error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

void UiJobOutcome::end(State state, const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state.load(std::memory_order_relaxed) != State::pending)
        return;
    m_error = error;
    m_state.store(state, std::memory_order_release);
}

ReportingJob::ReportingJob(std::unique_ptr<Job> job, std::shared_ptr<UiJobOutcome> outcome)
    : m_job(std::move(job)), m_outcome(std::move(outcome))
{}

// A job destroyed without a finalize never ran; after one, this changes nothing.
ReportingJob::~ReportingJob() { m_outcome->end(UiJobOutcome::State::dropped); }

void ReportingJob::process(Ctl& ctl) { m_job->process(ctl); }

void ReportingJob::finalize(bool canceled, std::exception_ptr& eptr)
{
    // Read before the job's own finalize, which may report the error and clear it.
    const std::string error = eptr ? what(eptr) : std::string();
    const bool        failed = eptr != nullptr;
    m_job->finalize(canceled, eptr);
    m_outcome->end(failed ? UiJobOutcome::State::failed : canceled ? UiJobOutcome::State::cancelled : UiJobOutcome::State::finished,
                   error);
}

UiJobWait wait_until(const std::function<bool()>& done,
                     std::chrono::milliseconds    cap,
                     std::chrono::milliseconds    poll,
                     const std::function<void()>& nudge)
{
    const auto deadline = std::chrono::steady_clock::now() + cap;
    for (;;) {
        if (done())
            return UiJobWait::finished;
        if (this_thread_cancelled())
            return UiJobWait::quitting;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return UiJobWait::timed_out;
        if (nudge)
            nudge();
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(poll, deadline - now));
    }
}

UiJobWait wait_for_ui_job(const UiJobOutcome&          outcome,
                          std::chrono::milliseconds    cap,
                          std::chrono::milliseconds    poll,
                          const std::function<void()>& nudge)
{
    const UiJobWait waited = wait_until([&outcome] { return outcome.state() != UiJobOutcome::State::pending; }, cap, poll, nudge);
    if (waited != UiJobWait::finished)
        return waited;
    switch (outcome.state()) {
    case UiJobOutcome::State::cancelled: return UiJobWait::cancelled;
    case UiJobOutcome::State::failed: return UiJobWait::failed;
    case UiJobOutcome::State::dropped: return UiJobWait::dropped;
    default: break;
    }
    return UiJobWait::finished;
}

nlohmann::json ui_job_unfinished_json(UiJobKind kind, UiJobWait wait, double waited_s, const std::string& error)
{
    const std::string job = noun(kind);
    const std::string status = std::string(status_noun(kind)) + "_started";
    switch (wait) {
    case UiJobWait::cancelled:
        return {{"status", "cancelled"},
                {"message", "The app cancelled the " + job + " before applying it (another job, a deleted object or a new "
                            "project cancels it), so nothing moved; its undo step restores nothing."}};
    case UiJobWait::dropped:
        return {{"status", "cancelled"},
                {"message", "The " + job + " was replaced by another job before it started, so nothing moved; its undo "
                            "step restores nothing."}};
    case UiJobWait::failed: return {{"status", "error"}, {"message", "The " + job + " failed: " + error}};
    case UiJobWait::timed_out: {
        const std::string running = running_name(kind);
        return {{"status", status},
                {"finished", false},
                {"ui_job", running},
                {"message", "Still " + running_words(kind) + " after " + seconds_text(waited_s) + " s: get_slicing_status's ui_job stays \"" +
                                running + "\" until it has finished; then get_scene_info reads the result."}};
    }
    case UiJobWait::quitting:
        return {{"status", status},
                {"finished", false},
                {"message", "OrcaMCP began quitting while the " + job + " ran, so it may not have finished."}};
    case UiJobWait::finished: break;
    }
    throw std::logic_error("ui_job_unfinished_json: the job finished");
}

std::vector<ObjectID> object_order(const Model& model)
{
    std::vector<ObjectID> order;
    for (const ModelObject* object : model.objects)
        order.push_back(object->id());
    return order;
}

int object_index_of(const Model& model, const ObjectID& id)
{
    for (std::size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id() == id)
            return int(i);
    return -1;
}

nlohmann::json object_id_changes(const std::vector<ObjectID>& before, const Model& model)
{
    nlohmann::json changes = nlohmann::json::array();
    for (std::size_t now = 0; now < model.objects.size(); ++now) {
        const auto it = std::find(before.begin(), before.end(), model.objects[now]->id());
        if (it != before.end() && std::size_t(it - before.begin()) != now)
            changes.push_back({{"object_id", int(now)}, {"previous_object_id", int(it - before.begin())}, {"name", model.objects[now]->name}});
    }
    return changes;
}

bool worker_done(Worker& worker)
{
    worker.process_events();
    return worker.is_idle();
}

std::string ui_job_busy_message(const std::string& tool)
{
    return "another job (an arrange, an orient or a bed fill) is running: poll get_slicing_status until ui_job is null, then call " + tool +
           " again";
}

nlohmann::json ui_job_json(bool worker_idle, const UiJobOutcome* last)
{
    if (worker_idle)
        return nullptr;
    if (last != nullptr && last->state() == UiJobOutcome::State::pending)
        return running_name(last->kind());
    return "other";
}

void note_started_ui_job(const std::shared_ptr<UiJobOutcome>& outcome) { last_started() = outcome; }

std::shared_ptr<UiJobOutcome> last_started_ui_job() { return last_started().lock(); }

std::chrono::milliseconds tool_wait_cap_from(const nlohmann::json& params)
{
    const auto meta = params.find("_meta");
    if (meta == params.end() || !meta->is_object())
        return k_default_tool_wait_cap;
    const auto cap = meta->find("orcamcp/wait_cap_s");
    if (cap == meta->end() || !cap->is_number())
        return k_default_tool_wait_cap;
    const double seconds = std::clamp(cap->get<double>(), 0.0, k_max_tool_wait_cap_s);
    return std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
}

ScopedToolWaitCap::ScopedToolWaitCap(const nlohmann::json& params) : m_previous(t_tool_wait_cap)
{
    t_tool_wait_cap = tool_wait_cap_from(params);
}

ScopedToolWaitCap::~ScopedToolWaitCap() { t_tool_wait_cap = m_previous; }

std::chrono::milliseconds tool_wait_cap() { return t_tool_wait_cap; }

}}} // namespace Slic3r::GUI::OrcaMCP
