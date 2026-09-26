#include "OrcaMCPMainThreadGate.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

struct MainThreadGate::State
{
    mutable std::mutex      mutex;
    std::condition_variable changed;
    bool                    closed  = false;
    int                     running = 0; // calls whose work has started and not finished
};

// One call's progress. Guarded by State::mutex.
struct MainThreadGate::Call
{
    bool               started = false;
    bool               done    = false;
    nlohmann::json     value;
    std::exception_ptr error;
};

MainThreadGate::MainThreadGate(Post post) : m_state(std::make_shared<State>()), m_post(std::move(post)) {}

nlohmann::json MainThreadGate::call(Work work)
{
    if (is_closed())
        throw McpShuttingDown();

    // A close() that lands between the check above and this post is harmless: the wait below sees
    // it, and the queued task sees it too and does nothing.
    auto call = std::make_shared<Call>();
    m_post([state = m_state, call, work = std::move(work)]() { run_queued(*state, *call, work); });

    std::unique_lock<std::mutex> lock(m_state->mutex);
    m_state->changed.wait(lock, [&] { return call->done || (m_state->closed && !call->started); });
    if (!call->done)
        throw McpShuttingDown();
    if (call->error)
        std::rethrow_exception(call->error);
    return std::move(call->value);
}

void MainThreadGate::run_queued(State& state, Call& call, const Work& work)
{
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.closed)
            return; // its caller has been released already
        call.started = true;
        ++state.running;
    }

    nlohmann::json     value;
    std::exception_ptr error;
    try {
        value = work();
    } catch (...) {
        error = std::current_exception();
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        call.value = std::move(value);
        call.error = error;
        call.done  = true;
        --state.running;
    }
    state.changed.notify_all();
}

void MainThreadGate::close()
{
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->closed = true;
    }
    m_state->changed.notify_all();
}

bool MainThreadGate::is_closed() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->closed;
}

bool MainThreadGate::work_in_progress() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->running > 0;
}

}}} // namespace Slic3r::GUI::OrcaMCP
