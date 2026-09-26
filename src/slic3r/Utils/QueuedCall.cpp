#include "QueuedCall.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <vector>

namespace Slic3r {

struct QueuedCalls::State
{
    mutable std::mutex      mutex;
    std::condition_variable changed;
    bool                    closed  = false;
    int                     running = 0;                  // calls whose work has started and not finished
    std::vector<std::function<void()>> after_work;        // posted once no work is running
};

// One call's progress. Guarded by State::mutex.
struct QueuedCalls::Call
{
    bool               started   = false;
    bool               done      = false;
    bool               abandoned = false; // its caller stopped waiting before it started
    std::exception_ptr error;
};

QueuedCalls::QueuedCalls(Post post) : m_state(std::make_shared<State>()), m_post(std::move(post)) {}

bool QueuedCalls::run(std::function<void()> work, std::optional<std::chrono::milliseconds> bound)
{
    if (is_closed())
        return false;

    // A close() that lands between the check above and this post is harmless: the wait below sees it,
    // and the queued task sees it too and does nothing.
    auto call = std::make_shared<Call>();
    m_post([state = m_state, call, work = std::move(work), post = m_post] { run_queued(*state, *call, work, post); });

    std::unique_lock<std::mutex> lock(m_state->mutex);
    const auto released = [&] { return !call->started && (m_state->closed || call->abandoned); };
    if (bound && !m_state->changed.wait_for(lock, *bound, [&] { return call->started || released(); }))
        call->abandoned = true; // it did not start in time, and now never will
    m_state->changed.wait(lock, [&] { return call->done || released(); });
    if (!call->done)
        return false;
    if (call->error)
        std::rethrow_exception(call->error);
    return true;
}

void QueuedCalls::run_queued(State& state, Call& call, const std::function<void()>& work, const Post& post)
{
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.closed || call.abandoned)
            return; // its caller has been released already: what `work` uses may be gone
        call.started = true;
        ++state.running;
    }
    state.changed.notify_all();

    std::exception_ptr error;
    try {
        work();
    } catch (...) {
        error = std::current_exception();
    }

    std::vector<std::function<void()>> after_work;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        call.error = error;
        call.done  = true;
        if (--state.running == 0)
            after_work.swap(state.after_work);
    }
    state.changed.notify_all();
    for (auto& task : after_work)
        post(std::move(task));
}

bool QueuedCalls::close()
{
    bool changed;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        changed          = !m_state->closed;
        m_state->closed = true;
    }
    m_state->changed.notify_all();
    return changed;
}

bool QueuedCalls::is_closed() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->closed;
}

bool QueuedCalls::defer_until_work_ends(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->running == 0)
        return false;
    m_state->after_work.push_back(std::move(task));
    return true;
}

bool run_queued_and_wait(const QueuedCalls::Post& queue, std::function<void()> fn, std::chrono::milliseconds bound)
{
    return QueuedCalls(queue).run(std::move(fn), bound);
}

} // namespace Slic3r
