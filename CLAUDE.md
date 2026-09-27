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

### MCP Tools (82 in the app, 84 reachable)

The registry holds 84: the app serves 82 through `tools/list`, and two are registered as
bridge-only and served by the bridge: `start_orca`, which launches OrcaSlicer and so cannot be
answered by it, and `wait_for_slice`, a wait the app's one request thread could not do without
stalling every other call.
Every tool's category is the row it sits in below. To regenerate the counts after adding a tool:

```bash
grep -hA1 -E '^\s*register_(bridge_)?tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'
```

| Category | Tools |
|----------|-------|
| **Scene** | `get_scene_info` (plates, objects with `filaments_used` — read that, not `extruder_id` — and `mesh_warning` (the object list's warning icon, with its reason; `with_model_object_features` adds the mesh-health numbers), and each plate's full occupancy: object footprints with brim, the prime tower, excluded bed areas; `open_dialogs` / `system_dialog_open` / `untracked_modal_loop`: a dialog waiting for the user), `new_project`, `load_project` (both cancel a running slice; refused while the startup restore prompt waits), `save_project`, `export_3mf` |
| **Models** | `load_model` (a 3MF is always geometry only: never its presets, never a rename; `.gcode` / `.gcode.3mf` only onto an empty scene, as a preview; returns `loaded_objects` in `get_scene_info`'s object shape, `filaments_added`; `multipart: merge\|separate`), `auto_orient`, `arrange_objects`, `get_object_info` (incl. every volume with its type and filament), `get_mesh_health` (mesh errors behind the object list's warning icon: the icon state, its exact tooltip, open edges, recorded repairs, shells, per object and per volume), `rename_object`, `set_object_printable` |
| **Transforms** | `move_object`, `rotate_object` (a change in degrees; `relative: false` is refused), `scale_object`, `mirror_object`, `flatten_object` (the named object only, which replaces the selection, as the GUI's Orient does for a selection; an object with an instance on a locked plate is refused), `clone_object`, `cut_object`, `delete_object`, `transform_objects` (rotate, scale, mirror and transform drop a resting object back onto the bed like the GUI; an explicit Z is kept) |
| **Plates** | `add_plate`, `select_plate`, `delete_plate`, `set_prime_tower_position` |
| **Config** | `get_presets`, `get_edited_presets` (25-48 KB), `get_config_values` (no arguments: the selected printer, print and per-slot filament presets with dirty flags, ~400 B; `keys`: just those settings, grouped by `apply_config` type, with `dirty` saved values; `dirty_only`), `select_preset` (`type: printer` returns the resulting `filaments`, each with its observed `color_source`: `unchanged`/`remembered`/`default`/`other`), `apply_config`, `clone_preset`, `save_preset`, `delete_preset`, `reset_preset`, `get_valid_config_keys` |
| **Per-Object** | `get_object_config`, `set_object_config`, `reset_object_config` |
| **Layer Ranges** | `get_object_layer_ranges`, `set_object_layer_range`, `delete_object_layer_range` |
| **Filaments & colour** | `get_filaments`, `set_object_filament` (whole-object form clears the volumes' own slots and reports `effective_filaments`; a volume's slot beats the object's), `set_mixed_filament`, `delete_mixed_filament`, `set_filament_color` (a slot's plate colour, saved for the selected printer so a switch away and back keeps it; `apply_config` edits the preset instead), `get_flush_volumes`, `set_flush_volumes`, `auto_calc_flush_volumes`, `get_toolchanger_config`, `suggest_color_mix`, `get_color_palette` |
| **Painting** | `paint_object` (`selection: state` repaints every facet now in `match_filament` / `match_state`), `remap_paint` (renumbers painted filaments at once, `{"1": 2, "2": 3}`, never chained; one undo step; `facets_before` / `facets_after`; `notes` names the `set_object_filament` call for unpainted facets on a moved base filament), `get_object_paint`, `clear_object_paint`, `set_brim_ears`, `get_object_components`, `pick_facet` |
| **Slicing** | `slice_all` (`status`: slicing_started, or not_started with a `reason`: `busy_slicing` — the pipeline is busy (slicing, exporting, uploading or stopping), nothing is started, call `wait_for_slice` then `slice_all` again — or already_sliced / busy_job / invalid (the app refuses the plate as it stands: its validation, with its message, an object partly off the plate, a filament check, missing plugins, a broken mixed filament, or a failed last slice; `message` says which) / nothing_to_slice (nothing printable on the plates -- partly off one or too tall -- and no refusal the app gives words for, which comes first) / unknown), `wait_for_slice` (bridge-only: polls `get_slicing_status` until the run is over, capped 15 s below `ORCAMCP_TIMEOUT`, or a quarter below it under 60 s; `outcome` done / ended_early / incomplete / not_slicing / timed_out / app_gone), `get_slicing_status` (`busy` / `busy_reason`: the one busy test slice_all and wait_for_slice share; per-plate `percent`, `stage` text, and `slice_run.outcome` for the last `slice_all` run: running / done / ended_early / incomplete, with `skipped` empty plates, which never keep a run from `done`; `state` follows that run; per-plate `gcode_check`: the check the slice ran on its own G-code, `{ok: true}` or `problems` codes and a `message`; `above_printable_height` adds `highest_layer_z_mm`, `printable_height_mm` and a `hint`), `export_gcode` (the selected plate, sliced; refused when its `gcode_check` failed, as the GUI's Export button is off), `get_print_estimate` (`time_by_feature`: seconds per feature, walls split, travel, tool changes, other and unattributed, summing to the total; `printed_layers` = the G-code's layer count; `object_layers` / `support_layers` split; `layer_count` deprecated, now the printed count too) |
| **Visualization** | `render_plate_view` (named cameras `iso/top/front/back/left/right/low`, fit to plate or object, default 3-view contact sheet, plate outline + 10 mm grid + origin + object labels, `objects_in_frame` / `uniform_image` metadata, `layer_view: first_layer` plan with brim, supports and rafts, and `layer_view: {layer}` / `{z}` for any sliced layer from the G-code, filtered by `features` / `filaments`, `color_by` feature or filament, with its height, filaments, extruded areas and per-object `objects_at_height` (object and support layer, overhang coverage); coordinates are bed mm; drawn from the 3D view whatever tab shows; images in the system temp directory), `get_preview_base64`, `set_gcode_view_type` |
| **Printers** | `get_printers` (`is_online` is not a live check; `current_print_host.last_status_age_s` is), `select_printer`, `add_physical_printer` (incl. optional Obico URL/token for Flashforge), `discover_printers`, `send_to_printer` (refused when a plate it would send failed its `gcode_check`), `get_printer_status` (a failure names host:port and the next step, with the last known material station as `cached`), `printer_control`, `list_printer_files`, `print_printer_file`, `match_project_to_printer` (falls back to the printer's last status, applied only with `allow_cached: true`) |
| **Adaptive** | `apply_adaptive_layer_height`, `clear_adaptive_layer_height` |
| **History** | `undo`, `redo` |
| **Info** | `get_server_info` (every tool's summary by category; `section` fetches the rest of the docs), `quit_app` (closes the app with no dialog; discards unsaved changes and closes an open dialog unanswered unless `discard_changes=false`; refuses while a system file chooser or alert is open), `start_orca` (bridge-only) |

### Tool list: one source for every tool's text

Every tool's name, category, summary, description and schema lives in its registration in
`src/slic3r/GUI/OrcaMCP/`: `register_tool({...})`, or `register_bridge_tool({...})` for a tool the
bridge answers itself (`start_orca`, `wait_for_slice`). Nothing else holds tool text; everything agents see is
generated from that registry:

| Where agents see it | Generated by |
|---------------------|--------------|
| `tools/list` while the app runs | `OrcaMCPServer::handle_tools_list` (bridge-only tools left out) |
| `get_server_info`'s catalogue | `OrcaMCPServerInfo.cpp`, on every call |
| The bridge's list while the app is down, and its own tools' text either way | `scripts/orcamcp_tools.json`, the golden file |

Regenerate the golden file with the command in "Adding New Tools", step 4, below.

The schema is also what a call is held to. Before the handler runs, `tools/call` refuses with
JSON-RPC **-32602** (`OrcaMCP::tool_arguments_error`, `OrcaMCPToolArguments.cpp`, against the schema
as `tools/list` publishes it):

- an argument the schema does not declare (every top-level schema says `additionalProperties: false`;
  `tools/list` adds it where a registration leaves it out), and a key of a nested object whose own
  schema says `additionalProperties: false` (`set_object_config`'s items, `set_object_layer_range`'s
  settings, `transform_objects`' entries and their `position`/`rotation`/`scale`,
  `apply_config.settings[]`, `set_brim_ears.points[]`, `printer_control.nozzles[]`, both
  `material_mappings[]`, `paint_object`'s `box`/`sphere`, `pick_facet.ray`);
- a required argument, or a nested object's required key, left out;
- `arguments` that is not an object (absent and `null` both mean no arguments).

The message names the tool, what is wrong, and what the object takes:
`scale_object has no argument "scale". Its arguments: object_id, include_preview, preview_resolution, preview_views, uniform, x, y, z.`,
`set_object_config: settings[0] has no key "unit". Its keys: key, value.` No type, range or enum is
checked; that stays the handler's job, and a nested value is looked into only when it is what its
schema describes (a settings list sent as its JSON text passes as it is). So a handler may read a
required argument directly, and every argument it reads must be declared: any other is refused
before the handler sees it. The bridge holds its own two tools to their schemas in the golden file
the same way, with the same words (`argument_error`, top level only: neither takes a nested object).

**A value a tool would act on as something else is refused, not reinterpreted.** Past the schema,
a handler that cannot do what a value asks answers with its own error (`{"status": "error",
"message"}` naming what to send instead), before it changes anything or takes an undo step:
`rotate_object relative: false`, one bound of `delete_object_layer_range`, an empty or malformed
`reset_object_config keys`, `clone_object count < 1`, an unknown `cut_object keep` or
`get_valid_config_keys category`, `scale_object uniform` without `x`, and `printer_control
set_temperature` with nothing to set (`tests/slic3rutils/test_mcp_argument_values.cpp`,
`[McpArgumentValues]`); and a `flatten_object` the orient job would not scope to the object
(`flatten_refusal`, `flatten_selection_refusal`; `tests/slic3rutils/test_transform_frames.cpp`,
`[orcamcp][transform_frames]`). A call that asks for no change stays a success with `changed: false`
or a zero count: a transform with no axes, `apply_config` / `set_object_config` with no settings, the
adaptive tools with no `object_ids`. An empty list that would be read as something else is refused
instead: `reset_object_config`'s `keys: []` (which meant every override) and `set_temperature`'s
`nozzles: []` (a command with nothing in it).

**Anything a tool returns in the shape a request takes must be accepted back**: an agent edits a
list by sending back the one a response gave it. So a strict nested object declares, as accepted and
ignored, the extra fields its response twin carries: `set_brim_ears.points[]` takes `z` (the
`brim_ears` of `set_brim_ears` and `get_object_paint`), both `material_mappings[]` take
`color_delta_e` (what a send reports). Adding `additionalProperties: false` to a nested object, or a
field to a response in a request's shape, means checking the other side, with a test that feeds the
response entry back through `tool_arguments_error` ("What a tool returns, sent back" in
`test_mcp_tool_arguments.cpp`).

What the tests enforce, with no app running:

- `tests/slic3rutils/test_mcp_tool_list.cpp` (`[orcamcp][tools]`, run by CI's unit-test jobs on
  every platform):
  - a registration without a category does not compile, and a duplicate name or an app tool
    without a handler throws;
  - `tools/call` refuses a bridge-only or unknown tool with JSON-RPC error -32602;
  - `tests/slic3rutils/test_mcp_tool_arguments.cpp` (`[McpToolArguments][orcamcp][tools]`): every
    app tool refuses an argument it does not take, and each required argument left out, with -32602
    naming it, before its handler runs; the nested cases above; arguments that are not an object;
    every required name is a declared property; each response entry in a request's shape is taken
    back; the walk's depth is the schema's, not the value's;
  - every summary is one line of at most 40 characters;
  - the golden file equals the registry: any name, category, summary, description or schema
    that differs fails, naming the tool and the field;
  - `get_server_info`'s default response names every tool, bridge-only ones included, reports
    `SoftFever_VERSION`, stays under 6 KB, and its section index matches the sections;
  - every tool name `get_server_info` mentions, in structured fields or prose, is a real tool
    (`tests/slic3rutils/mcp_tool_references.hpp`, reusable for other text).
- `scripts/tests/` (`python3 -m unittest discover -s scripts/tests -t scripts`, run by the fork's
  `Python tests` workflow on pushes to `mcp`): the bridge's offline and online lists are identical
  in names, descriptions and order; no bridge tool's text appears in the bridge; every
  bridge tool has a Python handler and every handler a tool; a missing or malformed file never
  stops the bridge, which then offers a fallback `start_orca` whose description names the file;
  `start_orca` and `wait_for_slice` refuse an argument their schema in the file does not declare,
  in the app's words, and no bridge tool takes a nested object; the Windows path rewrite forwards
  arguments that are not an object untouched, for the app to refuse (`test_bridge_arguments.py`).
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
| `src/slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.cpp` | Pure render math: pixel projection, palette, camera presets and fit (`frame_camera`), plate-view filter and blank-image hint, grid (unit-tested in `tests/slic3rutils/test_render_math.cpp`) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPImageFiles.cpp` | Where render and preview images are written (the system temp directory), which files are ours, and what `get_preview_base64` may read (unit-tested in `tests/slic3rutils/test_mcp_image_files.cpp`) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPRenderOverlay.cpp` | 2D overlays on finished renders: outline, grid, origin, labels, excluded areas |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPFirstLayerPlan.cpp` | Top-down first-layer plan from the sliced `Print` (what prints at the lowest height: bodies or rafts, brim, support, wipe tower) with footprint fallback; `paint_plan`, the canvas every top-down plan draws on |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPGcodeCheck.cpp` | The check a slice runs on its own G-code, read as the GUI's Print/Export buttons read it (`PartPlate::is_slice_result_ready_for_print`): `gcode_check_problems` decodes `gcode_check_result.error_code`, `toolpath_outside` and the bed-surface conflict into named problems; `get_slicing_status`'s `gcode_check` and the refusals of `export_gcode` and `send_to_printer` come from it (unit-tested in `tests/slic3rutils/test_gcode_check.cpp`) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPLayerPlan.cpp` | The sliced layer plan (`layer_view: {layer}` / `{z}`): layers of the G-code result (`gcode_layers`, by binary search on `layer_id`), height resolution, feature and filament filters, extruded areas, the object and support layers at a height and their overhang, legend and drawing (unit-tested in `tests/slic3rutils/test_layer_plan.cpp`, on synthetic moves and a real slice) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPInstanceBox.cpp` | Cached exact box of one instance (`instance_box`), which every per-plate description reads: `ModelObject::instance_bounding_box` walks every vertex on every call |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp` | Preset/config management |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPExtrusionFeatures.hpp` | The one table grouping extrusion roles into the features MCP tools report (`perimeters`, `infill`, `support`, `support_interface`, `brim`, `skirt`, `prime_tower`, `other`), plus the wall split; shared by `get_print_estimate`'s `time_by_feature` and the layer plan |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPModelLoad.cpp` | `load_model`'s decisions: what a file does to the scene (`load_file_kind`, `load_refusal`), the 3MF load type (`choose_3mf_load`, called by `Plater`'s `determine_load_type`) and the `loaded_objects` report, whose objects are `model_object_summary_json` (`OrcaMCPCommon.cpp`), shared with `get_scene_info` (unit-tested in `tests/slic3rutils/test_mcp_model_load.cpp`) |
| `src/slic3r/Utils/ObicoLink.cpp` | Flashforge preset's Obico link: page link object and token-free MCP status (spec `docs/superpowers/specs/2026-09-15-obico-camera-source-design.md`) |
| `src/slic3r/GUI/HttpServer.hpp` | HTTP server with JSON responses; listens on 127.0.0.1 only |
| `src/slic3r/GUI/HttpServer.cpp` | POST body reading, ResponseJson, the stop that waits for handlers and lets replies out |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshHealth.cpp` | Mesh health as the object list reports it: the warning icon and its exact tooltip through the list's own `mesh_errors_info` (`GUI_ObjectList.cpp`), the numbers behind them, and `get_mesh_health`'s shell lists (unit-tested in `tests/slic3rutils/test_mesh_health.cpp`); the tool itself is `OrcaMCPMeshTools.cpp` |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp` | How a call hands work to the main thread and waits, and how quitting releases it: the gate is `QueuedCalls` (`src/slic3r/Utils/QueuedCall.hpp`), with `call_through` for a tool's json (see "Threading Model"; unit-tested in `tests/slic3rutils/test_mcp_shutdown.cpp`, `test_queued_call.cpp`) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPQuit.cpp` | Quitting while a modal dialog is open: which dialogs are open, ending the innermost unanswered, holding the close until they are gone, and `quit_app`'s refusals (unit-tested in `tests/slic3rutils/test_mcp_quit.cpp`); the wx side (modal hook, turn timer) is `OrcaMCPQuitApp.cpp` |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPLoginServer.cpp` | Where the cloud login's callback is answered: a second port of the MCP server, on its thread (unit-tested in `tests/slic3rutils/test_http_server.cpp`) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPToolArguments.cpp` | Which tool calls reach a handler: an argument the tool's schema does not declare, a required one left out, or arguments that are not an object are refused with -32602 (see "Tool list"; unit-tested in `tests/slic3rutils/test_mcp_tool_arguments.cpp`) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPRequestGuard.cpp` | Which requests the server answers: no web page's and no DNS-rebound one on `/mcp`, login callbacks only where a login listens (see "Security"; unit-tested in `tests/slic3rutils/test_mcp_request_guard.cpp`) |
| `src/slic3r/Utils/ThreadCancel.cpp` | The per-request cancel check a quit applies to blocking network calls on the HTTP thread (unit-tested in `tests/slic3rutils/test_thread_cancel.cpp`) |
| `src/slic3r/GUI/GUI_App.cpp` | MCP route registration, HTTP server startup, and the shutdown order (`stop_http_server`) |
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

Declare every argument the handler reads: `tools/call` refuses any other, and a `required` one left
out, before the handler runs (see "Tool list"). A nested object is held to its keys only when its
schema says `additionalProperties: false`; add it wherever the handler reads nothing else, and accept
back whatever a response returns in that object's shape.

### 2. Implement the handler

```cpp
json OrcaMCPServer::handle_my_new_tool(const json& params) {
    return run_on_main_thread([&]() -> nlohmann::json {
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

The HTTP server runs **one** thread, and it calls the request handler itself, so calls are served
one at a time. All tool handlers use `run_on_main_thread()` which:
- Blocks the HTTP thread until the GUI operation completes
- Required for OpenGL rendering and wxWidgets operations
- Goes through the app's `MainThreadGate` (`OrcaMCPMainThreadGate.hpp`), which queues the work with
  `wxGetApp().CallAfter()` and waits for it. The gate is `QueuedCalls` (`src/slic3r/Utils/QueuedCall.hpp`):
  work that started is waited for, a caller released before its work started never has it run, what
  the work throws is rethrown. The printer agents' bounded GUI-thread calls use it too
  (`run_queued_and_wait`)

```cpp
template<typename Func>
nlohmann::json run_on_main_thread(Func&& func);  // throws McpShuttingDown once the app is quitting
```

### Shutdown: never wait on something that waits on the main thread

Quitting joins the HTTP thread from the main thread (`GUI_App::stop_http_server` ->
`HttpServer::stop`). A call waiting in `run_on_main_thread` waits for the main thread, so a join with
that call in flight waited forever: on 2026-09-26 `quit_app`, with a script polling
`get_slicing_status`, left the app hung for 15 minutes. The rules that prevent it:

- **One signal.** The main frame's close handler calls `OrcaMCPServer::shut_down()` right where it
  sets `set_closing(true)`, once the close can no longer be vetoed (`MainFrame.cpp`). That closes the
  gate: the waiting call is released with `McpShuttingDown`, work still queued is never run, and
  every later tool call is refused with JSON-RPC **-32002** ("OrcaMCP is quitting, so this call was
  not run. Use start_orca to start it again."). `McpShuttingDown` is a `JsonRpcError`
  (`OrcaMCPJsonRpcError.hpp`), so it takes the one `JsonRpcError` path. A call whose work has already
  started is waited for, since its work may still use what the caller owns.
- **Never abandon a handler, and still bound the quit.** A handler still running may use what the app
  destroys next, so no handler on the server's thread may block past the quit. Waits on the main
  thread are released by the gate, and every blocking call made for a request gives up once the gate
  is closed: the request's `ScopedThreadCancelCheck` (`src/slic3r/Utils/ThreadCancel.hpp`, installed in
  `GUI_App`'s route) aborts synchronous `Http` transfers within about a second, stops
  `discover_printers`, and ends a `TCPConsole` exchange (legacy Flashforge, MKS) within 100 ms. A new
  blocking call on that thread must honour `this_thread_cancelled()` or go through `Http`/`TCPConsole`.
  What cannot be cancelled -- the Bambu network plugin's calls inside a login callback -- is bounded:
  `GUI_App::stop_http_server` gives `HttpServer::stop` 5 s, and when a request still holds the thread
  then, the server is left alone and the process ends at once (`end_process_under_running_server_thread`:
  config saved, logs flushed, `std::_Exit(0)`), without the teardown the stuck call could be using.
  MainFrame::shutdown has saved the app config before that point; nothing after it saves user data.
- **Replies get out.** `HttpServer::stop` closes the listeners and idle connections at once, but lets a
  reply that is being written finish, for up to 2 s, so the caller released by the quit reads its
  -32002 rather than a reset.
- **A close inside a tool call's work** (the work pumped the event loop into it) is deferred: the main
  frame's close handler asks `OrcaMCPServer::defer_until_tool_call_returns` and, while the work runs,
  vetoes or returns, to close again once the work has returned. The plater is never reset, nor the
  frame torn down, under a running tool call.
- **A close while a modal dialog runs.** A dialog that nobody suppressed (the startup "restore
  unsaved items?" prompt, a dialog the user opened) runs a nested event loop, and every tool call's
  work runs inside it. A frame closed there is deleted at that loop's idle time and deletes the dialog,
  which lives on its caller's stack: on 2026-09-26 `quit_app` under the restore prompt aborted the app
  ("pointer being freed was not allocated"). So, before anything else, the close handler asks
  `OrcaMCP::hold_close_while_modal` (`OrcaMCPQuit.cpp`, wx side `OrcaMCPQuitApp.cpp`):
  - **Which dialogs.** A `wxModalDialogHook` sees every `ShowModal`, innermost last: the DPIDialogs, the
    ~23 plain `wxDialog` subclasses that never enter `dialogStack` (WipingDialog, FilamentMapDialog,
    ...), and native alerts and file choosers, which only the user can close. Each showing has its own
    id, so a dialog opened at a closed one's address is not mistaken for it.
  - **One per turn, innermost only.** A close that cannot be vetoed ends the innermost dialog, if it is
    the app's, and asks again on a 50 ms timer, not `CallAfter` (wx runs an event posted from a pending
    event in the same pass, before the ended loop has returned). A dialog ended under another is only
    hidden, its loop still running. The close goes on once no dialog is open and the event loop is the
    main one. It gives up after 10 s rather than tear the frame down under a loop that will not end:
    the app stays open, its unsaved changes kept (a forced close clears the dirty flag only once it is
    sure to go on), and every response's `active_warnings` carries a `QuitFailed` error until the next
    `quit_app`. The log has the first turn and every outcome with its reason (`hold_log`), not
    each turn; an ordinary close, nothing open, at debug.
  - **Unanswered, with the dialog's own no** (`end_dialog_unanswered`, `OrcaMCPQuitApp.cpp`): its Cancel
    button if it has one; else its No button, if that still says No or Cancel; else Cancel, what its
    close box returns. Never `wxID_ABORT`: callers that test for No or Cancel only take it for yes. The
    restore prompt tells a quit from a No by `closing_dialogs_to_quit()` and keeps its backup.
  - **A quit request from the system never tears the frame down under a dialog**
    (`session_end_closes_frame`, in GUI_App's `wxEVT_QUERY_END_SESSION` handler): the Dock's Quit, a quit
    Apple Event, a logout. It arrives inside the dialog's loop, where the close tore the frame down and
    aborted, and ending the dialog would answer it for the user. So with a dialog open the app config is
    saved and the logs flushed at once (the system may end the process anyway: a Windows critical
    shutdown ignores a refusal), and the request is refused when it can be, as wx's own macOS handler
    does; the user answers and quits again. With nothing open the frame closes as for any quit.
  - **Nor at the end of the session** (the same `session_end_closes_frame`, GUI_App's `wxEVT_END_SESSION`
    handler), which follows a request the app did not refuse or could not (the process ends as soon as
    it returns). With nothing open wx closes the frame as usual. With a dialog open wx's own handler
    would `Close(true)` the frame inside the dialog's loop, the abort above, so the frame is left alone:
    the config is saved, a warning logged, and the system ends the process.
  - **Two waits, one close.** The hold (a timer turn) and the tool-call deferral above (once the work
    has returned) wait for different things, so they stay two, but both ask the same `close_again`.
- **The cloud login shares the MCP server's thread.** Its callback port is a second listener on the MCP
  server (`HttpServer::listen_also`, `LoginCallbackServer` in `OrcaMCPLoginServer.cpp`), so login
  callbacks and MCP calls are served one at a time, as when they shared one port. A login never
  stops, moves or re-routes the MCP server, never binds its port, and is refused once the gate is
  closed. Anything else that stops or restarts an `HttpServer` from the main thread needs the same
  care.

Both servers listen on **127.0.0.1 only**: MCP has no authentication and can start prints.

### Security: web pages never reach MCP

Loopback alone does not keep out the browser, which runs on this machine too: any site could POST a
tool call to `http://localhost:13618/mcp` as a plain-text request (no CORS preflight) and start a
print. So every request passes `OrcaMCP::app_request_guard` (`OrcaMCPRequestGuard.cpp`, installed
with `HttpServer::set_request_guard` in `GUI_App::start_http_server`) before any handler sees it:

- **An `/mcp` request carrying an `Origin` header is refused**: HTTP 403, JSON-RPC **-32003**. Browsers
  send `Origin` on every cross-origin fetch and form post; the bridge, curl and other local MCP clients
  send none. No page in the app calls `/mcp` (Device tab, printer and Obico pages, Home, every script
  under `resources/web`; checked 2026-09-26), so no origin is allowed. A new page that needs MCP
  must be added to the rule deliberately.
- **An `/mcp` request whose `Host` is not `127.0.0.1:<port>`, `localhost:<port>` or `[::1]:<port>` is
  refused** the same way, against DNS rebinding (an attacker's name pointed at 127.0.0.1 makes its
  page same-origin, with no `Origin`, but with that name in `Host`).
- **A cloud-login callback is answered only while a login is in progress, on the port it listens on**
  (`LoginCallbackServer::listens_on`), 404 everywhere else. The login is in progress while its dialog
  is open: `GUI_App::ShowUserLogin` calls `stop_listening()` once `ShowModal()` returns, however it
  ended, which closes the listener. The callbacks are browser navigations from the cloud's page, so
  they get neither rule above; but a page can make the browser deliver a forged one, which would sign
  the user in to someone else's account. The Orca cloud's code callback is also checked against the
  state and PKCE verifier its dialog issued (`OrcaCloudServiceAgent::exchange_auth_code`); Bambu's
  `access_token` and `ticket` callbacks carry no state to check, so closing the route is their only
  guard. `/mcp` is matched on the path alone, so a callback whose query mentions `/mcp` stays a
  callback.
- **No reply carries `Access-Control-Allow-*`**, so no page can read one either.

The bridge sends no `Origin` and names `localhost` or `127.0.0.1` (`scripts/tests/test_bridge_request_headers.py`).

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
2. **Slicing progress**: `get_slicing_status`'s per-plate `percent` and `stage` stand still while another job (an arrange, an orient) runs: the GUI drops progress updates then (`Plater::priv::on_slicing_update`)
3. **Threading**: Long operations may cause HTTP timeouts (120s default)
4. **Quitting**: a tool call waiting for the GUI when the app quits gets -32002 and was not run; one
   blocked on the network gets its tool error ("Request cancelled", "Printer discovery was
   cancelled") within about a second. `quit_app` itself is served after the call ahead of it on the
   one HTTP thread, e.g. a `discover_printers` runs out its timeout first. A Bambu cloud sign-in
   callback in flight is not cancellable (the network plugin's own calls); if it still runs 5 s into
   the quit, the process ends there, without its teardown (the config is saved first). A quit,
   `new_project` or `load_project` during a slice first waits for the slice to cancel (upstream's
   `BackgroundSlicingProcess::stop`): usually well under a second, but organic tree supports check
   for a cancel only between phases, and on the -O0 dev build one wait took 47 s
5. **Local only**: the MCP server listens on 127.0.0.1; it cannot be reached from another machine

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
| Error dialogs from `GUI::show_error` (`ErrorDialog`, "OrcaMCP error": a load that fails -- an STL the reader cannot parse, G-code that will not process, an invalid 3MF configuration -- or "Another export job is running.") | Never opened. `show_error` defers its dialog with `CallAfter`, so it used to open after the tool call had returned and suppression was over: a modal nobody answers. Under suppression on the GUI thread the text is captured as an error (`add_mcp_suppressed_error`): `export_gcode` fails with it (`message` and `error_messages`); `load_model`, `load_project` and `new_project` fail with it only when they changed nothing, and otherwise succeed and list it in `error_messages` (`OrcaMCP::load_answer`: a load that added objects must not read as failed, or an agent loads it again); every other tool lists it in `info_messages`. The invalid-G-code `MessageDialog` of a G-code load is tagged `set_mcp_error()` and counts the same way |
| Startup "Previously unsaved items have been detected. Restore them?" prompt (`EVT_RESTORE_PROJECT`, after a crash) | **Not suppressed**: no MCP call is in flight at startup, so it waits for the user, and every tool call runs underneath it (`get_scene_info`'s `open_dialogs` and an `OpenDialog` active warning show it). `quit_app` closes it unanswered (as No) and the backup is kept, so the next launch asks again; `quit_app` with `discard_changes: false` refuses and names it. While it waits, `new_project` and `load_project` are refused: they would point `last_backup_path` at the new project's backup, and the one it offers would never be offered again |
| Send-to-printer (`send_to_printer`) | **Bambu:** the `SelectMachineDialog` is scheduled with `CallAfter` and the tool returns `dialog_opened`; the user drives it. **Print hosts (Flashforge, Moonraker, OctoPrint, …):** by default (`direct: true`) there is **no dialog** — the tool uploads the sliced plate and, because `start_print` also defaults to true, **starts the print**. It returns `queued`. Pass `start_print: false` to upload only, or `direct: false` to open the print-host dialog instead. Never call it to "look at the dialog": on 2026-09-18 that started a 7 h print. |

### Implementation

Dialog suppression is implemented in:
- `GUI.hpp/cpp`: `set_mcp_dialog_suppression()`, `is_mcp_dialog_suppression_enabled()`,
  `add_mcp_suppressed_answer()` (the `(auto-answered …)` format every site uses), and the per-prompt
  answers (`set_mcp_prompt_answer()`, `mcp_answer_for()`)
- `MsgDialog.cpp`: `ShowModal()` override checks suppression flag; a dialog tagged with
  `set_mcp_prompt_key()` takes the answer the tool set with `McpDialogSuppressionGuard::answer_prompt()`,
  and one tagged `set_mcp_error()` is recorded as an error
- `GUI.cpp`: `show_error()` captures its text under suppression instead of deferring the dialog
  (`mcp_captures_error()`); the guard reads the errors apart (`errors()`, `notices()`, `fail_on_errors()`)
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
| Slicing & Printing | `slice_all`, `get_slicing_status`, `get_print_estimate`, `export_gcode`, `send_to_printer` (the first four apply a settings change the slicer has not taken in yet, and that update can raise an error dialog: `OrcaMCP::apply_pending_update` takes the caller's open guard) |

Error messages that would have been shown in dialogs are captured and returned in the response as `error_messages` (for failures) or `info_messages` (for non-critical information). A tool that assembles `info_messages` itself from `messages()` lists the errors there too; `report()` and `notices()` keep them apart.

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

While the app shows a dialog that waits for the user (the startup restore prompt, one the user
opened, a system file chooser or alert), every response's `active_warnings` also carries a `warning`
of type `OpenDialog` naming it (`open_dialog_warning`, `OrcaMCPQuit.cpp`); `get_scene_info` lists the
titles in `open_dialogs`. A `quit_app` that could not close a dialog within 10 s leaves an `error` of
type `QuitFailed` there until the next `quit_app`: the app is still running, its changes kept. A Slice
All run that stopped early because another job (an arrange, an orient) was running leaves a `warning` of
type `SliceAllEndedEarly` naming the plate, until the next slice, plate-list change or project. A
slice the plate list's safety net cancelled (see probe P) is told once, as a `warning` of type
`SliceCancelled` naming the path that freed its plate.

The object list's mesh warning icon (open edges, or repairs a 3MF recorded) is scene state no MCP tool
can clear, so it is **not** in every tool's `active_warnings`: a permanent entry there would keep
`count` above 0 on every call, `get_slicing_status`'s polls included. It is reported where objects
are: each object entry's `mesh_warning` / `mesh_warning_reason` (`get_scene_info`, `load_model`'s
`loaded_objects`), and `get_mesh_health`. Only `get_scene_info` (every flagged object) and `load_model`
(the flagged objects it added) also add a `warning` of type `MeshErrors`, with `object_id`,
`object_name` and a `message` that gives the list's reason and what an agent can do -- MCP cannot
repair, and slicing closes each layer's outline across gaps of up to 2 mm -- in place of the GUI
tooltip's "Click the icon to repair model object" (`mesh_error_warnings`, `OrcaMCPMeshHealth.cpp`).

**Endpoints with active_warnings:** `get_scene_info`, `slice_all`, `get_slicing_status`, `get_print_estimate`, `load_model`, `arrange_objects`, `auto_orient`, all transform tools, `undo`, `redo`

The `count` field is always present (even when 0) to help confirm issues have been resolved.

---

## Environment Variables (Bridge Script)

| Variable | Default | Description |
|----------|---------|-------------|
| `ORCAMCP_HOST` | `localhost` | OrcaSlicer HTTP server host. The app listens on 127.0.0.1 only, so this is `localhost` or `127.0.0.1`; another machine cannot reach it |
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
echo "K Flashforge host ip:port keeps its port in the URL (rel2506/04):   $(U src/slic3r/Utils/Flashforge.cpp | grep -c 'const auto slash_pos = host.find')"
echo "L Flashforge local API: no retry, failure log or next step (rel2506/04; 0 = bug): $(U src/slic3r/Utils/Flashforge.cpp | grep -c 'run_with_retry')"
echo "M HttpServer::stop cuts a reply still being written (rel2506/04b): $(U src/slic3r/GUI/HttpServer.cpp | awk '/^void HttpServer::stop/{f=1} f&&/stop_all\(\)/{print "yes"; exit} f&&/^}/{print "no"; exit}')"
echo "N HttpServer listens on every interface (rel2506/04b):             $(U src/slic3r/GUI/HttpServer.hpp | grep -c 'acceptor(io_service, {boost::asio::ip::tcp::v4()')"
echo "R HttpServer serves a request without reading its Origin (rel2506/04b): $( { U src/slic3r/GUI/HttpServer.hpp; U src/slic3r/GUI/HttpServer.cpp; } | grep -qi '"origin"' && echo no || echo yes)"
echo "O priv::reset frees the prints before it stops the slice (rel2506/04c): $(U src/slic3r/GUI/Plater.cpp | awk '/^void Plater::priv::reset\(bool/{f=1} f&&/background_process\.(stop|reset)\(\)/{print "no"; exit} f&&/partplate_list\.reinit\(\)/{print "yes"; exit}')"
echo "P a Print the slice uses is freed unchecked / init() reuses print indices / a completion credits the current plate as posted (rel2506/04c): $(U src/slic3r/GUI/PartPlate.cpp | awk '/^int PartPlateList::destroy_print\(int/{f=1} f&&/before_free|stop/{print "no"; exit} f&&/delete it->second/{print "yes"; exit}') / $(U src/slic3r/GUI/PartPlate.cpp | awk '/^void PartPlateList::init\(\)/{f=1} f&&/m_print_index = 0;/{print "yes"; exit} f&&/^}/{print "no"; exit}') / $(U src/slic3r/GUI/Plater.cpp | grep -c 'get_current_plate()->update_slice_result_valid_state(evt.success())')"
echo "Q restore prompt closed by a quit deletes the backup (rel2506/04c):  $(U src/slic3r/GUI/Plater.cpp | awk '/EVT_RESTORE_PROJECT, \[this/{f=1} f&&/closing_dialogs_to_quit|wxID_ABORT/{print "no"; exit} f&&/remove_all\(last\)/{print "yes"; exit}')"
echo "S the logout handler ends dialogs with wxID_ABORT (rel2506/04c):    $(U src/slic3r/GUI/GUI_App.cpp | grep -c 'EndModal(wxID_ABORT)')"
echo "V the object list's mesh-error text lives inside ObjectList / its first icon reads mesh().stats() / ObjectList::get_repaired_errors_count exists (rel2506/05): $(U src/slic3r/GUI/GUI_ObjectList.cpp | awk '/^MeshErrorsInfo ObjectList::get_mesh_errors_info\(const int obj_idx/{f=1} f&&/_L_PLURAL/{print "yes"; exit} f&&/^}/{print "no"; exit}') / $(U src/slic3r/GUI/GUI_ObjectList.cpp | grep -c 'get_warning_icon_name(model_object->mesh().stats())') / $(U src/slic3r/GUI/GUI_ObjectList.cpp | grep -c '^int ObjectList::get_repaired_errors_count')"
echo "X upstream's slic3rutils tests get no Windows-first force-include (rel2506/ci-fixes; 0 = bug): $(U tests/slic3rutils/CMakeLists.txt | grep -c 'win_platform.hpp')"
echo "Y the processor reads print_z only from Bambu's Z tag / the G-code check and / the filament grouping take an unset extruder height for 0 mm / the height message indexes a 2-name list / the G-code check takes a printable_height of 0 for 0 mm / the Type 1 tower takes an unset extruder height for 0 mm (rel2506/07b): $(U src/libslic3r/GCode/GCodeProcessor.cpp | grep -c 'if (boost::starts_with(comment, " Z_HEIGHT:")) {') / $(U src/libslic3r/GCode/GCodeProcessor.cpp | grep -c '(extruder_id < printable_heights.size()) && (iter->second.max_print_z > printable_heights\[extruder_id\])') / $(U src/libslic3r/PrintObject.cpp | awk '/double printable_height = printable_height_per_extruder\[extruder_id\];/{getline; print ($0 ~ /for \(/ ? "yes" : "no"); exit}') / $(U src/slic3r/GUI/GLCanvas3D.cpp | grep -c 'std::string extruder_name = extruder_name_list\[extruder_id-1\]') / $(U src/libslic3r/GCode/GCodeProcessor.cpp | grep -c 'if ( iter->second.max_print_z > plate_printable_height ) {') / $(U src/libslic3r/GCode/WipeTower.cpp | grep -c 'layer_id && layer_z > m_printable_height\[extruder_id\]')"
echo "Z reslice refuses on a validation failure a settings fix removed until the 0.5 s timer runs / show_error defers its dialog (rel2506/06b): $(U src/slic3r/GUI/Plater.cpp | grep -c 'process_completed_with_error, return directly') / $(U src/slic3r/GUI/GUI.cpp | awk '/^void show_error\(wxWindow\* parent, const wxString/{f=1} f&&/CallAfter/{print "yes"; exit} f&&/^}/{print "no"; exit}')"
echo "AA upstream's slicer reads a layer range's layer_height unchecked, and a file can carry a range without one (rel2506/06b): $(U src/libslic3r/Slicing.cpp | grep -c 'it_range->second.option("layer_height")->getFloat()')"
echo "AB Print::apply copies a new object name without invalidating the G-code (rel2506/06b): $(U src/libslic3r/PrintApply.cpp | awk '/model_object.name       = model_object_new.name;/{print (prev ~ /invalidate_step\(psGCodeExport\)/ ? "no" : "yes"); exit} {prev=$0}')"
echo "AC ObjectList::get_default_layer_config reads the preset's float \"extruder\" (rel2506/06b): $(U src/slic3r/GUI/GUI_ObjectList.cpp | awk '/^DynamicPrintConfig ObjectList::get_default_layer_config/{f=1} f&&/opt_float\("extruder"\)/{print "yes"; exit} f&&/^}/{print "no"; exit}')"
echo "AD reload_scene recycles a GLVolume without its instance's printable flag (rel2506/06b): $(U src/slic3r/GUI/GLCanvas3D.cpp | awk '/^void GLCanvas3D::reload_scene/{f=1} f&&/[.>]printable *= /{print "no"; exit} f&&/^}/{print "yes"; exit}')"
echo "AE cancel_all leaves a job whose process has returned to finalize as not cancelled (rel2506/07e; 0 = bug): $(U src/slic3r/GUI/Jobs/BoostThreadWorker.hpp | grep -c 'cancel_all_count')"
echo "AF a cancelled or failed arrange keeps prepare_all's plates locked, its running flag and its notification (rel2506/07e): $(U src/slic3r/GUI/Jobs/ArrangeJob.cpp | awk '/^void ArrangeJob::finalize/{f=1} f&&/lock\(false\)|end_arrange_run/{print "no"; exit} f&&/if \(canceled \|\| eptr\)/{print "yes"; exit}')"
```

Items M and N: upstream's `HttpServer::stop` closes every connection at once, so a reply still being
written is cut (ours drains it, `IOServer::begin_stop`), and upstream binds all interfaces (we bind
127.0.0.1). Our other `HttpServer` changes are features, not fixes: the second listener for the
cloud login (`listen_also`) and the loopback endpoint helper. On "no" / 0, take upstream's code and
re-check that a reply in flight still reaches its client and the bind is still loopback.

Item R: upstream's `HttpServer` never reads a request's `Origin` header, so it serves whatever page
reaches it. Ours passes every request's `Origin`, `Host` and arrival port to a guard before the handler
(`set_request_guard`, called from `session::process_request`; `http_headers::value`), and
`OrcaMCP::app_request_guard` refuses web pages. On "no", upstream reads `Origin` somewhere: see whether
its check can replace our hook.

Item O: upstream's `Plater::priv::reset` calls `partplate_list.reinit()`, which deletes every plate's
`Print` -- the one the slicing thread is using too -- and repoints the background process at a new
one, before `background_process.reset()` stops the slice. The stop then cancelled the new `Print`
while the thread ran on in freed memory: quit, New Project (Cmd-N skips the menu's "not while
slicing" check) or Open during a slice crashed the app, SIGSEGV on the slicing thread (2026-09-26,
release and dev builds).
Ours stops the slice just before `reinit()`. On "no", take upstream's order and re-check a quit during
a tree-support slice.

Item P: the same wherever the plate list changes under a slice. Deleting the plate being sliced
deleted its `Print` under the slicing thread (SIGSEGV, 2026-09-26); undo and redo delete every
`PartPlate` and read them back (`rebuild_plates_after_deserialize`), leaving the process pointing at a
freed plate; a move or an arrange that recycles empty plates reorders the plates Slice All walks by
index. Ours stops the slice, and cancels a Slice All run, once the change is sure to happen
(`PartPlateList::before_plate_list_change` in `delete_plate` after its checks, `move_plate_to_index`,
`load_from_3mf_structure`, and `Plater::priv::undo_redo_to` before the jump, whose topmost snapshot
reads the plates' filament maps the slicing thread writes), all through
`Plater::priv::stop_slice_for_plate_list_change`, which reports a cancel only for a slice still in
progress (`BackgroundSlicingProcess::stop`'s `cancelled_a_slice`), then repoints the process at the
current plate. MCP's `delete_plate`, `undo` and `redo` report what it did (`slice_cancelled`). A jump
whose snapshot load throws is not mended (`recover_from_failed_jump`): the process is pointed at the
Plater's own empty Print and no plate, and MCP refuses every tool but saving a copy and quitting until
a restart (`OrcaMCP::refusal_after_failed_jump`). Under those stops is a safety net:
`PartPlateList::clear`, `destroy_print` and `delete_plate` tell a hook the Plater installs
(`set_before_free`) what they are about to free, and a slice running on it is stopped there with a
warning naming the path, whatever the caller (MCP tells it once: `SliceCancelled`). A completion is
credited by the print index it carries (`SlicingProcessCompletedEvent::print_index`) to the plate that
still holds it, and decided when it is handled, not when it was posted
(`OrcaMCP::credit_completion`): marked sliced only if that plate's current Print is finished and it
passed validation (`is_apply_result_invalid`, and not a start refused for a failed one). Upstream
marks whichever plate the process points at by then, with whatever the event said. Upstream's Slice
All also posts a "finished" for a plate it could not start (`restart_background_process` returns
false when the UI worker is busy, as well as when the Print is already finished) and so marks a
never-sliced plate sliced; ours ends the run on a busy worker (`Plater::priv::post_plate_not_started`,
`OrcaMCP::plate_not_started`; `get_slicing_status` says `slice_run.ended_early`). And
`PartPlateList::init()` no longer restarts print indices at 0, so a new project's plate never takes an
old one's. For each "no" (or 0), take upstream's and re-check that part: `delete_plate` and undo "add
plate" during a slice, a completion queued across `new_project`, Slice All after re-selecting the
same preset, Slice All while an arrange runs.

Item Q: upstream's restore prompt treats every answer but Yes as No and deletes the crashed session's
backup, including a prompt the app itself closed to quit (its system-logout handler, and our
`quit_app`). Ours returns when `OrcaMCP::closing_dialogs_to_quit()` and keeps the backup, so the next
launch asks again, and marks the prompt open (`RestorePromptOpen`) so MCP's `new_project` /
`load_project` refuse while it is open. On "no", take upstream's handler and re-check both.

Item S: upstream's `wxEVT_QUERY_END_SESSION` handler (the Dock's Quit, a quit Apple Event, a logout)
closes the main frame while a dialog's modal loop is on the stack -- the teardown-under-a-dialog abort
-- and then ends every dialog in `dialogStack` with `EndModal(wxID_ABORT)`, which callers that test
only for No or Cancel take for yes ("Sync printer information?" syncs). Ours never closes the frame
while a dialog is open (`OrcaMCP::session_end_closes_frame`): it saves the config and refuses the
request, as wx's own macOS handler does. On 0, take upstream's and re-check a Dock Quit with the
restore prompt open.

Item J: upstream opens every recent 3MF synchronously while building the main window, before
post_init starts the MCP server. Our patch skips it for an agent launch (`GUI::is_agent_launch()`,
`MainFrame.cpp`). "no" means upstream moved the load off the GUI thread: re-check whether our skip
is still needed.

Item X: on Windows every libslic3r_gui source starts with `<Windows.h>`, from its precompiled header or
`/FIslic3r/win_platform.hpp` with the PCH off; upstream's slic3rutils test target gets neither, and
guards a few tests with a hand-written Windows prologue (`test_dev_mapping.cpp`). A test that reaches
wx after libslic3r headers -- `PartPlate.hpp` does -- fails in `wx/msw/private.h` and asio. Ours
force-includes `slic3r/win_platform.hpp` for the test target (`tests/slic3rutils/CMakeLists.txt`). On
a non-zero, take upstream's line and drop ours.

Item V is a move, not a fix: upstream builds the object list's warning-icon tooltip inside
`ObjectList::get_mesh_errors_info`, which needs the list. Ours moves the body into the free function
`mesh_errors_info(const TriangleMeshStats&, ...)` in the same file (so `localization/i18n/list.txt`
still reaches its strings), taking the stats the member used to look up, and counting the repairs
with `repaired_errors_count`, the sum `ObjectList::get_repaired_errors_count` returned. That member
had no other caller, so ours removes it (the third count), and `get_warning_icon_name` is no longer
`static`. The member calls the free function; MCP's `get_mesh_health` and `get_scene_info` call it too.
On "no", upstream changed that function: redo the move on its version and re-run `[MeshHealth]`. If
upstream gains a caller of `get_repaired_errors_count`, restore the member. The second half is a fix: upstream's `add_object_to_list` sets an object's first
icon from `mesh().stats()`, which merges the model parts only and keeps only the last part's
repairs, while every later icon update and the tooltip read `get_object_stl_stats()`; a modifier's hole
or an earlier part's repairs left the icon off under a tooltip listing them. Ours reads
`get_object_stl_stats()` there too. On 0, take upstream's line.

Item Y: `GCode::process_layer` writes a layer's height as `; Z_HEIGHT:` for Bambu printers and `;Z:`
for every other, but upstream's processor reads only the first, so on every non-Bambu printer each
move's `print_z` stayed 0 and the check after a slice (`check_multi_extruder_gcode_valid`) never found
a toolpath above the printable height. It can be there: the pre-slice height check measures the
object without its raft. Ours reads both tags (`is_layer_z_tag`). That alone would have failed every
slice on the 155 non-Bambu multi-extruder profiles: none sets `extruder_printable_height`, whose 0
default the per-extruder check took for a 0 mm limit. So an unset height limits nothing
(`is_height_limit`, `PrintableHeightLimit.hpp`), there, in the bed check -- a `printable_height` of 0,
which the build volume takes for no height limit, would otherwise have failed every extrusion (the
pre-slice check still refuses an object on it, as upstream does) -- and in
`PrintObject::detect_extruder_geometric_unprintables`, where the same 0 marked every filament with a
per-feature assignment unprintable on that extruder and cancelled another extruder's real limit in the
filament grouping, and in `WipeTower::is_valid_last_layer`, where on a non-Bambu printer with the Type 1
tower (Bambu profiles set `[]`, which it skips) the same 0 dropped a block, its nozzle-change depth
and its ramming on each extruder's last tool-change layer. And the per-extruder height message names
a third or fourth tool "Tool N" instead of reading past the two Bambu nozzle names
(`extruder_display_name` in `_set_warning_notification`).
`print_z` has no other reader: the viewer, libvgcode, `custom_gcode_per_print_z` and MCP's layer plan
use move positions. For each 0 or "no", take upstream's code and re-run `fff_print_tests
"[GCodeHeightCheck],[PrintableHeight],[WipeTower]"`.

Item Z: a settings change reaches the slicer only when `background_process_timer` fires, 0.5 s later
(`Plater::priv::schedule_background_process`); until then upstream's `reslice()` returns early on the
last validation failure (`process_completed_with_error`), so a `slice_all` right after an agent fixed a
bad setting reported the old failure. Ours adds `Plater::apply_pending_background_update()`, which runs
what the timer's handler runs, only when it would run it; MCP calls it before slicing, reporting or
exporting, and only while the pipeline is idle (`OrcaMCP::should_apply_pending_update`). On 0, upstream
dropped the early return: re-check that a fixed setting then slices at once, and remove the hook if so.
The second check: upstream's `GUI::show_error` defers its `ErrorDialog` with `CallAfter`, so under MCP
it opened after the tool call had returned and suppression was over. Ours captures the text under
suppression on the GUI thread (`mcp_captures_error`). On "no", upstream shows it synchronously:
`MsgDialog::ShowModal` then catches it, and the capture can go -- keep the error channel
(`add_mcp_suppressed_error`) by tagging `ErrorDialog` instead.

Item AA: upstream's slicer (`layer_height_profile_from_ranges`, `layer_height_profile_adaptive`),
`Print::apply`'s range comparison and the object list all read a layer range's `layer_height`
without checking it is there. The object list always gives a range one, but a file can carry a
range without it (the importers copy whatever options it lists), the CLI's assemble list builds
ranges from whatever `range_params` it is given, and MCP's `set_object_layer_range` wrote such
ranges until rel2506/06b; the next slice dereferenced null and crashed the app (and the CLI). Ours
completes every range where it enters a scene -- `Plater::priv::load_model_objects` (every GUI and
MCP load), the CLI before slicing, `set_object_layer_range` -- with `complete_layer_ranges` (Model.cpp):
the object's effective layer height from the settings the caller slices with, within its nozzle's
limits, and extruder 0. The slicer keeps a backstop: a range that still arrives without one prints
at the object's layer height. On 0, upstream checks the option itself: drop the backstop, keep the
completion (the object list and `Print::apply` need it), and re-run `libslic3r_tests "[LayerRanges]"`
and `fff_print_tests "[LayerRanges]"`.

Item AC: upstream's `ObjectList::get_default_layer_config` (the defaults "Add height range" gives a
new range) also read the object's extruder, unused, falling back to the process preset's float
`extruder`, which it does not have: on an object without an extruder of its own it dereferenced
null and crashed the app. Ours returns `layer_range_defaults` (Model.cpp), the same defaults a
loaded file's ranges are completed with, by the extruder that prints the range (the range's own,
else the object's); MCP's `set_object_layer_range` completes through the same function. On "no",
take upstream's and re-check "Add height range" on an object whose config has no `extruder`.

Item AB: an object's name is in its G-code (the `; printing object` labels, `EXCLUDE_OBJECT` names,
`{first_object_name}`), but upstream's `Print::apply` copies a new name over without invalidating
anything, so a finished plate kept its G-code and the next slice took it back with the old name in
it. Ours invalidates `psGCodeExport` when the name changed. On "no", take upstream's and re-run
`fff_print_tests "Renaming an object after a slice*"`.

Item AD: a GLVolume's `printable` flag is set only by the printable toggles
(`GLCanvas3D::update_instance_printable_state_for_object`); upstream's `reload_scene` recycles a volume
without it. After an undo or redo of a toggle (the object list's or MCP's `set_object_printable`) the
volume kept the other state; the canvas's outside check skips unprintable volumes, so the plate had
nothing on it, was marked not ready to slice, and the next background update set
`process_completed_with_error`: `reslice()` refused the plate, silently, until the object was moved.
Ours sets every volume's flag from its instance in `reload_scene`, as it sets the instance's
transformation. On "no", take upstream's and re-check `set_object_printable` false, `undo`,
`slice_all`.

Item AE: upstream's `BoostThreadWorker` records whether a job was cancelled when its `process()`
returns, and delivers the finalize with that verdict later, on the main thread's next idle. A
`cancel_all` in between -- which `Plater::priv::remove`, `delete_object_from_model`, `reset` (a new or
loaded project) and `remove_selected` call just before freeing the model's objects -- was lost, and
the arrange's or orient's finalize then wrote to instances that had just been freed. Ours counts
`cancel_all` calls: a job pushed before one is finalized as cancelled, whenever it came
(`cancel_all_count`). So in that window the GUI now drops the result of an arrange or an orient
where upstream applied it: a delete, a new project, or a reslice's `stop_queue` (cancel_all, then
wait) that lands after the job computed its result and before it was applied cancels it, and the
objects stay where they were. That is what `cancel_all` means -- "delete the queued jobs and cancel
the current one" -- and every finalize already handles `canceled` by applying nothing. On a
non-zero, take upstream's and re-run `slic3rutils_tests "[McpUiJob]"`.

Item AF: upstream's `ArrangeJob::finalize` returns early for a cancelled or failed arrange, before
it undoes what the run set up: the plates `prepare_all` locked because their print sequence differs
from the global one stayed locked (a lock icon the user never set; every later arrange, orient and
`flatten_object` took them for locked), `Plater::m_arrange_running` stayed set (the plate toolbar's
arrange button, `last_arrange_job_is_finished`, did nothing again until an arrange was applied), and
"Arranging..." stayed up until it timed out. Item AE makes a cancel in the post-process window a
cancelled finalize too. Ours undoes all three however the finalize returns (`end_arrange_run`, from a
`ScopeGuard` at its top). On "no", take upstream's and re-run `slic3rutils_tests "[McpUiJob]"`.

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
