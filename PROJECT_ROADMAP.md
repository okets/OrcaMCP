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

## Milestone 1 — Endpoint Reliability

- [x] Test fixture project created: `/Users/hanan/Documents/STL/delme.3mf`
- [ ] Run full endpoint checklist (see below)
- [ ] Verify 360°/multi-view rendering outputs
- [ ] Verify error handling for invalid inputs

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

- [ ] `GET /mcp` (server info JSON)
- [ ] `POST /mcp` accepts JSON-RPC 2.0 requests
- [ ] JSON-RPC methods:
  - [ ] `initialize`
  - [ ] `ping`
  - [ ] `tools/list`
  - [ ] `tools/call`

## Tools (44 total)

**Server / docs**
- [ ] `get_server_info`

**Project / scene**
- [ ] `get_scene_info` (with and without `with_model_object_features`)
- [ ] `new_project`
- [ ] `load_project`
- [ ] `save_project`
- [ ] `export_3mf`

**Presets / configuration**
- [ ] `get_presets`
- [ ] `select_preset`
- [ ] `apply_config` (batch apply + "dirty preset" behavior)
- [ ] `get_edited_presets`
- [ ] `get_valid_config_keys`

**Visualization**
- [ ] `render_plate_view` with `save_to_file=true` (single view)
- [ ] `render_plate_view` with `save_to_file=true` (multi-view)

**Model import**
- [ ] `load_model`

**Plate management**
- [ ] `add_plate`
- [ ] `select_plate`
- [ ] `delete_plate`

**Object queries**
- [ ] `get_object_info`
- [ ] `rename_object`

**Object transforms**
- [ ] `move_object` (relative and absolute)
- [ ] `rotate_object` (relative and absolute)
- [ ] `scale_object` (uniform and non-uniform)
- [ ] `mirror_object`
- [ ] `transform_objects` (batch)
- [ ] `clone_object` (instances vs duplicate)
- [ ] `flatten_object`
- [ ] `cut_object` (keep=below|above|both)
- [ ] `delete_object`
- [ ] `auto_orient`
- [ ] `arrange_objects`
- [ ] `undo`
- [ ] `redo`

**Per-object configuration**
- [ ] `get_object_config`
- [ ] `set_object_config`
- [ ] `reset_object_config`
- [ ] `get_object_layer_ranges`
- [ ] `set_object_layer_range`
- [ ] `delete_object_layer_range`

**Adaptive layer height**
- [ ] `apply_adaptive_layer_height`
- [ ] `clear_adaptive_layer_height`

**Slicing & output**
- [ ] `slice_all`
- [ ] `get_slicing_status`
- [ ] `get_print_estimate`
- [ ] `export_gcode`

**Printers**
- [ ] `get_printers`
- [ ] `select_printer`
- [ ] `send_to_printer`

## Preview-capable tools (test with include_preview=true)

- [ ] `get_scene_info`
- [ ] `move_object`
- [ ] `rotate_object`
- [ ] `scale_object`
- [ ] `mirror_object`
- [ ] `flatten_object`
- [ ] `clone_object`
- [ ] `arrange_objects`
- [ ] `auto_orient`
- [ ] `apply_adaptive_layer_height`
- [ ] `clear_adaptive_layer_height`

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
