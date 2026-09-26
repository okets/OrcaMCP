// src/slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp
#pragma once
#include <functional>
#include <memory>
#include <stdexcept>
#include <nlohmann/json.hpp>

// How an MCP call on the HTTP thread hands work to the main thread and waits for it, and how the
// app's shutdown releases that wait. No wx: the main thread's queue is passed in, so the tests drive
// it with a queue of their own (tests/slic3rutils/test_mcp_shutdown.cpp).
//
// Why a gate and not a bare promise: when the app quits, the main thread joins the HTTP thread
// (HttpServer::stop), while an MCP call on the HTTP thread waits for the main thread to run its work.
// Nothing broke that wait, so the two blocked each other forever: on 2026-09-26 quit_app, with a
// script polling get_slicing_status, left the app hung for 15 minutes. close() is the break.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// What an MCP call that needed the main thread gets once the app has begun to quit. The work it
// asked for was not run.
struct McpShuttingDown : std::runtime_error
{
    McpShuttingDown();
};

class MainThreadGate
{
public:
    using Work = std::function<nlohmann::json()>;
    // Queues a task to run later on the main thread. It must not run the task before returning.
    using Post = std::function<void(std::function<void()>)>;
    // Asked on the main thread just before a task runs: true once the app has begun to quit, so work
    // that reaches the front of the queue during the teardown is not run against a half-closed GUI.
    using Quitting = std::function<bool()>;

    explicit MainThreadGate(Post post, Quitting quitting = {});

    // Runs `work` on the main thread and returns what it returned; an exception it throws is
    // rethrown here. Throws McpShuttingDown, without running `work`, when the gate is closed before
    // the work starts, or when `quitting` says so. Once the work has started, this waits for it to
    // finish even if the gate closes meanwhile: the work may still be using what its caller owns.
    nlohmann::json call(Work work);

    // Refuses every later call, and releases every caller whose work has not started with
    // McpShuttingDown. A released caller's queued task does nothing when the main thread reaches it.
    // Idempotent; safe from any thread.
    void close();
    bool is_closed() const;

private:
    struct State;
    struct Call;
    static void run_queued(State& state, const Quitting& quitting, Call& call, const Work& work);

    // Shared with every queued task, which can outlive both the caller it served and this gate.
    std::shared_ptr<State> m_state;
    Post                   m_post;
    Quitting               m_quitting;
};

}}} // namespace Slic3r::GUI::OrcaMCP
