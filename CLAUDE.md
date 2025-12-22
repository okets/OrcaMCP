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
                                                             │ - 44 MCP Tools  │
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

### MCP Tools (44 total)

| Category | Tools |
|----------|-------|
| **Scene** | `get_scene_info`, `new_project`, `load_project`, `save_project`, `export_3mf` |
| **Models** | `load_model`, `auto_orient`, `arrange_objects`, `get_object_info`, `rename_object` |
| **Transforms** | `move_object`, `rotate_object`, `scale_object`, `mirror_object`, `flatten_object`, `clone_object`, `cut_object`, `delete_object`, `transform_objects` |
| **Plates** | `add_plate`, `select_plate`, `delete_plate` |
| **Config** | `get_presets`, `get_edited_presets`, `select_preset`, `apply_config`, `get_valid_config_keys` |
| **Per-Object** | `get_object_config`, `set_object_config`, `reset_object_config` |
| **Layer Ranges** | `get_object_layer_ranges`, `set_object_layer_range`, `delete_object_layer_range` |
| **Slicing** | `slice_all`, `get_slicing_status`, `export_gcode`, `get_print_estimate` |
| **Visualization** | `render_plate_view` |
| **Printers** | `get_printers`, `select_printer`, `send_to_printer` |
| **Adaptive** | `apply_adaptive_layer_height`, `clear_adaptive_layer_height` |
| **History** | `undo`, `redo` |
| **Info** | `get_server_info` |

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
    "orcamcp": {
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
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` | 44 tool implementations (~3,700 lines) |
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

## Environment Variables (Bridge Script)

| Variable | Default | Description |
|----------|---------|-------------|
| `ORCAMCP_HOST` | `localhost` | OrcaSlicer HTTP server host |
| `ORCAMCP_PORT` | `13618` | OrcaSlicer HTTP server port |
| `ORCAMCP_TIMEOUT` | `120` | Request timeout in seconds |
| `ORCAMCP_DEBUG` | (unset) | Enable debug logging to stderr |

---

## Upstream Compatibility Notes

This fork includes a fix for loading 3MF files from other slicers (like JusPrin). The fix is in `src/libslic3r/PrintConfig.cpp` in `extend_extruder_variant()` - adds null checks for config options that may not exist in older 3MF files.

---

## Future Enhancements

- [ ] Direct file export without dialogs
- [ ] Slicing progress percentage
- [ ] WebSocket support for real-time notifications
- [ ] Batch operations for multiple files
- [ ] Calibration tools (flow rate, pressure advance)

---

*This project originated from JusPrin's MCP server implementation and was migrated to OrcaSlicer for broader compatibility.*
