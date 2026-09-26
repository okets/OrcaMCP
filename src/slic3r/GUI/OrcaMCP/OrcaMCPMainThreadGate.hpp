// src/slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp
#pragma once
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include "OrcaMCPJsonRpcError.hpp"

// How an MCP call on the HTTP thread hands work to the main thread and waits for it, and how the
// app's quit releases that wait. No wx: the main thread's queue is passed in, so the tests drive it
// with a queue of their own (tests/slic3rutils/test_mcp_shutdown.cpp).
//
// Why a gate and not a bare promise: when the app quits, the main thread joins the HTTP thread
// (HttpServer::stop), while an MCP call on the HTTP thread waits for the main thread to run its work.
// Nothing broke that wait, so the two blocked each other forever: on 2026-09-26 quit_app, with a
// script polling get_slicing_status, left the app hung for 15 minutes. close() is the break.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

class MainThreadGate
{
public:
    using Work = std::function<nlohmann::json()>;
    // Queues a task to run later on the main thread. It must not run the task before returning.
    using Post = std::function<void(std::function<void()>)>;

    explicit MainThreadGate(Post post);

    // Runs `work` on the main thread and returns what it returned; an exception it throws is
    // rethrown here. Throws McpShuttingDown, without running `work`, when the gate is closed before
    // the work starts. Once the work has started, this waits for it to finish even if the gate
    // closes meanwhile: the work may still be using what its caller owns.
    nlohmann::json call(Work work);

    // Refuses every later call, and releases every caller whose work has not started with
    // McpShuttingDown. A released caller's queued task does nothing when the main thread reaches it.
    // Idempotent; safe from any thread. Once closed, it stays closed: this is the app's one "quitting"
    // signal.
    void close();
    bool is_closed() const;

    // True while a call's work is running. Asked on the main thread, that means the asker is inside
    // that work -- it pumped the event loop into a quit -- and the work's caller is still waiting.
    bool work_in_progress() const;

private:
    struct State;
    struct Call;
    static void run_queued(State& state, Call& call, const Work& work);

    // Shared with every queued task, which can outlive both the caller it served and this gate.
    std::shared_ptr<State> m_state;
    Post                   m_post;
};

// The app's gate to the wx main thread, posting through wxGetApp().CallAfter (defined in
// OrcaMCPCommon.cpp). OrcaMCPServer::shut_down() closes it, when the main frame starts to close.
MainThreadGate& main_thread_gate();

}}} // namespace Slic3r::GUI::OrcaMCP
