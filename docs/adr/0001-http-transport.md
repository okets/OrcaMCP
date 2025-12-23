# ADR-0001: HTTP Transport with stdio Bridge

## Status
Accepted

## Context

The Model Context Protocol (MCP) specification defines stdio as the primary transport mechanism for communication between AI assistants and tool servers. MCP clients like Claude Code read from stdin and write to stdout to communicate with MCP servers.

However, OrcaSlicer is a GUI application built with wxWidgets and OpenGL. GUI applications on most platforms:
1. Do not have reliable access to stdin/stdout (especially on macOS/Windows)
2. Already have an event loop that conflicts with blocking stdin reads
3. Cannot be easily started as a subprocess by MCP clients

OrcaSlicer already includes an HTTP server (port 13618) for features like BBL printer authentication and LAN communication.

## Decision

We implement a **two-layer transport architecture**:

1. **HTTP Layer**: Embed the MCP server directly in OrcaSlicer's existing HTTP server
   - Handles JSON-RPC 2.0 requests on the `/mcp` endpoint
   - Reuses existing HTTP infrastructure
   - Runs on port 13618 (configurable)

2. **stdio Bridge**: Create a lightweight Python script (`orcamcp-bridge.py`) that:
   - Reads MCP JSON-RPC requests from stdin
   - Forwards them as HTTP POST requests to OrcaSlicer
   - Writes HTTP responses back to stdout

```
┌─────────────┐     stdio     ┌──────────────────┐     HTTP      ┌─────────────┐
│ Claude Code │ ←──────────→  │orcamcp-bridge.py │ ←──────────→  │ OrcaSlicer  │
│   (MCP)     │               │    (Python)      │               │ Port 13618  │
└─────────────┘               └──────────────────┘               └─────────────┘
```

## Consequences

### Positive
- **Clean separation**: Bridge is simple (~137 lines), server handles complexity
- **Reuses infrastructure**: No new HTTP server needed in OrcaSlicer
- **Standard protocol**: HTTP is well-understood, debuggable with curl
- **Language flexibility**: Bridge could be rewritten in any language
- **Works with existing OrcaSlicer**: No major architectural changes required
- **Testable independently**: Can test HTTP server with curl, bridge separately

### Negative
- **Extra process**: MCP clients must spawn the bridge script
- **Network dependency**: Relies on localhost networking (rarely an issue)
- **Two configs**: Bridge has env vars, OrcaSlicer has port config
- **Latency**: Extra hop through bridge (negligible in practice)

### Neutral
- **Python dependency**: Bridge requires Python 3.x (widely available)
- **Port conflict possible**: If port 13618 is in use, must configure alternative

## Implementation Notes

The bridge script is configured via environment variables:
- `ORCAMCP_HOST`: Defaults to `localhost`
- `ORCAMCP_PORT`: Defaults to `13618`
- `ORCAMCP_TIMEOUT`: Defaults to `120` seconds (for long slicing operations)
- `ORCAMCP_DEBUG`: Enable debug logging to stderr
