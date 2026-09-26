#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "slic3r/Utils/QueuedCall.hpp"

// A call handed to another thread's queue and waited for, with a bound. The Flashforge agent reads the
// plate's filaments on the GUI thread this way, and used to give up after 10 s while the queued task
// still held references to its locals: a GUI thread that reached it late wrote into a dead stack frame.

using namespace Slic3r;
using namespace std::chrono_literals;

namespace {

// A queue that keeps its tasks until the test runs them. Tasks may be queued from another thread.
struct HeldQueue
{
    std::mutex                         mutex;
    std::vector<std::function<void()>> tasks;

    std::function<void(std::function<void()>)> queue()
    {
        return [this](std::function<void()> task) {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back(std::move(task));
        };
    }
    size_t size()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return tasks.size();
    }
    void run_all()
    {
        std::vector<std::function<void()>> due;
        {
            std::lock_guard<std::mutex> lock(mutex);
            due.swap(tasks);
        }
        for (auto& task : due)
            task();
    }
};

} // namespace

TEST_CASE("a queued call that runs in time returns what it did", "[QueuedCall]")
{
    int  value = 0;
    auto queue = [](std::function<void()> task) { std::thread(std::move(task)).detach(); };

    CHECK(run_queued_and_wait(queue, [&] { value = 42; }, 5s));
    CHECK(value == 42);
}

TEST_CASE("a queued call reached after the caller gave up never runs", "[QueuedCall]")
{
    HeldQueue queue;
    // What the late task would write: owned here, so a wrong write is seen, not a crash.
    auto written = std::make_shared<std::atomic<int>>(0);

    const bool ran = run_queued_and_wait(queue.queue(), [written] { written->store(1); }, 20ms);
    queue.run_all(); // the GUI thread gets to it at last

    CHECK_FALSE(ran);
    CHECK(written->load() == 0);
}

TEST_CASE("a queued call that has started is waited for past the bound", "[QueuedCall]")
{
    // Its work may use what the caller owns, so the caller cannot return under it.
    std::promise<void>       started;
    std::shared_future<void> started_f = started.get_future().share();
    std::atomic<bool>        finished{false};

    auto queue = [&](std::function<void()> task) { std::thread(std::move(task)).detach(); };
    const bool ran = run_queued_and_wait(
        queue,
        [&] {
            started.set_value();
            std::this_thread::sleep_for(100ms);
            finished = true;
        },
        10ms);

    CHECK(ran);
    CHECK(finished);
}

TEST_CASE("what a queued call throws is rethrown to its caller, which is not left waiting", "[QueuedCall]")
{
    // The Flashforge print job waited for ever when the work threw: nothing marked it done.
    auto queue = [](std::function<void()> task) { std::thread(std::move(task)).detach(); };
    auto outcome = std::async(std::launch::async, [&]() -> std::string {
        try {
            run_queued_and_wait(queue, [] { throw std::runtime_error("no plate"); }, 5s);
            return "returned";
        } catch (const std::runtime_error& e) {
            return std::string("threw ") + e.what();
        }
    });

    REQUIRE(outcome.wait_for(5s) == std::future_status::ready);
    CHECK(outcome.get() == "threw no plate");
}

TEST_CASE("closing releases a caller whose work has not started, and that work never runs", "[QueuedCall]")
{
    HeldQueue   queue;
    QueuedCalls calls(queue.queue());
    auto        written = std::make_shared<std::atomic<int>>(0);

    auto ran = std::async(std::launch::async, [&] { return calls.run([written] { written->store(1); }); });
    for (int i = 0; i < 5000 && queue.size() == 0; ++i) // wait until it is queued
        std::this_thread::sleep_for(1ms);
    REQUIRE(queue.size() == 1);
    CHECK(calls.close());
    REQUIRE(ran.wait_for(5s) == std::future_status::ready);
    queue.run_all(); // the queue reaches it at last

    CHECK_FALSE(ran.get());
    CHECK(written->load() == 0);
    CHECK_FALSE(calls.close()); // closed already
    CHECK_FALSE(calls.run([written] { written->store(2); })); // refused at once
    CHECK(queue.size() == 0);
}
