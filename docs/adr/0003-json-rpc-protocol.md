# ADR-0003: JSON-RPC 2.0 Protocol

## Status
Accepted

## Context

The Model Context Protocol (MCP) specification requires JSON-RPC 2.0 as the wire protocol. This is not a choice but a requirement for MCP compatibility.

However, understanding the protocol is important for:
- Debugging communication issues
- Implementing error handling correctly
- Ensuring compatibility with MCP clients

## Decision

We implement JSON-RPC 2.0 as specified by MCP:

### Request Format
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

### Response Format (Success)
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"status\": \"success\", ...}"
    }]
  }
}
```

### Response Format (Error)
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32602,
    "message": "Invalid params: missing required parameter 'file_path'"
  }
}
```

### MCP Methods Implemented
- `initialize` - Protocol handshake
- `ping` - Health check
- `tools/list` - List available tools with schemas
- `tools/call` - Execute a tool

## Consequences

### Positive
- **MCP compatible**: Works with Claude Code, Claude Desktop, and other MCP clients
- **Standard protocol**: Well-documented, widely understood
- **Clear error codes**: Standard error codes for different failure modes
- **Stateless**: Each request is independent (no session management)
- **Debuggable**: Can test with curl, inspect with standard JSON tools

### Negative
- **Verbose**: JSON overhead for every request/response
- **No streaming**: Standard JSON-RPC doesn't support streaming responses
- **Schema duplication**: Tool schemas defined in code and repeated in responses

### Neutral
- **HTTP 200 for errors**: JSON-RPC errors return HTTP 200 (protocol requirement)
- **String IDs**: We support both integer and string request IDs

## Error Codes

We use standard JSON-RPC error codes:

| Code | Meaning |
|------|---------|
| -32700 | Parse error (invalid JSON) |
| -32600 | Invalid request |
| -32601 | Method not found |
| -32602 | Invalid params |
| -32603 | Internal error |

## Implementation Notes

Key implementation details:
- All responses are `Content-Type: application/json`
- Tool results are wrapped in MCP's `content` array format
- Errors during tool execution return JSON-RPC error, not HTTP error
- Request body parsing uses nlohmann::json library
