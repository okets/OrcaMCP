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
                                                             │ - Tool registry │
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

### MCP Tools (79 in the app, 80 reachable)

The registry holds 80: the app serves 79 through `tools/list`, and `start_orca`, which launches
OrcaSlicer and so cannot be answered by it, is registered as bridge-only and served by the bridge.
Every tool's category is the row it sits in below. To regenerate the counts after adding a tool:

```bash
grep -hA1 -E '^\s*register_(bridge_)?tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'
```

| Category | Tools |
|----------|-------|
| **Scene** | `get_scene_info` (plates, objects with `filaments_used` — read that, not `extruder_id` — and each plate's full occupancy: object footprints with brim, the prime tower, excluded bed areas), `new_project`, `load_project`, `save_project`, `export_3mf` |
| **Models** | `load_model` (a 3MF is always geometry only: never its presets, never a rename; `.gcode` / `.gcode.3mf` only onto an empty scene, as a preview; returns `loaded_objects` in `get_scene_info`'s object shape, `filaments_added`; `multipart: merge\|separate`), `auto_orient`, `arrange_objects`, `get_object_info` (incl. every volume with its type and filament), `rename_object`, `set_object_printable` |
| **Transforms** | `move_object`, `rotate_object`, `scale_object`, `mirror_object`, `flatten_object`, `clone_object`, `cut_object`, `delete_object`, `transform_objects` |
| **Plates** | `add_plate`, `select_plate`, `delete_plate`, `set_prime_tower_position` |
| **Config** | `get_presets`, `get_edited_presets`, `select_preset`, `apply_config`, `clone_preset`, `save_preset`, `delete_preset`, `reset_preset`, `get_valid_config_keys` |
| **Per-Object** | `get_object_config`, `set_object_config`, `reset_object_config` |
| **Layer Ranges** | `get_object_layer_ranges`, `set_object_layer_range`, `delete_object_layer_range` |
| **Filaments & colour** | `get_filaments`, `set_object_filament` (whole-object form clears the volumes' own slots and reports `effective_filaments`; a volume's slot beats the object's), `set_mixed_filament`, `delete_mixed_filament`, `set_filament_color` (a slot's plate colour; `apply_config` edits the preset instead), `get_flush_volumes`, `set_flush_volumes`, `auto_calc_flush_volumes`, `get_toolchanger_config`, `suggest_color_mix`, `get_color_palette` |
| **Painting** | `paint_object`, `get_object_paint`, `clear_object_paint`, `set_brim_ears`, `get_object_components`, `pick_facet` |
| **Slicing** | `slice_all`, `get_slicing_status`, `export_gcode`, `get_print_estimate` |
| **Visualization** | `render_plate_view` (named cameras `iso/top/front/back/left/right/low`, fit to plate or object, default 3-view contact sheet, plate outline + 10 mm grid + origin + object labels, `objects_in_frame` / `uniform_image` metadata, `layer_view: first_layer` plan with brim and supports; coordinates are bed mm), `get_preview_base64`, `set_gcode_view_type` |
| **Printers** | `get_printers`, `select_printer`, `add_physical_printer` (incl. optional Obico URL/token for Flashforge), `discover_printers`, `send_to_printer`, `get_printer_status`, `printer_control`, `list_printer_files`, `print_printer_file`, `match_project_to_printer` |
| **Adaptive** | `apply_adaptive_layer_height`, `clear_adaptive_layer_height` |
| **History** | `undo`, `redo` |
| **Info** | `get_server_info` (every tool's summary by category; `section` fetches the rest of the docs), `quit_app` (closes the app with no dialog; discards unsaved changes unless `discard_changes=false`), `start_orca` (bridge-only) |

### Tool list: one source for every tool's text

Every tool's name, category, summary, description and schema lives in its registration in
`src/slic3r/GUI/OrcaMCP/`: `register_tool({...})`, or `register_bridge_tool({...})` for a tool the
bridge answers itself (`start_orca`). Nothing else holds tool text; everything agents see is
generated from that registry:

| Where agents see it | Generated by |
|---------------------|--------------|
| `tools/list` while the app runs | `OrcaMCPServer::handle_tools_list` (bridge-only tools left out) |
| `get_server_info`'s catalogue | `OrcaMCPServerInfo.cpp`, on every call |
| The bridge's list while the app is down, and its own tools' text either way | `scripts/orcamcp_tools.json`, the golden file |

Regenerate the golden file with the command in "Adding New Tools", step 4, below.

What the tests enforce, with no app running:

- `tests/slic3rutils/test_mcp_tool_list.cpp` (`[orcamcp][tools]`, run by CI's unit-test jobs on
  every platform):
  - a registration without a category does not compile, and a duplicate name or an app tool
    without a handler throws;
  - `tools/call` refuses a bridge-only or unknown tool with JSON-RPC error -32602;
  - every summary is one line of at most 40 characters;
  - the golden file equals the registry: any name, category, summary, description or schema
    that differs fails, naming the tool and the field;
  - `get_server_info`'s default response names every tool, bridge-only ones included, reports
    `SoftFever_VERSION`, stays under 6 KB, and its section index matches the sections;
  - every tool name `get_server_info` mentions, in structured fields or prose, is a real tool
    (`tests/slic3rutils/mcp_tool_references.hpp`, reusable for other text).
- `scripts/tests/` (`python3 -m unittest discover -s scripts/tests -t scripts`, run by the fork's
  `Python tests` workflow on pushes to `mcp`): the bridge's offline and online lists are identical
  in names, descriptions and order; `start_orca`'s text appears nowhere in the bridge; every
  bridge tool has a Python handler and every handler a tool; a missing or malformed file never
  stops the bridge, which then offers a fallback `start_orca` whose description names the file.
- CI: Build all also runs on a change to `scripts/orcamcp_tools.json` alone, since only its C++
  tests can compare the file with the registry.

Adding a bridge-only tool: a `register_bridge_tool({...})` in `OrcaMCPServer::register_bridge_tools()`
(same fields as any tool, no handler), its Python handler in `BRIDGE_HANDLERS` in
`scripts/orcamcp-bridge.py`, then regenerate the golden file.

---

## Documentation Index

For detailed documentation beyond this quick reference, see the `docs/` folder:

| Document | Description |
|----------|-------------|
| [Architecture Overview](docs/architecture/overview.md) | System design with detailed diagrams |
| [Threading Model](docs/architecture/threading-model.md) | GUI thread requirements and patterns |
| [Transport Layer](docs/architecture/transport-layer.md) | HTTP + stdio bridge design |
| [ADRs](docs/adr/) | Architecture Decision Records |
| [Tools Reference](docs/tools/reference.md) | Every tool, with parameters and examples |
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
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` | Most tool implementations; filament and printer tools live in OrcaMCPFilamentTools.cpp / OrcaMCPPrinterTools.cpp |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServerInfo.cpp` | `get_server_info`: the catalogue generated from the registry, and the documentation sections |
| `scripts/orcamcp_tools.json` | Golden tool list, generated from the registry; the bridge serves it offline (see "Tool list") |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.cpp` | Plate rendering (`render_plate_view`), turntable previews. Draws into its own framebuffer. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.cpp` | Pure render math: pixel projection, palette, camera presets, grid (unit-tested in `tests/slic3rutils/test_render_math.cpp`) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPRenderOverlay.cpp` | 2D overlays on finished renders: outline, grid, origin, labels, excluded areas |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPFirstLayerPlan.cpp` | Top-down first-layer plan from the sliced `Print` (brim, support, wipe tower) with footprint fallback |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp` | Preset/config management |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPModelLoad.cpp` | `load_model`'s decisions: what a file does to the scene (`load_file_kind`, `load_refusal`), the 3MF load type (`choose_3mf_load`, called by `Plater`'s `determine_load_type`) and the `loaded_objects` report, whose objects are `model_object_summary_json` (`OrcaMCPCommon.cpp`), shared with `get_scene_info` (unit-tested in `tests/slic3rutils/test_mcp_model_load.cpp`) |
| `src/slic3r/Utils/ObicoLink.cpp` | Flashforge preset's Obico link: page link object and token-free MCP status (spec `docs/superpowers/specs/2026-09-15-obico-camera-source-design.md`) |
| `src/slic3r/GUI/HttpServer.hpp` | HTTP server with JSON responses |
| `src/slic3r/GUI/HttpServer.cpp` | POST body reading, ResponseJson |
| `src/slic3r/GUI/GUI_App.cpp` | MCP route registration, HTTP server startup |
| `scripts/orcamcp-bridge.py` | stdio-to-HTTP bridge for Claude Code |

### Where the app's data lives

This fork's data directory is named after the fork, **not** after OrcaSlicer:

| Platform | Path |
|----------|------|
| macOS | `~/Library/Application Support/OrcaMCP/` |
| Windows | `%APPDATA%\OrcaMCP\` |
| Linux | `~/.config/OrcaMCP/` |

Physical printers (print host, serial, API key) live in `user/default/machine/<name>.json`
there — `C5P.json` for the Creator 5 Pro. Searching the `OrcaSlicer` directory instead finds
nothing and looks like the printer was never saved.

### Running the live printer test

`tests/slic3rutils/test_flashforge_live.cpp` talks to a real Flashforge over the LAN. It skips
itself unless `FF_HOST`, `FF_SERIAL` and `FF_CHECK_CODE` are set, and is excluded from CI by
its `[flashforge-live]` tag. The three values are the `print_host`,
`flashforge_serial_number` and `printhost_apikey` of the physical printer preset above.

**`FF_CHECK_CODE` is a printer access credential — never echo it, never commit it, and never
paste it into a conversation.** Read it out of the preset and hand it straight to the test.

The test is safe to run on an idle machine with an operator present: it toggles the chamber
light, sets one nozzle to 40 °C and the bed to 30 °C, then returns every target to 0. It
deliberately never starts, pauses or cancels a print.

```bash
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[flashforge-live]"
```

---

## Adding New Tools

### 1. Register the tool in `OrcaMCPServer.cpp`

Find `register_builtin_tools()` and add:

```cpp
register_tool({
    "my_new_tool",
    ToolCategory::Scene,                    // required: without it the registration does not compile
    "What it does, at most 40 characters",  // get_server_info's catalogue line
    "Description of what the tool does",    // tools/list
    {
        {"type", "object"},
        {"properties", {
            {"param1", {{"type", "string"}, {"description", "What param1 does"}}},
            {"param2", {{"type", "integer"}, {"description", "What param2 does"}}}
        }},
        {"required", {"param1"}}
    },
    [](const nlohmann::json& params) -> nlohmann::json {
        return handle_my_new_tool(params);
    }
});
```

The category is one of CLAUDE.md's tool-table rows (`OrcaMCPServer::ToolCategory`). A second tool
with the same name, or one without a handler, throws when the registry is built.

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

### 4. Regenerate the golden tools file

`scripts/orcamcp_tools.json` is the tool list the bridge serves while the app is down, and it
must match the registry or `[orcamcp][tools]` fails. After adding a tool or changing any tool's
text or schema:

```bash
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests
ORCAMCP_UPDATE_TOOLS_GOLDEN=1 build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][tools]"
```

Commit the regenerated file with the change.

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

Every captured prompt that offered a choice (Yes, No or Cancel buttons) ends with the answer it was
given: `"<prompt> (auto-answered <answer>)"`. OK-only notices are captured as they are.

| Dialog Type | Default Action |
|-------------|----------------|
| Info dialogs (OK only) | Auto-OK, message captured |
| Yes/No dialogs (`MsgDialog`) | Auto-YES, `(auto-answered Yes)` |
| Warning dialogs | Auto-OK, message captured |
| 3MF version warnings | Auto-OK, message captured |
| Object too large/small | Auto-YES (scale to fit). `load_model`'s `loaded_objects` shows the resulting scale |
| "Several objects at multiple heights: load as a single object with multiple parts?" (`Plater.cpp`, `MCP_PROMPT_MULTIPART`) | Answered by `load_model`'s `multipart`: `merge` (default) = Yes, `separate` = No. The message names the other value: `(auto-answered Yes; pass multipart: "separate" to keep them as separate objects)` |
| "Project has unsaved changes, save before continuing?" (Yes/No/Cancel, `Plater::close_with_confirm`) | Auto-NO: continue without saving (discard), `(auto-answered No: continued without saving)`. Answering Yes would open a modal file dialog and hang the MCP call. |
| `UnsavedChangesDialog` (modified presets on new/load project, preset switch) | Discard the preset changes; the message names up to eight changed keys, `(auto-answered Discard: the changes were lost)` |
| `ProjectDropDialog` (project load behaviour) | Never shown under MCP, whatever `project_load_behaviour` says and whether the scene is empty: `load_model` always imports a 3MF as geometry only (no presets applied, no scene reset, no rename; decided by `OrcaMCP::choose_3mf_load`). `load_project` opens a 3MF as a project. |
| Native file dialogs (`wxFileDialog` via `Plater::priv::get_export_file`) | Never opened (`auto-answered Cancel`). `save_project` without a name returns an error asking for `output_path`; `export_gcode` / `export_3mf` without `output_path` return an error asking for a path. |
| Archive contents picker (`FileArchiveDialog`, loading a .zip) | Not opened; the ZIP is not imported (`auto-answered Cancel`), and `load_model` returns an error |
| `StepMeshDialog` (STEP/STP import tessellation) | Not opened; imported with the configured linear/angle deflection, which the message states |
| `TextureImportDialog` (textured or vertex-coloured OBJ, GLB, GLTF, FBX) | Not opened; imported as plain geometry, colours not mapped (`auto-answered Skip`) |
| "Connected printer is X. Sync the printer information and switch the preset?" (`TipsDialog`, project load with a mismatched Bambu printer connected) | Auto-NO: the printer preset is not switched |
| Any other `DPIDialog` modal (the fallback in `DPIAware::ShowModal`, `GUI_Utils.hpp`) | Not opened: answers Cancel, `"<dialog title> was suppressed (auto-answered Cancel)"`. The rows above answer their dialogs first, so this only catches a modal nobody handled |
| Send-to-printer (`send_to_printer`) | **Bambu:** the `SelectMachineDialog` is scheduled with `CallAfter` and the tool returns `dialog_opened`; the user drives it. **Print hosts (Flashforge, Moonraker, OctoPrint, …):** by default (`direct: true`) there is **no dialog** — the tool uploads the sliced plate and, because `start_print` also defaults to true, **starts the print**. It returns `queued`. Pass `start_print: false` to upload only, or `direct: false` to open the print-host dialog instead. Never call it to "look at the dialog": on 2026-09-18 that started a 7 h print. |

### Implementation

Dialog suppression is implemented in:
- `GUI.hpp/cpp`: `set_mcp_dialog_suppression()`, `is_mcp_dialog_suppression_enabled()`,
  `add_mcp_suppressed_answer()` (the `(auto-answered …)` format every site uses), and the per-prompt
  answers (`set_mcp_prompt_answer()`, `mcp_answer_for()`)
- `MsgDialog.cpp`: `ShowModal()` override checks suppression flag; a dialog tagged with
  `set_mcp_prompt_key()` takes the answer the tool set with `McpDialogSuppressionGuard::answer_prompt()`
- `UnsavedChangesDialog.cpp`: `ShowModal()` discards preset changes under suppression
- `Plater.cpp`: `close_with_confirm()`, `determine_load_type()`, `priv::get_export_file()`, `preview_zip_archive()`, `mcp_skip_step_mesh_dialog()`, `priv::run_textured_mesh_import_dialog()` and the sync-printer `TipsDialog` in `priv::load_files()` check the flag before opening a modal
- `OrcaMCPServer.cpp` / `OrcaMCPPrinterTools.cpp`: endpoints scope suppression with the RAII
  `McpDialogSuppressionGuard` (`OrcaMCPCommon.hpp`), which is nest-safe and restores the previous
  state even if the handler throws. Never call `set_mcp_dialog_suppression()` directly.

A modal opened inside `run_on_main_thread` blocks the GUI thread forever and the MCP call never
returns. `DPIDialog` subclasses that no handler answers fall back to Cancel in `DPIAware::ShowModal`
(`mcp_skip_unhandled_modal()`, `GUI.cpp`); give a dialog its own handler when Cancel is the wrong
answer or the agent needs more than the title. Dialogs that derive from `wxDialog` directly (21 classes,
e.g. `FilamentMapDialog`) and native ones (`wxFileDialog`/`wxDirDialog`/`wxMessageBox`) bypass both
`MsgDialog::ShowModal` and the fallback, so each must check `is_mcp_dialog_suppression_enabled()` at
its call site.

### Naming the Project ("export_3mf is this API's Save")

`export_3mf`, `save_project` and `load_project` all call `Plater::set_project_filename`, because a
nameless project cannot be saved from MCP at all (naming it needs a file dialog). Naming it is not
cosmetic: it retitles the window, adds the path to Recent Projects, and makes both `save_project`
and a Cmd-S in the GUI overwrite that file.

So the API's verbs map to the GUI's like this:

| MCP tool | GUI equivalent |
|----------|----------------|
| `export_3mf` with `output_path` | Save As (writes the file **and** names the project) |
| `save_project` with no `output_path` | Save (in place; error if the project has no name yet) |
| `save_project` with `output_path` | Save As |
| `load_project` | Open (the project takes the opened file's name) |
| `load_model` of a model file (a 3MF included) | Import (never names the project) |
| `load_model` of a `.gcode` or `.gcode.3mf`, onto an empty scene | Open as a G-code preview (the project takes the file's name); refused onto a scene with objects |

Each of those responses carries `"project_renamed_to": <path>` and an `info_messages` line whenever
the call changed the project's name, so an agent never has to guess which file a later
`save_project` will overwrite. Until 2026-09-26 a `load_model` of a 3MF onto an empty scene opened
it as a project and renamed it silently, and a later `save_project {}` overwrote the user's file.

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
| `ORCAMCP_SKIP_CLOUD_LOGIN` | (set by `start_orca`) | App-side: marks an agent launch (`GUI::is_agent_launch()`), so startup waits on nothing a person must answer. It skips the Orca cloud silent sign-in, which reads the keychain synchronously on the GUI thread (on macOS a permission prompt per freshly built binary), and the recent-project thumbnails, which open every recent 3MF on the GUI thread (for projects in `~/Documents`, a macOS privacy prompt per fresh binary). Home then shows the projects listed before the launch without thumbnails; projects saved or opened during the session get theirs. Either prompt, unanswered, blocks the app before the MCP server starts. Set it yourself when launching the app for an agent. |

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

### Carried upstream fixes — re-check at every major upstream release

Decided 2026-09-18: this fork does **not** send its fixes upstream (judged not worth the effort).
We carry them, and drop each one the moment upstream fixes it, so the conflict set shrinks anyway.
At every major upstream release, as part of the sync, run these probes against `upstream/main`.
A non-zero / "yes" means upstream still has the bug: keep our patch. A zero / "no" means upstream
fixed it: take upstream's version in the merge and re-verify ours is gone. Add a probe whenever a
new fork-only fix lands in an upstream file.

```bash
U(){ git show "upstream/main:$1"; }
echo "A worker thread calls show_error_info directly (4d76a06287):     $(U src/slic3r/GUI/Jobs/BoostThreadWorker.hpp | grep -c 'show_error_info')"
echo "B ~GLCanvas3D calls reset_volumes (f599bda795):                  $(U src/slic3r/GUI/GLCanvas3D.cpp | awk '/^GLCanvas3D::~GLCanvas3D/{f=1} f&&/reset_volumes/{print "yes"; exit} f&&/^}/{print "no"; exit}')"
echo "C unguarded result_polygon[0] (f599bda795):                      $(U src/libslic3r/PrintConfig.cpp | grep -c 'result = result_polygon\[0\]')"
echo "D Moonraker payload lacks sdcard key (91efc63d30; 0 = bug):      $(U src/slic3r/Utils/MoonrakerPrinterAgent.cpp | grep -c '\"sdcard\"')"
echo "E display name falls through to Unknown (041db47482):           $(U src/slic3r/GUI/DeviceManager.cpp | awk '/get_printer_type_display_str/{f=1} f&&/_L\("Unknown"\)/{print "yes"; exit}')"
echo "F error panel: Wrap( without Layout() (f5ff97bfa1):             $(U src/slic3r/GUI/SelectMachine.cpp | grep -c 'Wrap(') Wrap / $(U src/slic3r/GUI/SelectMachine.cpp | grep -A3 'Wrap(' | grep -c 'Layout()') Layout"
echo "G dead [this] capture CameraPopup (merge 476df4364e):            $(U src/slic3r/GUI/CameraPopup.cpp | grep -c 'Bind(wxEVT_TOGGLEBUTTON, \[this\](wxCommandEvent &e)')"
echo "H dead [this] capture StatusPanel (merge 476df4364e):            $(U src/slic3r/GUI/StatusPanel.cpp | grep -c 'm_bmToggleBtn_timelapse->Bind(wxEVT_TOGGLEBUTTON, \[this\]')"
echo "I extruder-count mismatch in the Send dialog (reported upstream): $(gh issue view 15758 -R OrcaSlicer/OrcaSlicer --json state -q .state 2>/dev/null || echo unknown)"
echo "J startup reads recent-project thumbnails on the GUI thread (rel2506/02): $(U src/slic3r/GUI/MainFrame.cpp | awk '/FileHistory::LoadThumbnails\(\)$/{f=1} f&&/parallel_for/{print "yes"; exit} f&&/^}/{print "no"; exit}')"
```

Item J: upstream opens every recent 3MF synchronously while building the main window, before
post_init starts the MCP server. Our patch skips it for an agent launch (`GUI::is_agent_launch()`,
`MainFrame.cpp`). "no" means upstream moved the load off the GUI thread: re-check whether our skip
is still needed.

Item I is not a fork patch -- we deliberately carry nothing for it (see
`docs/superpowers/plans/2026-09-17-next-release-plan.md`, Stage 3). It is here so the sync notices
when <https://github.com/OrcaSlicer/OrcaSlicer/issues/15758> closes.

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
