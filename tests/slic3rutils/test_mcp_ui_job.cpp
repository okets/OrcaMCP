#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Jobs/ArrangeJob.hpp"
#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"
#include "slic3r/GUI/Jobs/ProgressIndicator.hpp"
#include "slic3r/GUI/Jobs/WorkerDrain.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPUiJob.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/Utils/ThreadCancel.hpp"
#include "libslic3r/Model.hpp"
#include "mcp_thread_test_utils.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

// The UI worker's jobs -- an arrange, an orient -- as MCP starts them and waits for them. A real
// BoostThreadWorker runs here with this test's thread standing in for the main thread: it delivers
// the worker's messages (process_events), finalize among them, as the plater's idle handler does.

using namespace std::chrono_literals;
using mcp_test::k_bound;
using Slic3r::GUI::BoostThreadWorker;
using Slic3r::GUI::Job;
using Slic3r::GUI::Worker;
using namespace Slic3r::GUI::OrcaMCP;
using State = UiJobOutcome::State;

namespace {

// A job that says when its process has started and when it has returned, can be held in process
// until released (or cancelled), and records how its finalize was told it ended.
struct RecordingJob : Job
{
    std::promise<void> started;
    std::promise<void> processed;
    std::atomic<bool>  hold{false};
    bool               finalized          = false;
    bool               finalized_canceled = false;

    void process(Ctl& ctl) override
    {
        started.set_value();
        while (hold.load() && !ctl.was_canceled())
            std::this_thread::sleep_for(5ms);
        processed.set_value();
    }
    void finalize(bool canceled, std::exception_ptr&) override
    {
        finalized          = true;
        finalized_canceled = canceled;
    }
};

// Delivers the worker's messages on this thread until it is idle; false when `timeout` passes first.
bool pump_until_idle(Worker& worker, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!worker.is_idle()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        worker.process_events();
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

} // namespace

TEST_CASE("A job left to finish is finalized as not cancelled", "[McpUiJob][orcamcp]")
{
    BoostThreadWorker worker{nullptr, "test"};
    auto              job = std::make_shared<RecordingJob>();
    worker.push(job);
    REQUIRE(pump_until_idle(worker, k_bound));
    CHECK(job->finalized);
    CHECK_FALSE(job->finalized_canceled);
}

TEST_CASE("cancel_all after a job's process returned, before its finalize, finalizes it as cancelled",
          "[McpUiJob][orcamcp]")
{
    // The delete paths (Plater::priv::remove, delete_object_from_model, reset) call cancel_all and then
    // free the model's objects. A finalize still told "not cancelled" then wrote to freed instances.
    BoostThreadWorker worker{nullptr, "test"};
    auto              job       = std::make_shared<RecordingJob>();
    auto              processed = job->processed.get_future();
    worker.push(job);
    REQUIRE(processed.wait_for(k_bound) == std::future_status::ready);
    // However the cancel lands, the finalize must say cancelled: before process's verdict is read, the
    // cancel sets it; after, the count does. The wait only makes the second order, the one the count
    // is for, all but certain; it cannot make the test fail.
    std::this_thread::sleep_for(200ms);
    worker.cancel_all();
    REQUIRE(pump_until_idle(worker, k_bound));
    CHECK(job->finalized);
    CHECK(job->finalized_canceled);
}

TEST_CASE("A job pushed after cancel_all is not cancelled by it", "[McpUiJob][orcamcp]")
{
    // replace_job is cancel_all, then a push: the new job must run as asked.
    BoostThreadWorker worker{nullptr, "test"};
    worker.cancel_all();
    auto job = std::make_shared<RecordingJob>();
    worker.push(job);
    REQUIRE(pump_until_idle(worker, k_bound));
    CHECK(job->finalized);
    CHECK_FALSE(job->finalized_canceled);
}

TEST_CASE("cancel_all during a job's process finalizes it as cancelled", "[McpUiJob][orcamcp]")
{
    BoostThreadWorker worker{nullptr, "test"};
    auto              job     = std::make_shared<RecordingJob>();
    auto              started = job->started.get_future();
    job->hold.store(true);
    worker.push(job);
    REQUIRE(started.wait_for(k_bound) == std::future_status::ready); // it is in process, not in the queue
    worker.cancel_all();
    REQUIRE(pump_until_idle(worker, k_bound));
    CHECK(job->finalized);
    CHECK(job->finalized_canceled);
}

TEST_CASE("An arrange's run is undone however it ends: the plates it locked, its flag, its notification",
          "[McpUiJob][orcamcp]")
{
    // A cancelled or failed arrange returned from its finalize before undoing them: prepare_all's plates
    // stayed locked (every later arrange, orient and flatten_object took them for the user's), the
    // plate toolbar's arrange button did nothing again, and "Arranging..." stayed up until it timed out.
    Slic3r::Model              model;
    Slic3r::GUI::PartPlateList plates(nullptr, &model, Slic3r::ptFFF);
    plates.get_plate(0)->lock(true);
    std::atomic<bool> arrange_running{true};
    bool              notification_closed = false;
    Slic3r::GUI::end_arrange_run(plates, {plates.get_plate(0)->id()}, arrange_running,
                                 [&notification_closed] { notification_closed = true; });
    CHECK_FALSE(plates.get_plate(0)->is_locked());
    CHECK_FALSE(arrange_running.load());
    CHECK(notification_closed);
}

// ==================== WHEN THE PLATER GOES ====================
// ~Plater cancels the UI worker's jobs and delivers their last messages while it is whole
// (drain_worker), then has the worker deliver nothing more (stop_delivering): whatever a job that
// ignored the cancel sends later would reach a plater half destroyed.

namespace {

// A job whose finalize leaves its error set, as one that met something other than a std::exception
// does (PlaterJob clears only those): the worker rethrows it on the thread that delivers it.
struct LeavesErrorJob : Job
{
    bool std_exception;
    explicit LeavesErrorJob(bool std_exception) : std_exception(std_exception) {}

    void process(Ctl&) override {}
    void finalize(bool, std::exception_ptr& eptr) override
    {
        eptr = std_exception ? std::make_exception_ptr(std::runtime_error("left set")) : std::make_exception_ptr(7);
    }
};

// A job that ignores its cancel, reporting as it runs, until it is released.
struct StubbornJob : Job
{
    std::promise<void> started;
    std::atomic<bool>  released{false};

    void process(Ctl& ctl) override
    {
        started.set_value();
        while (!released.load()) {
            ctl.update_status(50, "still going");
            std::this_thread::sleep_for(1ms);
        }
    }
};

// A job that sends one of each message a job sends: a status, a main-thread call it waits on, its
// finalize.
struct TalkativeJob : Job
{
    std::promise<void> processed;
    bool               main_thread_call_ran = false;
    bool               finalized            = false;

    void process(Ctl& ctl) override
    {
        ctl.update_status(50, "half way");
        ctl.call_on_main_thread([this] { main_thread_call_ran = true; }).wait();
        processed.set_value();
    }
    void finalize(bool, std::exception_ptr&) override { finalized = true; }
};

struct CountingProgress : Slic3r::ProgressIndicator
{
    int updates = 0;

    void clear_percent() override {}
    void show_error_info(wxString, int, wxString, wxString) override {}
    void set_range(int) override {}
    void set_cancel_callback(CancelFn) override {}
    void set_progress(int) override { ++updates; }
    void set_status_text(const char*) override { ++updates; }
    int  get_range() const override { return 100; }
};

} // namespace

TEST_CASE("A drain delivers every job's last message, and what one throws is logged, never let out", "[McpUiJob][orcamcp]")
{
    STATIC_REQUIRE(noexcept(Slic3r::GUI::drain_worker(std::declval<Worker&>(), std::declval<std::chrono::milliseconds>(),
                                                      std::declval<const char*>())));
    const bool        std_exception = GENERATE(true, false);
    BoostThreadWorker worker{nullptr, "test"};
    auto              after     = std::make_shared<RecordingJob>();
    auto              processed = after->processed.get_future();
    worker.push(std::make_shared<LeavesErrorJob>(std_exception));
    worker.push(after);
    REQUIRE(processed.wait_for(k_bound) == std::future_status::ready); // both have run their process
    const Slic3r::GUI::WorkerDrain drained = Slic3r::GUI::drain_worker(worker, k_bound, "test");
    CHECK(drained.idle);
    REQUIRE(drained.escaped.size() == 1);
    CHECK(drained.escaped.front() == (std_exception ? "left set" : "an exception that is not a std::exception"));
    CHECK(after->finalized); // the drain went on past the escape
    CHECK(after->finalized_canceled);
}

TEST_CASE("A drain gives up at its deadline, however often a job that ignores its cancel reports", "[McpUiJob][orcamcp]")
{
    // A wait that ends only after a stretch with no message never ends while such a job runs.
    BoostThreadWorker worker{nullptr, "test"};
    auto              job     = std::make_shared<StubbornJob>();
    auto              started = job->started.get_future();
    worker.push(job);
    REQUIRE(started.wait_for(k_bound) == std::future_status::ready);
    // A drain that waited on would end only once this lets the job go, and then idle: the check below
    // fails, the test does not hang.
    std::promise<void> drain_returned;
    std::thread releaser([&job, returned = drain_returned.get_future()] {
        returned.wait_for(k_bound);
        job->released.store(true);
    });
    const Slic3r::GUI::WorkerDrain drained = Slic3r::GUI::drain_worker(worker, 100ms, "test");
    drain_returned.set_value();
    releaser.join();
    CHECK_FALSE(drained.idle);
    CHECK(drained.escaped.empty());
    REQUIRE(pump_until_idle(worker, k_bound));
}

TEST_CASE("A worker that stopped delivering drops what its jobs send: no status, no main-thread call, no finalize",
          "[McpUiJob][orcamcp]")
{
    auto              progress = std::make_shared<CountingProgress>();
    BoostThreadWorker worker{progress, "test"};
    worker.stop_delivering();
    auto job       = std::make_shared<TalkativeJob>();
    auto processed = job->processed.get_future();
    worker.push(job);
    REQUIRE(pump_until_idle(worker, k_bound));
    CHECK(processed.wait_for(0s) == std::future_status::ready); // the call it waited on let it go
    CHECK(progress->updates == 0);
    CHECK_FALSE(job->main_thread_call_ran);
    CHECK_FALSE(job->finalized);
}

// ==================== THE JOB MCP STARTS, AND ITS OUTCOME ====================

namespace {

// Finalize is told what the worker decided; this records it, and can throw from process.
struct FakeJob : Job
{
    bool* finalized_canceled = nullptr;
    bool  throws             = false;
    void  process(Ctl&) override
    {
        if (throws)
            throw std::runtime_error("no room on the plate");
    }
    void finalize(bool canceled, std::exception_ptr&) override
    {
        if (finalized_canceled != nullptr)
            *finalized_canceled = canceled;
    }
};

std::shared_ptr<ReportingJob> reporting(std::shared_ptr<UiJobOutcome> outcome, bool* finalized_canceled = nullptr)
{
    auto job                = std::make_unique<FakeJob>();
    job->finalized_canceled = finalized_canceled;
    return std::make_shared<ReportingJob>(std::move(job), std::move(outcome));
}

} // namespace

TEST_CASE("A reported job's finalize says whether it finished or was cancelled, and passes the verdict on",
          "[McpUiJob][orcamcp]")
{
    const bool canceled = GENERATE(false, true);
    auto       outcome  = std::make_shared<UiJobOutcome>(UiJobKind::arrange);
    bool       told     = !canceled;
    {
        auto               job = reporting(outcome, &told);
        std::exception_ptr none;
        job->finalize(canceled, none);
    }
    CHECK(told == canceled);
    CHECK(outcome->state() == (canceled ? State::cancelled : State::finished));
}

TEST_CASE("A reported job that failed says why", "[McpUiJob][orcamcp]")
{
    auto outcome = std::make_shared<UiJobOutcome>(UiJobKind::orient);
    {
        auto               job  = reporting(outcome);
        std::exception_ptr eptr = std::make_exception_ptr(std::runtime_error("no room on the plate"));
        job->finalize(false, eptr);
    }
    CHECK(outcome->state() == State::failed);
    CHECK(outcome->error() == "no room on the plate");
}

TEST_CASE("A reported job dropped before it ran is reported dropped", "[McpUiJob][orcamcp]")
{
    auto outcome = std::make_shared<UiJobOutcome>(UiJobKind::arrange);
    reporting(outcome).reset(); // what cancel_all's clearing of the queue does to it
    CHECK(outcome->state() == State::dropped);
}

TEST_CASE("On the worker, a reported job ends finished, cancelled or dropped as the worker ran it", "[McpUiJob][orcamcp]")
{
    BoostThreadWorker worker{nullptr, "test"};

    SECTION("left to run")
    {
        auto outcome = std::make_shared<UiJobOutcome>(UiJobKind::arrange);
        worker.push(reporting(outcome));
        REQUIRE(pump_until_idle(worker, k_bound));
        CHECK(outcome->state() == State::finished);
    }
    SECTION("failing in process")
    {
        auto outcome  = std::make_shared<UiJobOutcome>(UiJobKind::orient);
        auto job      = std::make_unique<FakeJob>();
        job->throws   = true;
        worker.push(std::make_shared<ReportingJob>(std::move(job), outcome));
        // The worker rethrows a finalize's unhandled exception on the thread that delivers it.
        const auto deadline = std::chrono::steady_clock::now() + k_bound;
        while (!worker.is_idle() && std::chrono::steady_clock::now() < deadline) {
            try {
                worker.process_events();
            } catch (const std::exception&) {}
            std::this_thread::sleep_for(2ms);
        }
        CHECK(outcome->state() == State::failed);
        CHECK(outcome->error() == "no room on the plate");
    }
    SECTION("cancelled while it runs, or cleared from the queue before it starts")
    {
        auto blocker = std::make_shared<RecordingJob>();
        auto started = blocker->started.get_future();
        blocker->hold.store(true);
        worker.push(blocker);
        auto outcome = std::make_shared<UiJobOutcome>(UiJobKind::arrange);
        worker.push(reporting(outcome));
        REQUIRE(started.wait_for(k_bound) == std::future_status::ready); // the blocker runs; ours waits in the queue
        worker.cancel_all();
        REQUIRE(pump_until_idle(worker, k_bound));
        CHECK(blocker->finalized_canceled);
        CHECK(outcome->state() == State::dropped);
    }
}

// ==================== WAITING FOR IT ====================

TEST_CASE("The wait ends as soon as the job has ended", "[McpUiJob][orcamcp]")
{
    UiJobOutcome outcome(UiJobKind::orient);
    std::thread  finalize([&outcome] {
        std::this_thread::sleep_for(100ms);
        outcome.end(State::finished);
    });
    // Finished, not timed out: it ended on the outcome, long before its cap.
    const UiJobWait waited = wait_for_ui_job(outcome, k_bound);
    finalize.join();
    CHECK(waited == UiJobWait::finished);
}

TEST_CASE("The wait nudges the main thread while it waits, so a finalize nothing else wakes is delivered",
          "[McpUiJob][orcamcp]")
{
    // The worker queues a finalize after the last wake-up of the job's own process: in an app no one
    // touches, the main thread's idle handler, which delivers it, then ran only when something else
    // happened. The wait's nudge is that something (wxWakeUpIdle in the app).
    UiJobOutcome outcome(UiJobKind::orient);
    int          nudges = 0;
    const UiJobWait waited = wait_for_ui_job(outcome, k_bound, 1ms, [&outcome, &nudges] {
        if (++nudges == 3)
            outcome.end(State::finished); // as the finalize this nudge let run would
    });
    CHECK(waited == UiJobWait::finished);
    CHECK(nudges == 3);
}

TEST_CASE("The wait reports how the job ended", "[McpUiJob][orcamcp]")
{
    const auto [state, expected] = GENERATE(table<State, UiJobWait>({{State::finished, UiJobWait::finished},
                                                                     {State::cancelled, UiJobWait::cancelled},
                                                                     {State::failed, UiJobWait::failed},
                                                                     {State::dropped, UiJobWait::dropped}}));
    UiJobOutcome outcome(UiJobKind::arrange);
    outcome.end(state);
    CHECK(wait_for_ui_job(outcome, k_bound) == expected);
}

TEST_CASE("The wait gives up at its cap", "[McpUiJob][orcamcp]")
{
    UiJobOutcome outcome(UiJobKind::arrange);
    CHECK(wait_for_ui_job(outcome, 200ms) == UiJobWait::timed_out);
    CHECK(wait_for_ui_job(outcome, 0ms) == UiJobWait::timed_out);
}

TEST_CASE("The wait lets go at once when the app quits", "[McpUiJob][orcamcp]")
{
    // The request's cancel check is the gate: OrcaMCPServer::shut_down closes it when the app quits.
    std::atomic<bool>                     quitting{false};
    const Slic3r::ScopedThreadCancelCheck check([&quitting] { return quitting.load(); });
    UiJobOutcome                          outcome(UiJobKind::arrange);
    std::thread                           quit([&quitting] {
        std::this_thread::sleep_for(100ms);
        quitting.store(true);
    });
    // Quitting, not timed out: it let go on the quit, long before its cap.
    const UiJobWait waited = wait_for_ui_job(outcome, k_bound);
    quit.join();
    CHECK(waited == UiJobWait::quitting);
}

TEST_CASE("Waiting until something is done ends when it is, at the cap, or on a quit", "[McpUiJob][orcamcp]")
{
    std::atomic<bool> done{false};
    std::thread       finish([&done] {
        std::this_thread::sleep_for(100ms);
        done.store(true);
    });
    CHECK(wait_until([&done] { return done.load(); }, k_bound) == UiJobWait::finished);
    finish.join();
    CHECK(wait_until([] { return false; }, 100ms) == UiJobWait::timed_out);
    const Slic3r::ScopedThreadCancelCheck check([] { return true; });
    CHECK(wait_until([] { return false; }, k_bound) == UiJobWait::quitting);
}

TEST_CASE("A job whose finalize starts another leaves the worker busy after it is reported finished", "[McpUiJob][orcamcp]")
{
    // A bed fill's finalize starts the plate's arrange (Plater::arrange): its outcome says finished while
    // the arrange is queued, so the tool waits for the worker to go idle too.
    BoostThreadWorker worker{nullptr, "test"};
    auto              follow_up = std::make_shared<RecordingJob>();
    struct Chaining : Job
    {
        Worker&                       worker;
        std::shared_ptr<RecordingJob> next;
        Chaining(Worker& worker, std::shared_ptr<RecordingJob> next) : worker(worker), next(std::move(next)) {}
        void process(Ctl&) override {}
        void finalize(bool, std::exception_ptr&) override { worker.push(next); }
    };
    auto outcome = std::make_shared<UiJobOutcome>(UiJobKind::fill_bed);
    worker.push(std::make_shared<ReportingJob>(std::make_unique<Chaining>(worker, follow_up), outcome));
    const auto deadline = std::chrono::steady_clock::now() + k_bound;
    while (outcome->state() == State::pending && std::chrono::steady_clock::now() < deadline) {
        worker.process_events();
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(outcome->state() == State::finished);
    CHECK_FALSE(worker.is_idle());
    REQUIRE(pump_until_idle(worker, k_bound));
    CHECK(follow_up->finalized);
}

TEST_CASE("Every object an arrange re-sorted is named, those on plates it did not arrange too", "[McpUiJob][orcamcp]")
{
    // A, B and D on the arranged plate, C elsewhere: the re-sort gives A, C, B, D, and B moved though
    // the arrange did not reach it.
    Slic3r::Model model;
    for (const char* name : {"A", "B", "C", "D"})
        model.add_object()->name = name;
    const std::vector<Slic3r::ObjectID> before = object_order(model);
    std::swap(model.objects[1], model.objects[2]);

    CHECK(object_id_changes(before, model) ==
          nlohmann::json::array({{{"object_id", 1}, {"previous_object_id", 2}, {"name", "C"}},
                                 {{"object_id", 2}, {"previous_object_id", 1}, {"name", "B"}}}));
    CHECK(object_id_changes(object_order(model), model).empty());
    CHECK(object_index_of(model, before[1]) == 2);
    CHECK(object_index_of(model, Slic3r::ObjectID()) == -1);
}

TEST_CASE("An object deleted since is left out of the renumbered objects", "[McpUiJob][orcamcp]")
{
    Slic3r::Model model;
    for (const char* name : {"A", "B", "C"})
        model.add_object()->name = name;
    const std::vector<Slic3r::ObjectID> before = object_order(model);
    model.delete_object(size_t(0));
    CHECK(object_id_changes(before, model) ==
          nlohmann::json::array({{{"object_id", 0}, {"previous_object_id", 1}, {"name", "B"}},
                                 {{"object_id", 1}, {"previous_object_id", 2}, {"name", "C"}}}));
}

// ==================== WHAT THE TOOL ANSWERS ====================

TEST_CASE("A job that did not finish is answered with what stopped it and what to do", "[McpUiJob][orcamcp]")
{
    CHECK(ui_job_unfinished_json(UiJobKind::arrange, UiJobWait::cancelled, 1.0, "") ==
          nlohmann::json{{"status", "cancelled"},
                         {"message", "The app cancelled the arrange before applying it (another job, a deleted object or a "
                                     "new project cancels it), so nothing moved; its undo step restores nothing."}});
    CHECK(ui_job_unfinished_json(UiJobKind::orient, UiJobWait::dropped, 0.0, "") ==
          nlohmann::json{{"status", "cancelled"},
                         {"message", "The orient was replaced by another job before it started, so nothing moved; its undo "
                                     "step restores nothing."}});
    CHECK(ui_job_unfinished_json(UiJobKind::arrange, UiJobWait::failed, 2.0, "no room on the plate") ==
          nlohmann::json{{"status", "error"}, {"message", "The arrange failed: no room on the plate"}});
    CHECK(ui_job_unfinished_json(UiJobKind::orient, UiJobWait::timed_out, 105.04, "") ==
          nlohmann::json{{"status", "orient_started"},
                         {"finished", false},
                         {"ui_job", "orienting"},
                         {"message", "Still orienting after 105.0 s: get_slicing_status's ui_job stays \"orienting\" until "
                                     "it has finished; then get_scene_info reads the result."}});
    CHECK(ui_job_unfinished_json(UiJobKind::arrange, UiJobWait::quitting, 3.0, "") ==
          nlohmann::json{{"status", "arrange_started"},
                         {"finished", false},
                         {"message", "OrcaMCP began quitting while the arrange ran, so it may not have finished."}});
    CHECK(ui_job_unfinished_json(UiJobKind::fill_bed, UiJobWait::timed_out, 10.0, "") ==
          nlohmann::json{{"status", "fill_bed_started"},
                         {"finished", false},
                         {"ui_job", "filling_bed"},
                         {"message", "Still filling the bed after 10.0 s: get_slicing_status's ui_job stays \"filling_bed\" until "
                                     "it has finished; then get_scene_info reads the result."}});
    CHECK(ui_job_unfinished_json(UiJobKind::fill_bed, UiJobWait::cancelled, 1.0, "").at("message").get<std::string>().find(
              "cancelled the bed fill") != std::string::npos);
}

TEST_CASE("A tool asked to start a job while another runs says how to tell when it has ended", "[McpUiJob][orcamcp]")
{
    CHECK(ui_job_busy_message("auto_orient") ==
          "another job (an arrange, an orient or a bed fill) is running: poll get_slicing_status until ui_job is null, then call "
          "auto_orient again");
}

TEST_CASE("get_slicing_status names the UI job that is running", "[McpUiJob][orcamcp]")
{
    UiJobOutcome arranging(UiJobKind::arrange);
    UiJobOutcome ended(UiJobKind::orient);
    ended.end(State::finished);
    CHECK(ui_job_json(/*worker_idle=*/true, &arranging).is_null());
    CHECK(ui_job_json(false, &arranging) == "arranging");
    CHECK(ui_job_json(false, &ended) == "other");   // the worker holds a job MCP did not start
    CHECK(ui_job_json(false, nullptr) == "other");
    UiJobOutcome filling(UiJobKind::fill_bed);
    CHECK(ui_job_json(false, &filling) == "filling_bed");
}

TEST_CASE("The wait's cap is what the bridge sends, 105 s without it", "[McpUiJob][orcamcp]")
{
    using std::chrono::milliseconds;
    CHECK(tool_wait_cap_from(nlohmann::json::object()) == milliseconds(105000));
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", 30}}}}) == milliseconds(30000));
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", 1.5}}}}) == milliseconds(1500));
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", 0}}}}) == milliseconds(0));
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", -3}}}}) == milliseconds(0));
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", 1e300}}}}) == milliseconds(3600000)); // an hour at most
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", "30"}}}}) == milliseconds(105000));
    CHECK(tool_wait_cap_from({{"_meta", {{"progressToken", 7}}}}) == milliseconds(105000));

    const ScopedToolWaitCap scoped({{"_meta", {{"orcamcp/wait_cap_s", 12}}}});
    CHECK(tool_wait_cap() == milliseconds(12000));
}

TEST_CASE("An arrange unlocks only the plates it locked, never a plate of a list that replaced them", "[McpUiJob][orcamcp]")
{
    // A project opened while the arrange ran: the reset cancels it, and its late finalize meets a new
    // plate list, whose first plate the user saved locked. Its index is the arranged plate's; its
    // identity is not.
    Slic3r::Model              model;
    Slic3r::GUI::PartPlateList arranged(nullptr, &model, Slic3r::ptFFF);
    Slic3r::GUI::PartPlateList opened(nullptr, &model, Slic3r::ptFFF);
    opened.get_plate(0)->lock(true);
    std::atomic<bool> arrange_running{true};
    Slic3r::GUI::end_arrange_run(opened, {arranged.get_plate(0)->id()}, arrange_running, [] {});
    CHECK(opened.get_plate(0)->is_locked());
    CHECK_FALSE(arrange_running.load());
}
