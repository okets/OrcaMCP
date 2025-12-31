# CLAUDE.md - OrcaMCP Project

This file provides guidance to Claude Code when working with the OrcaMCP project.

---

## Project Vision

**Goal:** Enable Claude Code to control all aspects of OrcaSlicer through natural language commands.

OrcaMCP adds an MCP (Model Context Protocol) server to OrcaSlicer, allowing AI assistants to:
- Load and manipulate 3D models
- Configure print settings
- Slice and export G-code
- Send prints to printers
- Visualize the build plate

### Why This Matters

Traditional 3D printing workflow requires manual interaction with slicer software. OrcaMCP enables:
- **Natural language control**: "Load this STL, orient it for minimal supports, slice at 0.2mm layer height"
- **Local-first AI**: No cloud dependency - runs entirely on your machine
- **Full automation**: Complete slicing workflows via Claude Code CLI
- **Visual feedback**: Turntable previews show results of operations

### Testing Guidelines

**IMPORTANT:** When testing OrcaMCP functionality, always use the MCP tools directly (`mcp__orca-slicer__*`), NOT direct HTTP calls via curl. The MCP interface is what we're testing - direct HTTP bypasses the bridge and doesn't validate the full integration.

---

## Architecture

```
┌─────────────────┐         ┌──────────────────────┐         ┌─────────────────┐
│   Claude Code   │  stdio  │  orcamcp-bridge.py   │  HTTP   │   OrcaSlicer    │
│      CLI        │◄───────►│     (Python)         │◄───────►│  Port 13618     │
└─────────────────┘         └──────────────────────┘         └─────────────────┘
                                                                      │
                                                                      ▼
                                                             ┌─────────────────┐
                                                             │  OrcaMCPServer  │
                                                             │                 │
                                                             │ - JSON-RPC 2.0  │
                                                             │ - 49 MCP Tools  │
                                                             │ - Thread-safe   │
                                                             └─────────────────┘
```

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Transport | HTTP + stdio bridge | OrcaSlicer is a GUI app; pure stdio doesn't work |
| Server location | Embedded in OrcaSlicer | Reuse existing HTTP server on port 13618 |
| Protocol | JSON-RPC 2.0 over HTTP | Standard MCP protocol |
| Threading | Main thread via CallAfter | OpenGL/GUI operations require main thread |

---

## Implementation Status

**Status**: Complete & Tested ✓

### MCP Tools (49 total)

| Category | Tools |
|----------|-------|
| **Scene** | `get_scene_info`, `new_project`, `load_project`, `save_project`, `export_3mf` |
| **Models** | `load_model`, `auto_orient`, `arrange_objects`, `get_object_info`, `rename_object` |
| **Transforms** | `move_object`, `rotate_object`, `scale_object`, `mirror_object`, `flatten_object`, `clone_object`, `cut_object`, `delete_object`, `transform_objects` |
| **Plates** | `add_plate`, `select_plate`, `delete_plate` |
| **Config** | `get_presets`, `get_edited_presets`, `select_preset`, `apply_config`, `clone_preset`, `save_preset`, `delete_preset`, `reset_preset`, `get_valid_config_keys` |
| **Per-Object** | `get_object_config`, `set_object_config`, `reset_object_config` |
| **Layer Ranges** | `get_object_layer_ranges`, `set_object_layer_range`, `delete_object_layer_range` |
| **Slicing** | `slice_all`, `get_slicing_status`, `export_gcode`, `get_print_estimate` |
| **Visualization** | `render_plate_view`, `get_preview_base64` |
| **Printers** | `get_printers`, `select_printer`, `send_to_printer` |
| **Adaptive** | `apply_adaptive_layer_height`, `clear_adaptive_layer_height` |
| **History** | `undo`, `redo` |
| **Info** | `get_server_info` |

---

## Documentation Index

For detailed documentation beyond this quick reference, see the `docs/` folder:

| Document | Description |
|----------|-------------|
| [Architecture Overview](docs/architecture/overview.md) | System design with detailed diagrams |
| [Threading Model](docs/architecture/threading-model.md) | GUI thread requirements and patterns |
| [Transport Layer](docs/architecture/transport-layer.md) | HTTP + stdio bridge design |
| [ADRs](docs/adr/) | Architecture Decision Records |
| [Tools Reference](docs/tools/reference.md) | All 50 tools with parameters and examples |
| [Workflows](docs/tools/workflows.md) | Common multi-tool patterns |
| [Adding Tools](docs/contributing/adding-tools.md) | How to add new MCP tools |
| [Code Style](docs/contributing/code-style.md) | C++ and Python conventions |
| [Building](docs/setup/building.md) | Cross-platform build guide |
| [Configuration](docs/setup/configuration.md) | Claude Code and environment setup |
| [Troubleshooting](docs/setup/troubleshooting.md) | Common issues and solutions |

---

## Quick Start

### Test the MCP Server

```bash
# Check server info
curl -s http://localhost:13618/mcp | jq .

# List available tools
curl -s -X POST http://localhost:13618/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' | jq .

# Get scene info
curl -s -X POST http://localhost:13618/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_scene_info","arguments":{}}}' | jq .
```

### Claude Code Configuration

The project includes `.mcp.json` for automatic configuration:

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

---

## Build Commands

```bash
# Build everything (first time)
./build_release_macos.sh

# Build only slicer (after deps built)
./build_release_macos.sh -s

# Copy to Applications
cp -R build/arm64/src/Release/OrcaSlicer.app /Applications/
```

---

## Key Files Reference

| File | Purpose |
|------|---------|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp` | MCP server class definition |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` | 48 tool implementations (~3,900 lines) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.cpp` | Plate rendering, turntable previews |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp` | Preset/config management |
| `src/slic3r/GUI/HttpServer.hpp` | HTTP server with JSON responses |
| `src/slic3r/GUI/HttpServer.cpp` | POST body reading, ResponseJson |
| `src/slic3r/GUI/GUI_App.cpp` | MCP route registration, HTTP server startup |
| `scripts/orcamcp-bridge.py` | stdio-to-HTTP bridge for Claude Code |

---

## Adding New Tools

### 1. Register the tool in `OrcaMCPServer.cpp`

Find `register_builtin_tools()` and add:

```cpp
register_tool("my_new_tool",
    "Description of what the tool does",
    {
        // JSON Schema for parameters
        {"param1", {{"type", "string"}, {"description", "What param1 does"}}},
        {"param2", {{"type", "integer"}, {"description", "What param2 does"}}}
    },
    {"param1"},  // Required parameters
    [this](const json& params) -> json {
        return handle_my_new_tool(params);
    }
);
```

### 2. Implement the handler

```cpp
json OrcaMCPServer::handle_my_new_tool(const json& params) {
    return run_on_main_thread<json>([&]() {
        // Your implementation here
        // Use wxGetApp().plater() to access the Plater
        // Return JSON result
        return {{"status", "success"}};
    });
}
```

### 3. Add to header if needed

If implementing as a separate method, declare in `OrcaMCPServer.hpp`.

---

## Threading Model

All tool handlers use `run_on_main_thread()` which:
- Blocks the HTTP worker thread until GUI operation completes
- Required for OpenGL rendering and wxWidgets operations
- Uses `wxGetApp().CallAfter()` internally

```cpp
template<typename T>
T run_on_main_thread(std::function<T()> func);
```

---

## Code Style Standards

- **Single Responsibility:** Each method has one clear purpose
- **DRY:** Validation logic is centralized
- **Clean Code:** Methods are small, focused, and well-named
- **C++17 standard** with selective C++20 features
- **Naming:** PascalCase for classes, snake_case for functions/variables

---

## Known Limitations

1. **Export dialogs**: `export_gcode` and `export_3mf` open file dialogs if no path specified
2. **Slicing progress**: `get_slicing_status` reports running/idle, not percentage
3. **Threading**: Long operations may cause HTTP timeouts (120s default)

---

## Windows Compatibility

### Path Handling

The bridge script automatically normalizes file paths on Windows. Both forward slashes and backslashes work:

```python
# Both of these work on Windows:
load_model(file_path="C:/Models/benchy.stl")
load_model(file_path="C:\\Models\\benchy.stl")
```

Path normalization is applied to: `file_path`, `output_path`, `path` parameters.

### Executable Location

On Windows, OrcaMCP installs to:
- `C:\Program Files\OrcaMCP\orca-mcp.exe`

The bridge script searches these locations automatically:
- `%ProgramFiles%\OrcaMCP\orca-mcp.exe`
- `%ProgramFiles(x86)%\OrcaMCP\orca-mcp.exe`
- `%LOCALAPPDATA%\Programs\OrcaMCP\orca-mcp.exe`

Override with `ORCAMCP_APP_PATH` environment variable if needed.

---

## Dialog Suppression for Automation

MCP operations automatically suppress GUI dialogs that would block automation. Instead of showing modal dialogs, messages are captured and returned in the response.

### How It Works

When `load_model`, `load_project`, or `new_project` is called:
1. Dialog suppression is enabled
2. The operation is performed
3. Any dialogs that would have appeared are captured
4. Suppression is disabled
5. Messages are returned in `info_messages` array

### Response Format

```json
{
  "status": "success",
  "file": "C:\\Models\\old_project.3mf",
  "info_messages": [
    "Load 3MF: The 3MF file was generated by an old OrcaSlicer version, loading geometry data only."
  ],
  "active_warnings": {"count": 0, "warnings": []}
}
```

### Auto-Handled Dialogs

| Dialog Type | Default Action |
|-------------|----------------|
| Info dialogs (OK only) | Auto-OK, message captured |
| Yes/No dialogs (scaling) | Auto-YES (safer to scale) |
| Yes/No/Cancel (save changes) | Auto-NO (discard changes for new_project) |
| Warning dialogs | Auto-OK, message captured |
| 3MF version warnings | Auto-OK, message captured |
| Object too large/small | Auto-YES (scale to fit) |

### Implementation

Dialog suppression is implemented in:
- `GUI.hpp/cpp`: `set_mcp_dialog_suppression()`, `is_mcp_dialog_suppression_enabled()`
- `MsgDialog.cpp`: `ShowModal()` override checks suppression flag
- `OrcaMCPServer.cpp`: All critical endpoints enable suppression

### Endpoints with Dialog Suppression

| Category | Endpoints |
|----------|-----------|
| File Operations | `load_model`, `load_project`, `new_project`, `save_project`, `export_gcode`, `export_3mf` |
| Preset Management | `select_preset`, `apply_config`, `clone_preset`, `save_preset`, `delete_preset`, `reset_preset` |
| Slicing & Printing | `slice_all`, `send_to_printer` |

Error messages that would have been shown in dialogs are captured and returned in the response as `error_messages` (for failures) or `info_messages` (for non-critical information).

---

## Active Warnings

Most tool responses include an `active_warnings` section that exposes OrcaSlicer's notification system. This helps AI agents detect and respond to issues like G-code conflicts, missing supports, or slicing errors.

```json
{
  "status": "success",
  "active_warnings": {
    "count": 1,
    "warnings": [
      {
        "level": "serious_warning",
        "message": "Conflicts of G-code paths have been found...",
        "type": "GcodeOverlap"
      }
    ]
  }
}
```

**Warning levels:** `warning`, `serious_warning`, `error`

**Endpoints with active_warnings:** `get_scene_info`, `slice_all`, `get_slicing_status`, `get_print_estimate`, `load_model`, `arrange_objects`, `auto_orient`, all transform tools, `undo`, `redo`

The `count` field is always present (even when 0) to help confirm issues have been resolved.

---

## Environment Variables (Bridge Script)

| Variable | Default | Description |
|----------|---------|-------------|
| `ORCAMCP_HOST` | `localhost` | OrcaSlicer HTTP server host |
| `ORCAMCP_PORT` | `13618` | OrcaSlicer HTTP server port |
| `ORCAMCP_TIMEOUT` | `120` | Request timeout in seconds |
| `ORCAMCP_DEBUG` | (unset) | Enable debug logging to stderr |

---

## Upstream Compatibility Notes

This fork includes a fix for loading 3MF files from other slicers. The fix is in `src/libslic3r/PrintConfig.cpp` in `extend_extruder_variant()` - adds null checks for config options that may not exist in older 3MF files.

---

## Syncing with Upstream OrcaSlicer

This fork tracks `SoftFever/OrcaSlicer` as `upstream`. To incorporate upstream updates:

### Branch Strategy

```
upstream/main (OrcaSlicer)
      │
      ▼
    main  ──────► keeps in sync with OrcaSlicer
      │
      ▼
    mcp   ──────► MCP work + upstream updates (default branch)
```

### Sync Commands

```bash
# 1. Fetch latest from upstream
git fetch upstream

# 2. Update local main branch
git checkout main
git merge upstream/main

# 3. Merge upstream changes into mcp branch
git checkout mcp
git merge main
# (resolve any conflicts if needed)

# 4. Push updated branches
git push origin main
git push origin mcp
```

### One-Liner for Quick Sync

```bash
git fetch upstream && git checkout main && git merge upstream/main && git checkout mcp && git merge main && git push origin main mcp
```

### Alternative: Rebase (cleaner history)

```bash
git fetch upstream
git checkout mcp
git rebase upstream/main
git push origin mcp --force-with-lease
```

---

## Future Enhancements

Post-release features will be driven by user feedback. See GitHub Issues for current requests.

