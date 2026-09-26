# OrcaMCP Threading Model

## Overview

OrcaMCP must bridge two threading models:
1. **HTTP Server**: one thread, which calls the request handler itself, so requests are served one
   at a time
2. **OrcaSlicer GUI**: Single-threaded, requires main thread for all operations

This document explains how we safely execute MCP tool handlers.

## The Problem

```
┌─────────────────┐     ┌─────────────────┐
│  HTTP Worker    │     │   Main Thread   │
│    Thread       │     │     (GUI)       │
├─────────────────┤     ├─────────────────┤
│                 │     │                 │
│ Receives MCP    │     │ wxWidgets       │
│ request         │     │ event loop      │
│                 │     │                 │
│ Needs to call   │     │ OpenGL context  │
│ Plater methods  │ ✗   │ bound here      │
│                 │     │                 │
│ Can't directly  │     │ Model state     │
│ access GUI!     │     │ lives here      │
│                 │     │                 │
└─────────────────┘     └─────────────────┘
```

**Why GUI operations require main thread:**
- wxWidgets: All widget operations must be on main thread
- OpenGL: Context created on main thread, most operations require same thread
- Model state: Not thread-safe, modified by GUI interactions

## The Solution: run_on_main_thread()

Work is handed to the main thread through the app's `MainThreadGate`
(`src/slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.cpp`), which queues it with wxWidgets' `CallAfter()`
and waits for it:

```cpp
template<typename Func>
nlohmann::json run_on_main_thread(Func&& func)
{
    return main_thread_gate().call(MainThreadGate::Work(std::forward<Func>(func)));
}
```

`MainThreadGate::call` queues a task that runs `func` on the main thread, then waits until the task
has finished or the gate is closed. The task shares its state with the caller through a
`shared_ptr`, so a caller that was released early (see Shutdown below) leaves nothing dangling.

## Execution Flow

```
HTTP Worker Thread                    Main Thread
      │                                    │
      │ handle_load_model() called         │
      │                                    │
      │ run_on_main_thread([&]() {         │
      │   // closure defined               │
      │ });                                │
      │                                    │
      │ ─────── CallAfter() ─────────────► │
      │                                    │
      │ waits on the gate                  │ ◄─── Event loop picks up
      │ (BLOCKED)                          │
      │                                    │ Execute closure:
      │                                    │   - Access Plater
      │                                    │   - Load model
      │                                    │   - Update GUI
      │                                    │
      │ ◄──── result, and a notify ─────── │
      │                                    │
      │ (unblocked)                        │
      │ return result                      │
      │                                    │
```

## Tool Handler Pattern

Every tool handler follows this pattern:

```cpp
json OrcaMCPServer::handle_some_tool(const json& params) {
    return run_on_main_thread<json>([&]() {
        // SAFE: This code runs on main thread

        auto* plater = wxGetApp().plater();
        if (!plater) {
            return json{{"error", "No plater available"}};
        }

        // Perform GUI operations
        plater->some_operation();

        // Return result
        return json{{"status", "success"}};
    });
}
```

## What Requires Main Thread

| Operation | Why |
|-----------|-----|
| Model loading | Updates GUI, triggers repaints |
| Object transforms | Modifies model state, updates canvas |
| Plate operations | Changes build plate, updates view |
| Preset changes | Updates sidebar, triggers refresh |
| Slicing start | Background thread spawned from main |
| Preview rendering | OpenGL context access |
| Undo/redo | State stack manipulation |
| Export operations | May show dialogs |

## Consequences

### Blocking Behavior
- HTTP worker thread is blocked during execution
- Only one tool can execute at a time on GUI thread
- Long operations delay response

### Timeout Handling
The bridge script has a 120-second default timeout:
```bash
ORCAMCP_TIMEOUT=120  # seconds
```

Long operations that may approach this limit:
- Large model slicing
- Complex auto-orient calculations
- Multi-object arrangements
- High-resolution preview rendering

### No Parallelism
Sequential execution on main thread means:
- Tools cannot run concurrently
- Order of tool calls is preserved
- No race conditions possible

## Async Operations

Some OrcaSlicer operations are inherently async:

```cpp
// slice_all starts background slicing
json handle_slice_all(const json& params) {
    return run_on_main_thread<json>([&]() {
        auto* plater = wxGetApp().plater();
        plater->reslice();  // Returns immediately
        return json{{"status", "slicing_started"}};
    });
}
```

Callers must poll for completion:
```cpp
// get_slicing_status checks background state
json handle_get_slicing_status(const json& params) {
    return run_on_main_thread<json>([&]() {
        auto* plater = wxGetApp().plater();
        bool is_slicing = plater->is_slicing();
        return json{{"is_slicing", is_slicing}};
    });
}
```

## Exception Handling

An exception thrown by the work on the main thread is caught there and rethrown on the HTTP thread by
`MainThreadGate::call`. `handle_tools_call` turns it into a JSON-RPC error naming the tool (-32603),
except `McpShuttingDown`, which `handle_request` answers as -32002 (below).

## Shutdown

Quitting joins the HTTP thread from the main thread (`GUI_App::stop_http_server` ->
`HttpServer::stop`). A call waiting in `run_on_main_thread` is waiting for that same main thread, so
before 2026-09-26 the two waited on each other forever: `quit_app`, or a Cmd-Q, with an agent polling
left the app hung until it was killed.

```
HTTP thread                               Main thread
    │ get_slicing_status                      │ quit_app's Close(true)
    │ run_on_main_thread: CallAfter(work) ──► │ queued behind the close
    │ waits for the main thread               │ close handler -> MainFrame::shutdown
    │                                         │ -> GUI_App::shutdown -> stop_http_server
    │                                         │ -> HttpServer::stop -> join()
    │ ...forever                              │ ...forever
```

What breaks the cycle, in order:

1. The main frame's close handler, where it sets `set_closing(true)` (the close can no longer be
   vetoed), calls `OrcaMCPServer::shut_down()`, which closes the gate: the app's one "quitting"
   signal. The waiting call is released with `McpShuttingDown`, answered with JSON-RPC **-32002**
   ("OrcaMCP is quitting, so this call was not run. Use start_orca to start it again."), work still
   queued is never run, and every later `tools/call` is refused. That matters because the handler
   goes on to reset the plater and tear the frame down before the server stops. A call whose work has
   already started is waited for instead, since the work may still use what the caller owns.
2. A call blocked on the network rather than on the main thread gives up too. `GUI_App`'s route puts a
   `ScopedThreadCancelCheck` (`src/slic3r/Utils/ThreadCancel.hpp`) in scope for every request, tied to
   the gate: synchronous `Http` transfers abort within about a second and report "Request
   cancelled", and `discover_printers` stops listening. The call returns its tool error.
3. `GUI_App::stop_http_server()` stops the server. `HttpServer::stop` closes the listeners and the idle
   connections, lets a reply that is still being written finish (up to 2 s), and joins the thread.
   It never abandons a handler that is still running, since the handler may use what the app
   destroys next; past 3 s it logs that it is still waiting.

When the quit itself runs inside a tool call's work (the work pumped the event loop into a close), the
call's caller waits on that work, so the join would never end. `stop_http_server` sees it
(`OrcaMCPServer::inside_a_tool_call()`) and leaves the stop to `OnExit`.

`tests/slic3rutils/test_mcp_shutdown.cpp`, `test_http_server.cpp` and `test_thread_cancel.cpp` cover
each step with a main thread that never runs its work, a server that never answers, and a client slow
to read.

## Common Pitfalls

### 1. Forgetting run_on_main_thread
```cpp
// WRONG: Direct GUI access from HTTP thread
json handle_bad(const json& params) {
    auto* plater = wxGetApp().plater();  // Race condition!
    plater->do_something();  // May crash!
}

// CORRECT: Wrap in run_on_main_thread
json handle_good(const json& params) {
    return run_on_main_thread<json>([&]() {
        auto* plater = wxGetApp().plater();
        plater->do_something();
        return json{{"status", "ok"}};
    });
}
```

### 2. Capturing References to Temporaries
```cpp
// WRONG: params may be destroyed before main thread executes
json handle_bad(const json& params) {
    return run_on_main_thread<json>([params]() {  // Copy, don't reference
        // Use params here
    });
}
```

### 3. Long Operations Without Timeout Awareness
```cpp
// BAD: No indication this may take long
json handle_slice(const json& params) {
    return run_on_main_thread<json>([&]() {
        slice_everything();  // May take 10 minutes!
        return json{{"done", true}};
    });
}

// BETTER: Start async, let caller poll
json handle_slice(const json& params) {
    return run_on_main_thread<json>([&]() {
        start_slicing_async();
        return json{{"status", "started"}};
    });
}
```

## Related Documentation

- [ADR-0004: Main Thread Execution](../adr/0004-main-thread-execution.md)
- [Architecture Overview](overview.md)
