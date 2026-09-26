#include "QueuedCall.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>

namespace Slic3r {

namespace {

// Shared by the caller and the queued task, which can outlive the caller.
struct CallState
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    started   = false;
    bool                    done      = false;
    bool                    abandoned = false; // the caller stopped waiting before the task started
};

} // namespace

bool run_queued_and_wait(const std::function<void(std::function<void()>)>& queue,
                         std::function<void()>                             fn,
                         std::chrono::milliseconds                         bound)
{
    auto state = std::make_shared<CallState>();
    queue([state, fn = std::move(fn)] {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->abandoned)
                return; // its caller has returned: what `fn` uses may be gone
            state->started = true;
        }
        state->changed.notify_all();
        fn();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
        }
        state->changed.notify_all();
    });

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->changed.wait_for(lock, bound, [&] { return state->started; })) {
        state->abandoned = true;
        return false;
    }
    state->changed.wait(lock, [&] { return state->done; });
    return true;
}

} // namespace Slic3r
