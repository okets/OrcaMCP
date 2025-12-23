# ADR-0002: Embedded Server in OrcaSlicer

## Status
Accepted

## Context

To expose MCP functionality, we needed to decide where the MCP server logic would run:

**Option A: External Server Process**
- Separate application that communicates with OrcaSlicer via IPC
- Could use DBus, named pipes, or OrcaSlicer's existing HTTP API

**Option B: Embedded Server in OrcaSlicer**
- MCP server code compiled directly into OrcaSlicer
- Direct access to internal APIs and data structures

OrcaSlicer already has an HTTP server for printer communication and authentication. The internal APIs for model manipulation, slicing, and rendering are C++ classes not designed for external access.

## Decision

We embed the MCP server directly in OrcaSlicer:

1. **OrcaMCPServer class**: Static class handling MCP protocol
2. **Route registration**: Add `/mcp` route to existing HTTP server
3. **Direct API access**: Call OrcaSlicer internals directly (Plater, Model, etc.)

```cpp
// In GUI_App.cpp
m_http_server.set_request_handler([](method, url, body) {
    if (url.find("/mcp") != std::string::npos) {
        return OrcaMCPServer::handle_request(method, url, body);
    }
    return existing_handler(method, url, body);
});
```

## Consequences

### Positive
- **Direct access**: No serialization/IPC overhead for internal calls
- **Single process**: Simpler deployment, no process coordination
- **Rich functionality**: Can access everything OrcaSlicer can do
- **Consistent state**: Always sees current model/scene state
- **Reuses HTTP server**: No additional port or server needed

### Negative
- **Tight coupling**: MCP server code depends on OrcaSlicer internals
- **Build complexity**: Must rebuild OrcaSlicer to update MCP server
- **Version lock**: MCP server version tied to OrcaSlicer version
- **Testing harder**: Can't test MCP server without full OrcaSlicer

### Neutral
- **C++ implementation**: Matches OrcaSlicer's language but more verbose than Python
- **wxWidgets dependency**: Must use CallAfter for thread safety

## Alternatives Considered

### External Python Server
Could have created a Python server using OrcaSlicer's HTTP API:
- Pros: Easier to develop, independent deployment
- Cons: Limited to existing HTTP endpoints, can't access internals
- Rejected: Too limited for rich MCP functionality

### Plugin Architecture
Could have created a plugin system:
- Pros: Clean separation, hot-reloadable
- Cons: Complex to implement, OrcaSlicer doesn't have plugin system
- Rejected: Too much infrastructure work

## Implementation Notes

Key files:
- `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp` - Class definition
- `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` - Tool implementations (~3,700 lines)
- `src/slic3r/GUI/GUI_App.cpp` - Route registration
