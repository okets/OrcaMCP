# OrcaMCP Threading Model

## Overview

OrcaMCP must bridge two threading models:
1. **HTTP Server**: Multi-threaded, handles requests concurrently
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

We use a promise/future pattern with wxWidgets' `CallAfter()`:

```cpp
template<typename T>
T run_on_main_thread(std::function<T()> func) {
    // Create a promise to hold the result
    std::promise<T> promise;
    auto future = promise.get_future();

    // Schedule work on main thread
    wxGetApp().CallAfter([&promise, &func]() {
        try {
            // Execute on main thread, set result
            promise.set_value(func());
        } catch (...) {
            // Propagate exceptions
            promise.set_exception(std::current_exception());
        }
    });

    // Block until main thread completes
    return future.get();
}
```

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
      │ future.get()                       │ ◄─── Event loop picks up
      │ (BLOCKED)                          │
      │                                    │ Execute closure:
      │                                    │   - Access Plater
      │                                    │   - Load model
      │                                    │   - Update GUI
      │                                    │
      │ ◄──── promise.set_value() ──────── │
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

Exceptions in tool handlers are properly propagated:

```cpp
wxGetApp().CallAfter([&promise, &func]() {
    try {
        promise.set_value(func());
    } catch (...) {
        // Capture any exception
        promise.set_exception(std::current_exception());
    }
});

// In HTTP handler:
try {
    return future.get();  // May throw
} catch (const std::exception& e) {
    return make_error_response(e.what());
}
```

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
