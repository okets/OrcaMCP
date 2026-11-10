#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"

#include <array>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

namespace Slic3r {
// Private implementation seam: intentionally absent from the public class/header API.
bool geometry_readiness_snapshot(std::mutex &, const BackgroundSlicingProcess::State &);
}

namespace {
using Process = Slic3r::BackgroundSlicingProcess;
constexpr std::array<Process::State, 8> states {{
    Process::STATE_INITIAL, Process::STATE_IDLE, Process::STATE_STARTED,
    Process::STATE_RUNNING, Process::STATE_FINISHED, Process::STATE_CANCELED,
    Process::STATE_EXIT, Process::STATE_EXITED
}};

class HeldMutex {
public:
    explicit HeldMutex(std::mutex &mutex)
        : worker([this, &mutex] {
            std::unique_lock<std::mutex> lock(mutex);
            owned.set_value();
            release.get_future().wait();
        })
    { owned.get_future().wait(); }
    ~HeldMutex() { unlock_and_join(); }
    void unlock_and_join()
    {
        if (worker.joinable()) {
            release.set_value();
            worker.join();
        }
    }
private:
    std::promise<void> owned;
    std::promise<void> release;
    std::thread worker;
};
}

TEST_CASE("Geometry readiness admits only quiescent states and releases its lock", "[GeometryReadiness]")
{
    std::mutex mutex;
    for (unsigned cycle = 0; cycle < 16; ++cycle) {
        for (auto state : states) {
            CAPTURE(cycle, state);
            const bool expected = state == Process::STATE_INITIAL || state == Process::STATE_IDLE;
            CHECK(Slic3r::geometry_readiness_snapshot(mutex, state) == expected);
            // A different thread proves the production helper retained no lock on either path.
            auto observer = std::async(std::launch::async, [&mutex] {
                std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
                return lock.owns_lock();
            });
            CHECK(observer.get());
        }
    }
}

TEST_CASE("Geometry readiness refuses another thread's held mutex before release", "[GeometryReadiness]")
{
    using namespace std::chrono_literals;
    std::mutex mutex;
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        for (auto state : states) {
            CAPTURE(cycle, state);
            HeldMutex owner(mutex);
            auto caller = std::async(std::launch::async, [&] {
                return Slic3r::geometry_readiness_snapshot(mutex, state);
            });
            // This is a cleanup watchdog, not a latency contract. Release before assertions
            // so even a mistakenly blocking implementation can finish and join safely.
            const auto completion = caller.wait_for(1s);
            owner.unlock_and_join();
            const bool admitted = caller.get();
            REQUIRE(completion == std::future_status::ready);
            CHECK_FALSE(admitted);
            CHECK(Slic3r::geometry_readiness_snapshot(mutex, state) ==
                  (state == Process::STATE_INITIAL || state == Process::STATE_IDLE));
        }
    }
}
