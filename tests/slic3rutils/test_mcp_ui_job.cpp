#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Jobs/ArrangeJob.hpp"
#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPUiJob.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/Utils/ThreadCancel.hpp"
#include "libslic3r/Model.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// The UI worker's jobs -- an arrange, an orient -- as MCP starts them and waits for them. A real
// BoostThreadWorker runs here with this test's thread standing in for the main thread: it delivers
// the worker's messages (process_events), finalize among them, as the plater's idle handler does.

using namespace std::chrono_literals;
using Slic3r::GUI::BoostThreadWorker;
using Slic3r::GUI::Job;
using Slic3r::GUI::Worker;
using namespace Slic3r::GUI::OrcaMCP;
using State = UiJobOutcome::State;

namespace {

// A job that says when its process has returned, can be held in process until released (or
// cancelled), and records how its finalize was told it ended.
struct RecordingJob : Job
{
    std::promise<void> processed;
    std::atomic<bool>  hold{false};
    bool               finalized          = false;
    bool               finalized_canceled = false;

    void process(Ctl& ctl) override
    {
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
    REQUIRE(pump_until_idle(worker, 5s));
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
    REQUIRE(processed.wait_for(5s) == std::future_status::ready);
    std::this_thread::sleep_for(200ms); // its finalize is queued by now, with process's verdict
    worker.cancel_all();
    REQUIRE(pump_until_idle(worker, 5s));
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
    REQUIRE(pump_until_idle(worker, 5s));
    CHECK(job->finalized);
    CHECK_FALSE(job->finalized_canceled);
}

TEST_CASE("cancel_all during a job's process finalizes it as cancelled", "[McpUiJob][orcamcp]")
{
    BoostThreadWorker worker{nullptr, "test"};
    auto              job = std::make_shared<RecordingJob>();
    job->hold.store(true);
    worker.push(job);
    std::this_thread::sleep_for(50ms);
    worker.cancel_all();
    REQUIRE(pump_until_idle(worker, 5s));
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
        REQUIRE(pump_until_idle(worker, 5s));
        CHECK(outcome->state() == State::finished);
    }
    SECTION("failing in process")
    {
        auto outcome  = std::make_shared<UiJobOutcome>(UiJobKind::orient);
        auto job      = std::make_unique<FakeJob>();
        job->throws   = true;
        worker.push(std::make_shared<ReportingJob>(std::move(job), outcome));
        // The worker rethrows a finalize's unhandled exception on the thread that delivers it.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
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
        blocker->hold.store(true);
        worker.push(blocker);
        auto outcome = std::make_shared<UiJobOutcome>(UiJobKind::arrange);
        worker.push(reporting(outcome));
        std::this_thread::sleep_for(50ms);
        worker.cancel_all();
        REQUIRE(pump_until_idle(worker, 5s));
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
    const auto started = std::chrono::steady_clock::now();
    const UiJobWait waited  = wait_for_ui_job(outcome, 5s);
    const auto      took    = std::chrono::steady_clock::now() - started;
    finalize.join();
    CHECK(waited == UiJobWait::finished);
    CHECK(took < 2s);
}

TEST_CASE("The wait reports how the job ended", "[McpUiJob][orcamcp]")
{
    const auto [state, expected] = GENERATE(table<State, UiJobWait>({{State::finished, UiJobWait::finished},
                                                                     {State::cancelled, UiJobWait::cancelled},
                                                                     {State::failed, UiJobWait::failed},
                                                                     {State::dropped, UiJobWait::dropped}}));
    UiJobOutcome outcome(UiJobKind::arrange);
    outcome.end(state);
    CHECK(wait_for_ui_job(outcome, 1s) == expected);
}

TEST_CASE("The wait gives up at its cap", "[McpUiJob][orcamcp]")
{
    UiJobOutcome outcome(UiJobKind::arrange);
    const auto   started = std::chrono::steady_clock::now();
    CHECK(wait_for_ui_job(outcome, 200ms) == UiJobWait::timed_out);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
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
    const auto      started = std::chrono::steady_clock::now();
    const UiJobWait waited  = wait_for_ui_job(outcome, 60s);
    const auto      took    = std::chrono::steady_clock::now() - started;
    quit.join();
    CHECK(waited == UiJobWait::quitting);
    CHECK(took < 2s);
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
}

TEST_CASE("A tool asked to start a job while another runs says how to tell when it has ended", "[McpUiJob][orcamcp]")
{
    CHECK(ui_job_busy_message("auto_orient") ==
          "another job (an arrange or an orient) is running: poll get_slicing_status until ui_job is null, then call "
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
}

TEST_CASE("The wait's cap is what the bridge sends, 105 s without it", "[McpUiJob][orcamcp]")
{
    using std::chrono::milliseconds;
    CHECK(tool_wait_cap_from(nlohmann::json::object()) == milliseconds(105000));
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", 30}}}}) == milliseconds(30000));
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", 1.5}}}}) == milliseconds(1500));
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", 0}}}}) == milliseconds(0));
    CHECK(tool_wait_cap_from({{"_meta", {{"orcamcp/wait_cap_s", -3}}}}) == milliseconds(105000));
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
