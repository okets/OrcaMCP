// src/slic3r/Utils/QueuedCall.hpp
#pragma once
#include <chrono>
#include <functional>
#include <exception>
#include <memory>
#include <optional>

namespace Slic3r {

// Work one thread hands to another thread's queue, and waits for. OrcaMCP's MainThreadGate (an MCP call
// waiting for the GUI thread) and the printer agents' bounded GUI-thread calls both go through it, so
// they keep one set of rules:
//
// - In run(), work that has started is waited for, however long it takes: it may use what its caller owns, so it
//   may capture the caller's locals by reference.
// - A caller released before its work started -- by close(), or by its bound running out -- gets false
//   at once, and that work never runs, even when the queue reaches it later.
// - What the work throws is rethrown to its caller, which is always released.
//
// run_owned_until is the separate strict-deadline contract for value-owned work;
// it can release a caller while its work finishes safely. Existing run() callers
// keep the reference-lifetime behavior above.
//
// No wx: the queue is passed in, so the tests drive it with queues of their own
// (tests/slic3rutils/test_queued_call.cpp, test_mcp_shutdown.cpp).
class QueuedCalls
{
public:
    // Queues a task to run later on the other thread. It must not run the task before returning.
    using Post = std::function<void(std::function<void()>)>;

    explicit QueuedCalls(Post post);

    // Runs `work` through the queue and waits for it. true: it ran to its end; false: the caller was
    // released before it started, by close() or once `bound` (when given) ran out. What `work` threw
    // is rethrown here.
    bool run(std::function<void()> work, std::optional<std::chrono::milliseconds> bound = std::nullopt);

    // Strict caller deadline, including work that has already started. Unlike run(),
    // work MUST own all captured state: it can continue after this returns false.
    // Work still queued at the deadline is abandoned and never starts.
    bool run_owned_until(std::function<void()> work, std::chrono::steady_clock::time_point deadline);

    // Nonblocking, value-owned cleanup. A closed gate skips it (the native app
    // owns shutdown teardown). on_error runs on the queue thread and must not throw.
    void post_owned(std::function<void()> work, std::function<void(std::exception_ptr)> on_error);

    // Releases every caller whose work has not started, and makes every later run() return false.
    // Once closed it stays closed. Returns true when this call closed it. Safe from any thread.
    bool close();
    bool is_closed() const;

    // While some work is running, keeps `task` to post once none is, and returns true; otherwise does
    // nothing and returns false.
    bool defer_until_work_ends(std::function<void()> task);

private:
    struct State;
    struct Call;
    static void run_queued(State& state, Call& call, const std::function<void()>& work, const Post& post);

    std::shared_ptr<State> m_state; // shared with every queued task, which can outlive this and its caller
    Post                   m_post;
};

// One call through `queue` under those rules, its wait to start bounded by `bound`. true once `fn` has
// run to its end; false when it had not started within `bound`, and never will.
bool run_queued_and_wait(const QueuedCalls::Post& queue, std::function<void()> fn, std::chrono::milliseconds bound);

} // namespace Slic3r
