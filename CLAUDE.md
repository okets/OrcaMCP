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

### Platform Build Commands

**Windows:**
```bash
cmake --build . --config %build_type% --target ALL_BUILD -- -m
```

**macOS:**
```bash
cmake --build build/arm64 --config RelWithDebInfo --target all --
```

**Linux:**
```bash
cmake --build build/arm64 --config RelWithDebInfo --target all --
```

### Build System
- Uses CMake with minimum version 3.13 (maximum 3.31.x on Windows)
- Primary build directory: `build/`
- Dependencies are built in `deps/build/`
- Windows builds use Visual Studio generators
- macOS builds use Xcode by default, Ninja with -x flag
- Linux builds use Ninja generator

### Testing
Tests are located in `tests/` using Catch2 framework:
- `tests/libslic3r/` - Core library tests
- `tests/fff_print/` - FFF slicing tests
- `tests/sla_print/` - SLA tests

```bash
cd build && ctest --output-on-failure
```

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

## Release Workflow

**CRITICAL: Always use `release.yml`, never `build_all.yml` for releases!**

### The One Rule

**To release: push a `v*` tag whose value exactly matches `SoftFever_VERSION` in `version.inc`.**

That's it. Do not click "Run workflow" on `build_all.yml`, `build_orca.yml`, or any sub-workflow. Tag-push triggers `release.yml` automatically, which calls the build pipeline and publishes a GitHub Release.

### Why version.inc and the tag MUST match

`release.yml` downloads artifacts by exact name (see `release.yml:91-107`):

- `OrcaMCP_Mac_universal_V<version>`
- `OrcaMCP_Linux_ubuntu_2404_V<version>`
- `OrcaMCP_Windows_V<version>`

The `<version>` portion is `tag` minus the `v` prefix. The build job names those artifacts by reading `SoftFever_VERSION` from `version.inc` (see `build_orca.yml:52`). If `version.inc` ≠ tag, the download step silently misses, and the release job fails after a ~1-hour build. This is what caused the failed `v2.3.2.14` and `v2.3.2.13` runs.

### Workflow Architecture

```
release.yml  (tag push v*, or manual)
  ├─ build_check_cache.yml × 4 (macOS arm64, macOS x86_64, Linux, Windows)
  │    └─ build_deps.yml         (only if deps cache miss)
  │         └─ build_orca.yml    (the actual app build, names artifacts from version.inc)
  ├─ build_orca.yml              (macOS Universal: combines arm64 + x86_64)
  └─ create_release job          (downloads named artifacts, publishes Release)

build_all.yml  (CI on push/PR/cron — NEVER for releases)
  └─ same build_check_cache.yml chain, but no Release publish
```

Active workflows you should never invoke for a release:
- `build_all.yml` — CI nightly + per-push builds
- `build_orca.yml`, `build_check_cache.yml`, `build_deps.yml` — `workflow_call` only, not direct dispatch

### Creating a New Release

1. **Bump version** in `version.inc`:
   ```bash
   # Edit version.inc and change SoftFever_VERSION
   # e.g., "2.3.2.10" -> "2.3.2.11"
   ```

2. **Commit and push** the version bump:
   ```bash
   git add version.inc
   git commit -m "Bump version to 2.3.2.11"
   git push origin mcp
   ```

3. **Create and push a tag** — value MUST match `SoftFever_VERSION` exactly, prefixed with `v`:
   ```bash
   git tag v2.3.2.11   # ← must match version.inc
   git push origin v2.3.2.11
   ```
   This automatically triggers `release.yml` which builds AND creates a GitHub Release.

### Alternative: Manual Release Trigger

```bash
gh workflow run release.yml -R okets/OrcaMCP -f tag=v2.3.2.11 -f draft=true
```

### Workflow Differences

| Workflow | Trigger | Creates Release? | Use Case |
|----------|---------|------------------|----------|
| `build_all.yml` | Push to main, PR, schedule, manual | No (artifacts only) | CI/testing |
| `release.yml` | Tag push `v*`, manual | Yes (with assets) | **Production releases** |

### Recovery: Replace Release Assets

If you ran `build_all.yml` instead of `release.yml`, you can recover by downloading artifacts and updating the release:

```bash
# 1. Download artifacts from the build run
gh run download <RUN_ID> -R okets/OrcaMCP

# 2. Delete old release assets
gh release delete-asset v2.3.2.10 OrcaMCP-v2.3.2.10-windows-x64-installer.exe -R okets/OrcaMCP

# 3. Upload new assets
gh release upload v2.3.2.10 ./path/to/new/artifact.exe -R okets/OrcaMCP
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

1. **Export paths**: `export_gcode`, `export_3mf` and `save_project` never open a file dialog (a modal would hang the MCP call); without a path they return an error / `cancelled`
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
| Warning dialogs | Auto-OK, message captured |
| 3MF version warnings | Auto-OK, message captured |
| Object too large/small | Auto-YES (scale to fit) |
| "Project has unsaved changes, save before continuing?" (Yes/No/Cancel, `Plater::close_with_confirm`) | Auto-NO: continue without saving (discard), message captured. Answering Yes would open a modal file dialog and hang the MCP call. |
| `UnsavedChangesDialog` (modified presets on new/load project, preset switch) | Discard the preset changes, message captured |
| `ProjectDropDialog` (project load behaviour, "load geometry only") | Load geometry only (`load_model` never replaces the current project; use `load_project` to open a 3MF as a project) |
| Native file dialogs (`wxFileDialog` via `Plater::priv::get_export_file`) | Never opened. `save_project` returns `cancelled`; `export_gcode` / `export_3mf` without `output_path` return an error asking for a path. |
| Archive contents picker (`FileArchiveDialog`, loading a .zip) | Not opened; the ZIP is not imported, message captured |
| `StepMeshDialog` (STEP/STP import tessellation) | Not opened; imported with the configured linear/angle deflection, message captured |
| Send-to-printer dialogs (`SelectMachineDialog`, print-host send) | Not suppressed: `send_to_printer` schedules them with `CallAfter` and returns `dialog_opened` immediately, so the user drives the dialog after the tool replies |

### Implementation

Dialog suppression is implemented in:
- `GUI.hpp/cpp`: `set_mcp_dialog_suppression()`, `is_mcp_dialog_suppression_enabled()`
- `MsgDialog.cpp`: `ShowModal()` override checks suppression flag
- `UnsavedChangesDialog.cpp`: `ShowModal()` discards preset changes under suppression
- `Plater.cpp`: `close_with_confirm()`, `determine_load_type()`, `priv::get_export_file()`, `preview_zip_archive()`, `mcp_skip_step_mesh_dialog()` check the flag before opening a modal
- `OrcaMCPServer.cpp` / `OrcaMCPPrinterTools.cpp`: endpoints scope suppression with the RAII
  `McpDialogSuppressionGuard` (`OrcaMCPCommon.hpp`), which is nest-safe and restores the previous
  state even if the handler throws. Never call `set_mcp_dialog_suppression()` directly.

Dialogs that are NOT `MsgDialog` subclasses (native `wxFileDialog`/`wxDirDialog`/`wxMessageBox`, and
`DPIDialog` subclasses such as `UnsavedChangesDialog`) bypass `MsgDialog::ShowModal`, so each one must
check `is_mcp_dialog_suppression_enabled()` at its call site. A modal opened inside `run_on_main_thread`
blocks the GUI thread forever and the MCP call never returns.

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
