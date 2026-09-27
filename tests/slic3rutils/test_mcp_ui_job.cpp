#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Jobs/ArrangeJob.hpp"
#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/Model.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

// The UI worker's jobs -- an arrange, an orient -- as MCP starts them and waits for them. A real
// BoostThreadWorker runs here with this test's thread standing in for the main thread: it delivers
// the worker's messages (process_events), finalize among them, as the plater's idle handler does.

using namespace std::chrono_literals;
using Slic3r::GUI::BoostThreadWorker;
using Slic3r::GUI::Job;
using Slic3r::GUI::Worker;

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
    // Plate 7 is one the arrange locked that is gone since: skipped.
    Slic3r::GUI::end_arrange_run(plates, {0, 7}, arrange_running, [&notification_closed] { notification_closed = true; });
    CHECK_FALSE(plates.get_plate(0)->is_locked());
    CHECK_FALSE(arrange_running.load());
    CHECK(notification_closed);
}
