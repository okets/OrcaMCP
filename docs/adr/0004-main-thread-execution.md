# ADR-0004: Main Thread Execution

## Status
Accepted

## Context

OrcaSlicer is a GUI application using:
- **wxWidgets**: Cross-platform GUI framework
- **OpenGL**: 3D rendering for the build plate visualization

Both technologies have strict threading requirements:
- wxWidgets GUI operations must run on the main thread
- OpenGL contexts are typically bound to the thread that created them
- Model manipulation affects GUI state that must be synchronized

The HTTP server runs requests on worker threads for concurrency. This creates a conflict: MCP tool handlers need to manipulate GUI state, but they're invoked from HTTP worker threads.

## Decision

We implement a **main thread dispatch pattern** using `run_on_main_thread<T>()`:

```cpp
template<typename T>
T run_on_main_thread(std::function<T()> func) {
    std::promise<T> promise;
    auto future = promise.get_future();

    wxGetApp().CallAfter([&promise, &func]() {
        try {
            promise.set_value(func());
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });

    return future.get();  // Blocks until main thread completes
}
```

**How it works:**
1. HTTP worker thread receives MCP request
2. Worker calls tool handler
3. Handler wraps operation in `run_on_main_thread()`
4. `CallAfter()` queues work on wxWidgets main event loop
5. Worker thread blocks on `future.get()`
6. Main thread executes operation, sets promise value
7. Worker thread unblocks, returns response

## Consequences

### Positive
- **Thread safety**: All GUI operations run on correct thread
- **Simple pattern**: Single wrapper function for all handlers
- **Exception propagation**: Exceptions from main thread reach HTTP response
- **No race conditions**: Sequential execution on main thread prevents conflicts

### Negative
- **Blocking**: HTTP worker thread blocked during entire operation
- **No parallelism**: Only one tool executes at a time on GUI thread
- **Timeout risk**: Long operations (slicing) may timeout HTTP connection
- **Potential deadlock**: If main thread blocks on HTTP (doesn't happen currently)

### Neutral
- **Latency**: Round-trip to main thread adds minimal latency
- **Resource usage**: Worker thread held during operation

## Implementation Notes

### All Tool Handlers Must Use This Pattern

```cpp
json OrcaMCPServer::handle_load_model(const json& params) {
    return run_on_main_thread<json>([&]() {
        // Safe to access GUI here
        auto* plater = wxGetApp().plater();
        // ... manipulate model ...
        return result_json;
    });
}
```

### Timeout Configuration

The bridge script has a 120-second default timeout (`ORCAMCP_TIMEOUT`) to accommodate:
- Large model loading
- Complex slicing operations
- Multi-object arrangements

### Operations That Require Main Thread

- Model loading and manipulation
- Plate operations
- Preset changes
- OpenGL rendering (previews, thumbnails)
- Undo/redo
- Export operations

### Async Operations

Some operations are inherently async in OrcaSlicer:
- `slice_all` - Slicing runs in background thread
- `auto_orient` - Orientation computation
- `arrange_objects` - Arrangement algorithm

For these, the tool returns immediately and callers poll `get_slicing_status`.

## Alternatives Considered

### Message Queue Pattern
- Pros: Non-blocking, could batch operations
- Cons: Complex response correlation, async complexity
- Rejected: Over-engineered for current needs

### Mutex-Protected Shared State
- Pros: Simpler than cross-thread dispatch
- Cons: wxWidgets still requires main thread for GUI
- Rejected: Doesn't solve the fundamental requirement
