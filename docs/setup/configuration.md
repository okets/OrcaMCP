# OrcaMCP Configuration

This guide covers configuring OrcaMCP for use with Claude Code and other MCP clients.

## Claude Code Configuration

### Automatic (Recommended)

OrcaMCP includes a `.mcp.json` file in the repository root:

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

When you open the OrcaMCP project directory in Claude Code, the tools are automatically available.

### Manual Configuration

For global availability, add to `~/.claude.json`:

```json
{
  "mcpServers": {
    "orca-slicer": {
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
| `ORCAMCP_HOST` | `127.0.0.1` | Where the bridge looks for OrcaMCP before it has chosen an instance: `127.0.0.1` or `localhost`. The app listens on 127.0.0.1 only, so another machine cannot reach it |
| `ORCAMCP_PORT` | (unset: `13618`) | Set: the session uses the OrcaMCP on this port and only that one, even with several running; with nothing there, calls answer "No OrcaMCP answers on port N" and no other instance is used. Unset: the only running instance is used (see "Multiple OrcaMCP Instances") |
| `ORCAMCP_INSTANCES_DIR` | `~/.orcamcp/instances` | Where each running instance publishes its entry, read by the app and the bridge alike. For tests |
| `ORCAMCP_TIMEOUT` | `120` | Request timeout in seconds. It also bounds how long `arrange_objects`, `auto_orient`, `flatten_object` and `clone_object` wait for their job: the bridge sends wait_for_slice's cap (15 s below it) with every tool call, as `params._meta["orcamcp/wait_cap_s"]` |
| `ORCAMCP_DEBUG` | (unset) | Enable debug logging to stderr |
| `ORCAMCP_SKIP_CLOUD_LOGIN` | set to `1` by `start_orca` | Read by the **app**, not the bridge: skips the Orca cloud silent sign-in at startup. That sign-in reads the keychain synchronously on the GUI thread and, on macOS, can block on a permission prompt before the MCP server starts. Set it yourself if you launch the app for an agent by other means. |

### Setting Environment Variables

**In .mcp.json:**
```json
{
  "mcpServers": {
    "orca-slicer": {
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

### Ports 13618 to 13627

OrcaMCP's MCP server listens on 127.0.0.1 at the first of ports 13618 to 13627 that nothing else
answers on, so several OrcaMCP windows can run at once. A window not on 13618 shows a short
notification naming the port and who holds 13618, and Preferences > MCP shows its port.

### Port Conflicts

If another application uses a port in the range, OrcaMCP skips it. If all ten are taken, the window
runs without MCP and a warning says so. To see who holds them:

```bash
lsof -nP -iTCP -sTCP:LISTEN | grep -E ':1361[89]|:1362[0-7]'
```

## Claude Desktop Configuration

For Claude Desktop (not Claude Code CLI), add to the MCP configuration:

**macOS:** `~/Library/Application Support/Claude/claude_desktop_config.json`
**Windows:** `%APPDATA%\Claude\claude_desktop_config.json`

```json
{
  "mcpServers": {
    "orca-slicer": {
      "command": "python3",
      "args": ["/path/to/OrcaMCP/scripts/orcamcp-bridge.py"]
    }
  }
}
```

## Multiple OrcaMCP Instances

Several OrcaMCP windows can run at once, each on its own port (above). Each publishes an entry in
`~/.orcamcp/instances/<pid>.json`: its pid, port, version, program, data folder and open project. The
folder and its files are yours alone, and hold no credential.

An agent lists them with `list_instances` and chooses one with `select_instance` (by pid, port or
project). A session with one window running uses it, as before. With several, a session that has not
chosen is refused with the list, and a chosen window that quits is never replaced by another silently.
`start_orca` names the window it launched or found. See `docs/tools/reference.md`, "Bridge Tools".

Windows that share a data folder overwrite each other's settings and presets as they save them; give a
second window its own with `--datadir <folder>`.

## Verifying Configuration

### Test the Connection

```bash
# Test OrcaSlicer is running and MCP is active
curl -s http://localhost:13618/mcp | jq .
```

Expected output:
```json
{
  "name": "orca-slicer",
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

The HTTP server listens on 127.0.0.1 only, for MCP and the cloud login's callback alike (before
v2.5.0.6-dev it listened on every interface):
- Only processes on the same machine can connect
- No network exposure, and no setting to turn it on

### No Authentication

The MCP server has no authentication:
- Any local process can call any tool; a web page cannot (a request with an `Origin` header, or with
  a `Host` other than `127.0.0.1`, `localhost` or `[::1]` on its port, is refused with 403)
- Tools can load/export files anywhere the user has access
- Tools can send to configured printers

### Recommendations

- Run OrcaSlicer as a regular user (not root/admin)
- Be aware that `send_to_printer` can start prints
- Review scripts before running with MCP

## See Also

- [Building](building.md) - Build from source
- [Troubleshooting](troubleshooting.md) - Common issues
