// src/slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp
#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include "OrcaMCPJsonRpcError.hpp"
#include "slic3r/Utils/QueuedCall.hpp"

// How an MCP call on the HTTP thread hands work to the main thread and waits for it, and how the
// app's quit releases that wait.
//
// Why a gate and not a bare promise: when the app quits, the main thread joins the HTTP thread
// (HttpServer::stop), while an MCP call on the HTTP thread waits for the main thread to run its work.
// Nothing broke that wait, so the two blocked each other forever: on 2026-09-26 quit_app, with a
// script polling get_slicing_status, left the app hung for 15 minutes. close() is the break.
//
// The gate is QueuedCalls (slic3r/Utils/QueuedCall.hpp), whose rules it relies on: a call released
// before its work started never has it run, work that started is waited for, what it throws is
// rethrown. close() refuses every later call and releases the waiting ones; it is the app's one
// "quitting" signal. defer_until_work_ends() is how the main frame's close handler waits for a tool
// call's work that pumped the event loop into it. No wx: the tests drive it with queues of their own
// (tests/slic3rutils/test_mcp_shutdown.cpp).

namespace Slic3r { namespace GUI { namespace OrcaMCP {

using MainThreadGate = QueuedCalls;
using McpWork        = std::function<nlohmann::json()>;

// Runs `work` through the gate and returns what it returned. Throws McpShuttingDown, without running
// `work`, when the gate is closed before the work starts.
inline nlohmann::json call_through(MainThreadGate& gate, const McpWork& work)
{
    // By reference: the gate never runs the work once this caller has been released, and waits for it
    // once it has started.
    nlohmann::json value;
    if (!gate.run([&] { value = work(); }))
        throw McpShuttingDown();
    return value;
}

// The app's gate to the wx main thread, posting through wxGetApp().CallAfter (defined in
// OrcaMCPCommon.cpp). OrcaMCPServer::shut_down() closes it, when the main frame starts to close.
MainThreadGate& main_thread_gate();

}}} // namespace Slic3r::GUI::OrcaMCP
