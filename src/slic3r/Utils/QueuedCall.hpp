// src/slic3r/Utils/QueuedCall.hpp
#pragma once
#include <chrono>
#include <functional>

namespace Slic3r {

// Runs `fn` through `queue`, which runs the task it is given later on another thread (the GUI thread's
// queue, for a printer agent), and waits for it. Returns true once `fn` has run to its end.
//
// The wait for `fn` to start is bounded: past `bound` the caller stops waiting and gets false, and a
// task the caller gave up on never runs `fn` when it is reached at last. So `fn` may capture what the
// caller owns by reference: it is never run after the caller has returned. Once `fn` has started, the
// caller waits for it to finish, however long that takes, for the same reason. This is the rule of
// OrcaMCP's MainThreadGate, for any queue.
bool run_queued_and_wait(const std::function<void(std::function<void()>)>& queue,
                         std::function<void()>                             fn,
                         std::chrono::milliseconds                         bound);

} // namespace Slic3r
