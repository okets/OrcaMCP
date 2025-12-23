# OrcaMCP Configuration

This guide covers configuring OrcaMCP for use with Claude Code and other MCP clients.

## Claude Code Configuration

### Automatic (Recommended)

OrcaMCP includes a `.mcp.json` file in the repository root:

```json
{
  "mcpServers": {
    "orcamcp": {
      "command": "python3",
      "args": ["./scripts/orcamcp-bridge.py"]
    }
  }
}
```

When you open the OrcaMCP project directory in Claude Code, the tools are automatically available.

### Manual Configuration

For global availability, add to `~/.claude.json`:

```json
{
  "mcpServers": {
    "orcamcp": {
      "command": "python3",
      "args": ["/full/path/to/OrcaMCP/scripts/orcamcp-bridge.py"]
    }
  }
}
```

## Environment Variables

The bridge script accepts these environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `ORCAMCP_HOST` | `localhost` | OrcaSlicer HTTP server host |
| `ORCAMCP_PORT` | `13618` | OrcaSlicer HTTP server port |
| `ORCAMCP_TIMEOUT` | `120` | Request timeout in seconds |
| `ORCAMCP_DEBUG` | (unset) | Enable debug logging to stderr |

### Setting Environment Variables

**In .mcp.json:**
```json
{
  "mcpServers": {
    "orcamcp": {
      "command": "python3",
      "args": ["./scripts/orcamcp-bridge.py"],
      "env": {
        "ORCAMCP_TIMEOUT": "300",
        "ORCAMCP_DEBUG": "1"
      }
    }
  }
}
```

**In shell:**
```bash
export ORCAMCP_TIMEOUT=300
export ORCAMCP_DEBUG=1
python3 scripts/orcamcp-bridge.py
```

## Port Configuration

### Default Port: 13618

OrcaSlicer's HTTP server runs on port 13618 by default. This is hardcoded in OrcaSlicer.

### Port Conflicts

If another application uses port 13618:

1. Check what's using the port:
```bash
lsof -i :13618
```

2. Stop the conflicting application, or

3. Wait - OrcaSlicer may fail to start its HTTP server if port is occupied

## Claude Desktop Configuration

For Claude Desktop (not Claude Code CLI), add to the MCP configuration:

**macOS:** `~/Library/Application Support/Claude/claude_desktop_config.json`
**Windows:** `%APPDATA%\Claude\claude_desktop_config.json`

```json
{
  "mcpServers": {
    "orcamcp": {
      "command": "python3",
      "args": ["/path/to/OrcaMCP/scripts/orcamcp-bridge.py"]
    }
  }
}
```

## Multiple OrcaSlicer Instances

Currently not supported - only one OrcaSlicer instance can bind to port 13618.

To work around this:
1. Use a single OrcaSlicer instance
2. Use multiple plates within one project

## Verifying Configuration

### Test the Connection

```bash
# Test OrcaSlicer is running and MCP is active
curl -s http://localhost:13618/mcp | jq .
```

Expected output:
```json
{
  "name": "orcamcp",
  "version": "1.0.0",
  "protocol": "mcp",
  "description": "OrcaSlicer 3D Slicer MCP Server for Claude Code integration"
}
```

### Test the Bridge

```bash
# Send a test request through the bridge
echo '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}' | python3 scripts/orcamcp-bridge.py
```

Expected output:
```json
{"jsonrpc":"2.0","id":1,"result":{}}
```

### Test Claude Code Integration

In Claude Code, ask:
```
What OrcaMCP tools are available?
```

Claude should list the 44 available tools.

## OrcaSlicer Settings

### Printer Configuration for send_to_printer

To use `send_to_printer` with OctoPrint/Klipper:

1. In OrcaSlicer, go to Printer Settings
2. Under "Print Host", configure:
   - Hostname: Your OctoPrint/Klipper IP (e.g., `10.10.10.20`)
   - API Key: Your OctoPrint API key
   - Host Type: OctoPrint/Klipper/etc.

### Bed Texture (Optional)

The project includes a custom build plate texture with OrcaMCP branding:
- Located at `resources/images/OrcaMCP_buildplate.svg`
- Set `bed_custom_texture` in your machine profile to use it

## Logging

### Bridge Debug Logging

Enable debug output:
```bash
ORCAMCP_DEBUG=1 python3 scripts/orcamcp-bridge.py
```

Debug output goes to stderr, so it won't interfere with MCP protocol on stdout.

### OrcaSlicer Logging

Check OrcaSlicer's console output for MCP-related messages:
- macOS: Run from Terminal to see stdout
- Windows: Check `OrcaSlicer.log` in user data directory
- Linux: Run from terminal

## Security Considerations

### Local Only

The HTTP server binds to localhost only:
- Only processes on the same machine can connect
- No network exposure by default

### No Authentication

The MCP server has no authentication:
- Any local process can call any tool
- Tools can load/export files anywhere the user has access
- Tools can send to configured printers

### Recommendations

- Run OrcaSlicer as a regular user (not root/admin)
- Be aware that `send_to_printer` can start prints
- Review scripts before running with MCP

## See Also

- [Building](building.md) - Build from source
- [Troubleshooting](troubleshooting.md) - Common issues
