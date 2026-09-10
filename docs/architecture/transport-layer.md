# OrcaMCP Transport Layer

## Overview

OrcaMCP uses a two-layer transport architecture to bridge the gap between MCP's stdio-based protocol and OrcaSlicer's GUI application nature.

```
┌───────────────┐   stdio    ┌──────────────────┐   HTTP    ┌─────────────┐
│  MCP Client   │ ◄────────► │orcamcp-bridge.py │ ◄───────► │ OrcaSlicer  │
│(Claude Code)  │            │    (Python)      │           │ Port 13618  │
└───────────────┘            └──────────────────┘           └─────────────┘
```

## Why Two Layers?

### The stdio Requirement
MCP specification defines stdio as the primary transport:
- MCP client spawns server as subprocess
- Communication via stdin/stdout
- Simple, cross-platform, no network configuration

### The GUI Application Problem
OrcaSlicer is a GUI application:
- Doesn't reliably have stdin/stdout (especially on macOS/Windows)
- Has its own event loop (wxWidgets)
- Can't be spawned as a simple subprocess

### The Solution
Use OrcaSlicer's existing HTTP server and add a lightweight bridge:
- Bridge handles stdio ↔ HTTP translation
- OrcaSlicer handles actual tool execution
- Clean separation of concerns

## Layer 1: stdio Bridge (orcamcp-bridge.py)

### Purpose
Translate MCP stdio transport to HTTP requests.

### Configuration
Environment variables:
```bash
ORCAMCP_HOST=localhost    # OrcaSlicer host
ORCAMCP_PORT=13618        # OrcaSlicer HTTP port
ORCAMCP_TIMEOUT=120       # Request timeout (seconds)
ORCAMCP_DEBUG=1           # Enable debug logging to stderr
```

### Implementation

```python
def send_request(request_data):
    """Forward JSON-RPC request to OrcaSlicer HTTP server."""
    url = f"http://{host}:{port}/mcp"
    req = urllib.request.Request(
        url,
        data=json.dumps(request_data).encode('utf-8'),
        headers={'Content-Type': 'application/json'},
        method='POST'
    )
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.loads(response.read().decode('utf-8'))

def main():
    """Main loop: read stdin, forward to HTTP, write to stdout."""
    for line in sys.stdin:
        request = json.loads(line)
        response = send_request(request)
        print(json.dumps(response), flush=True)
```

### Error Handling
The bridge handles several error conditions:

| Error | Response |
|-------|----------|
| Nothing listening | JSON-RPC error -32000 "Nothing is listening at {URL}. OrcaMCP is not running -- use the 'start_orca' tool." |
| Reachable but the request failed | JSON-RPC error -32000 "OrcaSlicer is reachable but the request did not complete: ..." |
| Timeout | JSON-RPC error -32000 "Request timed out" |
| Invalid JSON | JSON-RPC error -32700 "Parse error" |
| HTTP error | JSON-RPC error with HTTP status |

### MCP Configuration
In `.mcp.json`:
```json
{
  "mcpServers": {
    "orca-slicer": {
      "command": "python3",
      "args": ["./scripts/orcamcp-bridge.py"]
    }
  }
}
```

## Layer 2: HTTP Server (OrcaSlicer)

### Existing Infrastructure
OrcaSlicer already has an HTTP server for:
- BBL printer authentication
- LAN device communication
- Remote monitoring features

### MCP Route Registration
In `GUI_App.cpp`:
```cpp
m_http_server.set_request_handler(
    [](const std::string& method,
       const std::string& url,
       const std::string& body) -> std::shared_ptr<HttpServer::Response> {

        // Route /mcp to OrcaMCPServer
        if (url.find("/mcp") != std::string::npos) {
            return OrcaMCPServer::handle_request(method, url, body);
        }

        // Fall back to existing handlers
        return HttpServer::bbl_auth_handle_request(method, url, body);
    });

m_http_server.start();  // Starts on port 13618
```

### Request Processing
1. HTTP server receives POST to `/mcp`
2. Body contains JSON-RPC 2.0 request
3. `OrcaMCPServer::handle_request()` parses and dispatches
4. Tool handler executes on main thread
5. Response returned as JSON

### Response Format
All responses are `Content-Type: application/json`:
```cpp
return std::make_shared<HttpServer::ResponseJson>(result_json);
```

## Protocol: JSON-RPC 2.0

### Request Structure
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "load_model",
    "arguments": {
      "file_path": "/path/to/model.stl"
    }
  }
}
```

### Response Structure (Success)
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"status\": \"success\", \"object_id\": \"abc123\"}"
    }]
  }
}
```

### Response Structure (Error)
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32602,
    "message": "Invalid params: file_path is required"
  }
}
```

## Testing the Transport

### Test HTTP Layer Directly
```bash
# Health check
curl -s http://localhost:13618/mcp | jq .

# List tools
curl -s -X POST http://localhost:13618/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' | jq .

# Call a tool
curl -s -X POST http://localhost:13618/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_scene_info","arguments":{}}}' | jq .
```

### Test Bridge Layer
```bash
# Send request through bridge
echo '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' | python3 scripts/orcamcp-bridge.py

# With debug output
ORCAMCP_DEBUG=1 echo '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}' | python3 scripts/orcamcp-bridge.py
```

### Test Full MCP Stack
Use Claude Code with the `.mcp.json` configuration and observe the tools available.

## Troubleshooting

### Nothing Listening
```
Nothing is listening at http://localhost:13618/mcp. OrcaMCP is not running -- use the 'start_orca' tool.
```
**Causes:**
- OrcaSlicer not running
- HTTP server not started (check OrcaSlicer logs)
- Port conflict (another app using 13618)

**Solutions:**
- Launch OrcaSlicer
- Check if MCP server initialized in OrcaSlicer console
- Change port via `ORCAMCP_PORT` env var

### Reachable But Not Answering
```
OrcaSlicer is reachable but the request did not complete: ...
```
Something answered at the port, so the app is running. The bridge never turns this into
"not running": a probe that fails is not proof of death, only proof of no answer.

**Solutions:**
- Retry; the server handles one request at a time, so a burst queues
- Raise `ORCAMCP_TIMEOUT` if a slice, render or export is in progress

### Timeout Errors
```
Error: Request timed out after 120 seconds
```
**Causes:**
- Long-running operation (large model slicing)
- OrcaSlicer frozen/busy
- Network issues (shouldn't happen on localhost)

**Solutions:**
- Increase timeout: `ORCAMCP_TIMEOUT=300`
- For slicing, use `slice_all` then poll `get_slicing_status`
- Check OrcaSlicer is responsive

### Parse Errors
```
Error: Invalid JSON in request
```
**Causes:**
- Malformed JSON sent to bridge
- Encoding issues
- Incomplete request

**Solutions:**
- Validate JSON before sending
- Ensure UTF-8 encoding
- Check for truncated input

## Security Considerations

### Localhost Only
The HTTP server binds to localhost by default:
- Only local processes can connect
- No authentication required
- Suitable for single-user development machine

### No Authentication
- Anyone with localhost access can call tools
- Tools can modify files on disk (export, load)
- Tools can interact with printers

### Recommendations
- Don't expose port 13618 to network
- Run OrcaSlicer as unprivileged user
- Be cautious with `send_to_printer` tool

## Related Documentation

- [ADR-0001: HTTP Transport](../adr/0001-http-transport.md)
- [Architecture Overview](overview.md)
- [Threading Model](threading-model.md)
