#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "slic3r/Utils/QueuedCall.hpp"

// A call handed to another thread's queue and waited for, with a bound. The Flashforge agent reads the
// plate's filaments on the GUI thread this way, and used to give up after 10 s while the queued task
// still held references to its locals: a GUI thread that reached it late wrote into a dead stack frame.

using namespace Slic3r;
using namespace std::chrono_literals;

namespace {

// A queue that keeps its tasks until the test runs them, on a thread of its own when asked.
struct HeldQueue
{
    std::vector<std::function<void()>> tasks;

    std::function<void(std::function<void()>)> queue()
    {
        return [this](std::function<void()> task) { tasks.push_back(std::move(task)); };
    }
    void run_all()
    {
        for (auto& task : tasks)
            task();
        tasks.clear();
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
