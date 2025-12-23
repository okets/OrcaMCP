# OrcaMCP Roadmap

Keep this lightweight and execution-focused. Use the checkboxes as the single source of truth for "what's left".

---

## Documentation Framework (Completed)

- [x] Create `docs/` folder structure with hybrid approach
- [x] Architecture Decision Records (ADRs) for key decisions
- [x] API reference for all 44 MCP tools
- [x] Workflow guides for common tasks
- [x] Contributing guides (adding tools, code style)
- [x] Setup guides (building, configuration, troubleshooting)
- [x] Rewrite README.md to be OrcaMCP-first
- [x] Update CLAUDE.md with documentation index

**Created files:**
```
docs/
├── README.md                    # Navigation index
├── architecture/
│   ├── overview.md              # System architecture
│   ├── threading-model.md       # GUI thread patterns
│   └── transport-layer.md       # HTTP + stdio bridge
├── adr/
│   ├── README.md                # ADR template
│   ├── 0001-http-transport.md   # Why HTTP + bridge
│   ├── 0002-embedded-server.md  # Why embedded in OrcaSlicer
│   ├── 0003-json-rpc-protocol.md # JSON-RPC 2.0
│   └── 0004-main-thread-execution.md # Threading model
├── tools/
│   ├── reference.md             # All 44 tools documented
│   └── workflows.md             # Common patterns
├── contributing/
│   ├── adding-tools.md          # Guide for new tools
│   └── code-style.md            # C++/Python conventions
└── setup/
    ├── building.md              # Build instructions
    ├── configuration.md         # Claude Code setup
    └── troubleshooting.md       # Common issues
```

---

## Milestone 0 — JusPrin → OrcaMCP Transition (Completed)

- [x] App renamed to OrcaMCP (version.inc, CMakeLists.txt)
- [x] Custom branding (purple accent, MCP badges, build plate logo)
- [x] CLAUDE.md updated with project vision and architecture
- [x] Re-read JusPrin's original docs for any missed insights
- [x] `.mcp.json` configured for `scripts/orcamcp-bridge.py`
- [x] Verify Claude Desktop MCP config works
- [x] OrcaSlicer build includes OrcaMCP HTTP server on port 13618
- [x] Environment vars documented (ORCAMCP_HOST/PORT/TIMEOUT/DEBUG)
- [x] Fix JSON Schema compliance for Claude API (set_object_config, set_object_layer_range, transform_objects)

---

## Milestone 1 — Endpoint Reliability (Completed)

- [x] Test fixture project created: `/Users/hanan/Documents/STL/delme.3mf`
- [x] Run full endpoint checklist (see below) - 44/46 tools tested
- [x] Verify 360°/multi-view rendering outputs - Working perfectly
- [x] Verify error handling for invalid inputs

---

## Milestone 2 — Public Repo Readiness

- [x] README.md rewritten for OrcaMCP
- [x] Documentation framework complete
- [ ] GitHub Actions: build artifacts (macOS/Linux/Windows)
- [ ] License clarification (AGPL-3.0 from OrcaSlicer)
- [ ] Attribution notes for OrcaSlicer, JusPrin, upstream projects

---

## Milestone 3 — Future Enhancements

- [ ] Direct file export without dialogs
- [ ] Slicing progress percentage (not just running/idle)
- [ ] WebSocket support for real-time notifications
- [ ] Batch operations for multiple files
- [ ] Calibration tools (flow rate, pressure advance)

---

# MCP Endpoint Test Checklist

## Transport / Protocol

- [x] `GET /mcp` (server info JSON)
- [x] `POST /mcp` accepts JSON-RPC 2.0 requests
- [x] JSON-RPC methods:
  - [x] `initialize`
  - [x] `ping`
  - [x] `tools/list`
  - [x] `tools/call`

## Tools (46 total) - Tested 2024-12-23

**Server / docs**
- [x] `get_server_info`

**Project / scene**
- [x] `get_scene_info` (with and without `with_model_object_features`)
- [x] `new_project`
- [x] `load_project` *(works but may timeout on large files)*
- [x] `save_project` *(opens dialog if no filename set)*
- [x] `export_3mf` *(silent export with output_path)*

**Presets / configuration**
- [x] `get_presets`
- [x] `select_preset`
- [x] `apply_config` (batch apply + "dirty preset" behavior)
- [x] `get_edited_presets`
- [x] `get_valid_config_keys`

**Visualization**
- [x] `render_plate_view` with `save_to_file=true` (single view)
- [x] `render_plate_view` with `save_to_file=true` (multi-view / 360°)

**Model import**
- [x] `load_model`

**Plate management**
- [x] `add_plate`
- [x] `select_plate`
- [x] `delete_plate`

**Object queries**
- [x] `get_object_info`
- [x] `rename_object`

**Object transforms**
- [x] `move_object` (relative and absolute)
- [x] `rotate_object` (relative and absolute)
- [x] `scale_object` (uniform and non-uniform)
- [x] `mirror_object`
- [x] `transform_objects` (batch)
- [x] `clone_object` (instances vs duplicate)
- [x] `flatten_object`
- [x] `cut_object` (keep=below|above|both)
- [x] `delete_object`
- [x] `auto_orient`
- [x] `arrange_objects`
- [x] `undo`
- [x] `redo`

**Per-object configuration**
- [x] `get_object_config`
- [x] `set_object_config`
- [x] `reset_object_config`
- [x] `get_object_layer_ranges`
- [x] `set_object_layer_range`
- [x] `delete_object_layer_range`

**Adaptive layer height**
- [x] `apply_adaptive_layer_height`
- [x] `clear_adaptive_layer_height`

**Slicing & output**
- [x] `slice_all`
- [x] `get_slicing_status`
- [x] `get_print_estimate`
- [x] `export_gcode` *(opens dialog - silent export TODO)*

**Printers**
- [x] `get_printers`
- [ ] `select_printer` *(skipped - no Bambu printers available)*
- [ ] `send_to_printer` *(skipped - would open printer dialog)*

## Preview-capable tools (test with include_preview=true)

- [x] `get_scene_info`
- [x] `move_object`
- [x] `rotate_object`
- [x] `scale_object`
- [x] `mirror_object`
- [x] `flatten_object`
- [x] `clone_object`
- [x] `arrange_objects`
- [x] `auto_orient`
- [x] `apply_adaptive_layer_height`
- [x] `clear_adaptive_layer_height`

---

# Key Decisions Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2024-12 | HTTP + stdio bridge | OrcaSlicer is GUI app; pure stdio doesn't work |
| 2024-12 | Embedded server | Direct access to OrcaSlicer internals |
| 2024-12 | Main thread execution | wxWidgets/OpenGL require main thread |
| 2024-12 | Hybrid docs structure | CLAUDE.md entry point + docs/ for depth |
| 2024-12 | Purple branding | Distinguish from upstream OrcaSlicer |
| 2024-12 | Strict JSON Schema compliance | Claude API requires draft 2020-12; all properties need type |

See `docs/adr/` for detailed Architecture Decision Records.
