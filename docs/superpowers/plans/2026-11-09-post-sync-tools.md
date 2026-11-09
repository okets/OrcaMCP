# Post-sync MCP tools (upstream e2db7e5ef1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give agents what the 2026-11-09 upstream sync gave users: import options, primitive sizes, a primitive as a
new object, Order By and the real by-object print order, why a setting is unavailable, and the GUI's own checks on a
plate rename, the excluded bed area and a Flashforge manual-filament send.

**Architecture:** Each tool's decisions live in a pure, unit-tested file apart from the app (the fork's pattern:
`OrcaMCP<Thing>.cpp` + `tests/slic3rutils/test_<thing>.cpp`); the tool runs the app's own action on the main thread.
Where an upstream function hides what a tool needs, the fork adds the smallest hook to it and a carried-fix probe
(CLAUDE.md, "Carried upstream fixes").

**Tech Stack:** C++17, wxWidgets, nlohmann::json, Catch2 (`slic3rutils_tests`), CMake/Ninja.

**Spec:** the sync report of 2026-11-09 (this conversation) and the three change-list reviews it summarised; the
upstream commits are named per task. CLAUDE.md is the project's standing spec ("Tool list", "Dialog Suppression",
"Carried upstream fixes").

## Global Constraints

- Work on branch `sync-upstream-2026-11-09` in `/Users/hanan/Projects/OrcaMCP-sync-1109`; build dir `build/arm64`
  (Release, tests on). Never push.
- Commit at the end of every task, one commit per task, nothing else in it; `git status` clean after.
- Every argument a handler reads is declared; nested objects are `additionalProperties: false`; anything a response
  returns in a request's shape is accepted back (CLAUDE.md "Tool list").
- A value a tool cannot act on is refused with `{"status":"error","message"}` naming what to send, before any change
  or undo step. One undo step per call that changes the model, none for no change (`changed: false`).
- Tool summary ≤ 40 characters; `get_server_info`'s default response stays under 6 KB.
- Follow-ups go in `next_steps`, built only in `OrcaMCPNextSteps.cpp`, with a case in `test_mcp_next_steps.cpp`.
- No tool text sends an agent or user to a GUI button (security prompts excepted).
- After any schema or text change regenerate the golden file:
  `ORCAMCP_UPDATE_TOOLS_GOLDEN=1 build/arm64/tests/slic3rutils/Release/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][tools]"`,
  then run it again without the variable: PASS.
- New C++: no identifier Windows headers define (`near`, `far`, `small`, `min`, `max`, `ERROR`, `IN`, `OUT`,
  `interface`); include every wx header used (fork sources build without the PCH on Windows clang).
- A new tool updates: CLAUDE.md's tool table row and the "(N in the app, M reachable)" counts (now 111/115),
  `docs/tools/reference.md`, and the golden file.

## Review Focus

1. `import_options` on a file it does not apply to (STL, OBJ, a 3MF project) — refused, never silently ignored.
2. Order By on a project whose objects have instances on several plates — `object_id_changes` lists every renumbered
   object, those on other plates included (upstream re-sorts the whole list).
3. `print_order` asked of a plate that is not the current one, or not by object — `null` with `print_order_unknown`
   saying why, never a stale order from an earlier apply.
4. `add_volume size` carrying a field of another shape (`{"diameter": 20}` for a cube) — refused, naming the shape's
   fields.
5. `apply_config` of a key a settings tab disables — reported in `unavailable` with the tab's reason, read after
   `UpdatePresetTabs()` so the reason is the current one.

---

### Task 1: load_model `import_options`

Upstream fa5f7e3db4. Today MCP answers the import dialogs with the saved options (item CF,
`mcp_answer_geometry_import_options` in `Plater.cpp`); this lets one call choose.

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPGeometryImport.hpp`, `.cpp` (add to `src/slic3r/CMakeLists.txt` beside the
  other OrcaMCP sources)
- Create: `tests/slic3rutils/test_mcp_geometry_import.cpp` (add to `tests/slic3rutils/CMakeLists.txt`)
- Modify: `src/slic3r/GUI/Plater.cpp` (`mcp_answer_geometry_import_options` and the name-policy hook in
  `Plater::priv::load_files`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` (`load_model` schema and handler, ~line 3642)

**Interfaces:**
- Produces (namespace `Slic3r::OrcaMCP`):
  - `struct GeometryImportRequest { std::optional<bool> keep_names, sort_names; std::optional<std::string> load_as, name_cleanup, replacement; };`
    — `load_as` ∈ `assembly|single_object|separate_objects`, `name_cleanup` ∈ `remove|replace`.
  - `struct GeometryImportRead { std::optional<GeometryImportRequest> request; std::string refusal; };`
  - `GeometryImportRead read_geometry_import_request(const nlohmann::json& params);` — reads `params["import_options"]`.
  - `std::optional<std::string> geometry_import_refusal(const GeometryImportRequest&, const std::string& format, bool cad_3mf);`
    — `format` as `geometry_import_format()` returns it ("step", "amf", "3mf", "" for others).
  - `void apply_geometry_import_request(const GeometryImportRequest&, const std::string& format, GeometryImportOptions&);`
  - `void apply_geometry_import_request(const GeometryImportRequest&, GeometryImportNamePolicy&);`
  - `class ScopedGeometryImportRequest` (sets, then clears, the call's request; main thread) and
    `const GeometryImportRequest* current_geometry_import_request();`

- [ ] **Step 1: Write the failing tests** (`[McpGeometryImport][orcamcp]`):
  `"import_options reads every field"` — `{keep_names:false, sort_names:true, load_as:"single_object", name_cleanup:"replace", replacement:"-"}` round-trips;
  `"an unknown load_as is refused"` — refusal names `assembly, single_object, separate_objects`;
  `"replacement without name_cleanup replace is refused"`;
  `"an invalid replacement is refused"` — `replacement:"/"` refused (`valid_import_name_replacement`, `libslic3r/Format/ImportNames.hpp`);
  `"import_options on an STL is refused"` — `geometry_import_refusal(req, "", false)` names STEP, AMF and CAD 3MF;
  `"load_as on a 3MF is refused"` — `geometry_import_refusal({load_as}, "3mf", true)` set; keep_names alone on a CAD 3MF is fine;
  `"a 3MF project takes no import_options"` — `geometry_import_refusal(req, "3mf", false)` set;
  `"apply sets the dialog's fields"` — `single_object` gives `separate_objects=false, load_as_assembly=false`; `separate_objects` gives `separate_objects=true`; `show_dialog` false, `remember` false after apply;
  `"apply sets the name rule"` — `replace` + `"-"` gives `remove=false, replacement="-"`; `remove` gives `remove=true`.
- [ ] **Step 2: Run** `slic3rutils_tests "[McpGeometryImport]"` — FAIL (no such file/function).
- [ ] **Step 3: Implement** the functions above. `mcp_answer_geometry_import_options` applies
  `current_geometry_import_request()` before describing the answer, so `info_messages` names what was used ("the
  call's options" when a request is set, "the saved options" otherwise); the name-policy hook applies it before
  `prepare_geometry_import_names`. Nothing is saved to the app config.
- [ ] **Step 4: Wire `load_model`:** schema `import_options` object (`additionalProperties:false`, the five fields with
  enums); the handler reads and refuses (`geometry_import_refusal` with `geometry_import_format(file_path)` and
  `is_geometry_only_3mf(file_path)`) before loading, then loads inside a `ScopedGeometryImportRequest`. Describe in the
  tool text that a STEP's tessellation keeps the saved values.
- [ ] **Step 5: Run** `"[McpGeometryImport]"` and `"[McpModelLoad]"` — PASS; regenerate the golden file; `"[orcamcp][tools]"` PASS.
- [ ] **Step 6: Docs:** `docs/tools/reference.md` load_model section; CLAUDE.md tool table (Models row: `load_model`) and
  item CF paragraph (a call's options win over the saved ones).
- [ ] **Step 7: Commit** `mcp: load_model takes import_options for STEP, AMF and CAD 3MF imports`.

### Task 2: add_volume `size` (fork hook CG)

Upstream 0ef9015904 put the size into a dialog; `primitive_size` / `create_mesh` are `static` in `GUI_ObjectList.cpp`.

**Files:**
- Modify: `src/slic3r/GUI/GUI_ObjectList.hpp:399-400`, `GUI_ObjectList.cpp` (`primitive_size`, `load_generic_subobject`, `load_shape_object`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPartEdits.hpp/.cpp`, `OrcaMCPPartTools.cpp` (add_volume, ~line 987 and registration ~1347)
- Test: `tests/slic3rutils/test_part_edits.cpp`
- Modify: CLAUDE.md (probe CG + paragraph; tool table Models row)

**Interfaces:**
- Produces:
  - `void ObjectList::load_generic_subobject(const std::string& type_name, const ModelVolumeType type, bool interactive = true, const std::vector<double>* size = nullptr);`
  - `void ObjectList::load_shape_object(const std::string& type_name, bool interactive, const std::vector<double>* size = nullptr);`
    — a given `size` (in the dialog's field order) is used as is, no dialog.
  - `struct PrimitiveSize { std::vector<double> values; std::string refusal; };`
    `PrimitiveSize primitive_size_values(const std::string& shape, const nlohmann::json& size);` in `OrcaMCPPartEdits`
    — fields by shape: cube `x,y,z`; cylinder, cone, disc `diameter,height`; sphere `diameter`; torus
    `inner_diameter,tube_diameter`. Every field of the shape required; each finite, > 0 and ≤ 1000 mm.

- [ ] **Step 1: Failing tests** (`[PartEdits]`): `"a cube's size is x, y, z in that order"` → `{10,20,30}`;
  `"a torus's size is inner then tube diameter"`; `"a field of another shape is refused"` (cube + `diameter`, message
  lists `x, y, z`); `"a missing field is refused"`; `"a size of 0 or 2000 is refused"`.
- [ ] **Step 2: Run** `"[PartEdits]"` — FAIL.
- [ ] **Step 3: Implement** `primitive_size_values`; the hook in `primitive_size(type_name, interactive, size)` returns
  `*size` when given and its length matches the fields.
- [ ] **Step 4: Wire add_volume:** optional `size` object (`additionalProperties:false`; the seven field names); only
  with `shape`; refused before any change; pass `&values` to `load_generic_subobject(..., false, &values)`. Update the
  description ("a tenth of the bed's longest side" stays the default).
- [ ] **Step 5: Run** `"[PartEdits]"` PASS; golden file regenerated; `"[orcamcp][tools]"` PASS.
- [ ] **Step 6: CLAUDE.md:** probe line `CG upstream's primitive size lives in a dialog only (a hook: 0 = expected, keep ours)`
  counting `const std::vector<double>\* size` in upstream's `GUI_ObjectList.hpp`, and a short "Item CG" paragraph.
- [ ] **Step 7: Commit** `mcp: add_volume takes a primitive's size`.

### Task 3: `add_primitive_object` (new tool)

The canvas's Add Primitive as a new object (`ObjectList::load_shape_object`), which no tool covers.

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPartTools.cpp` (registration + handler), `tests/slic3rutils/test_mcp_tool_arguments.cpp` only if a new nested schema needs its case.

**Interfaces:**
- Consumes: `load_shape_object(type_name, false, &values)`, `primitive_size_values` (Task 2), `model_object_summary_json` (`OrcaMCPCommon.cpp`).
- Produces: tool `add_primitive_object {shape (enum as add_volume), size?}`, category `Models`, summary
  `"Add a cube, cylinder, ... as an object"` (≤ 40); answer `{status, changed:true, object: <model_object_summary_json>, object_id}`; one undo step (the list's own).

- [ ] **Step 1: Failing test:** `test_mcp_tool_arguments.cpp` already holds every tool to its schema; add the
  sent-back case only if `object` returns a request-shaped field (it does not). So the failing check is the golden
  file: register the tool, run `"[orcamcp][tools]"` — FAIL (golden differs).
- [ ] **Step 2: Implement** the handler: refusals shared with add_volume (UI job running, shape unknown, size via
  `primitive_size_values`); under `McpDialogSuppressionGuard`; the new object is the last; refuse with an error if none was added.
- [ ] **Step 3: Regenerate** the golden file; `"[orcamcp][tools]"` PASS; counts in CLAUDE.md become 112/116.
- [ ] **Step 4: Docs:** reference.md entry; CLAUDE.md Models row.
- [ ] **Step 5: Commit** `mcp: add_primitive_object adds a primitive as its own object`.

### Task 4: `order_objects` (new tool)

Upstream b4772dca38, `ObjectList::order_objects(ObjectOrder, bool all_plates)`: re-sorts the list, which is the
by-object print order; a second call with the same order reverses it.

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPObjectOrder.hpp/.cpp`; `tests/slic3rutils/test_mcp_object_order.cpp`
- Modify: `OrcaMCPArrangeTools.cpp` (registration + handler beside arrange_objects, which shares the plate selection and `object_id_changes`)

**Interfaces:**
- Consumes: `sort_objects(std::vector<ModelObject*>&, ObjectOrder, bool descending)` (GUI_ObjectList.hpp:68);
  `object_order(const Model&)` / `object_id_changes(before, model)` (OrcaMCPUiJob.hpp:152,159); `Plater::SingleSnapshot`.
- Produces:
  - `std::optional<ObjectOrder> object_order_named(const std::string& by);` — `name|filament|height|size`.
  - `bool plate_in_order(std::vector<ModelObject*> plate_objects, ObjectOrder, bool descending);`
  - tool `order_objects {by, descending=false, plate_index? | all_plates?}`, category `Plates`, summary
    `"Sort a plate's objects (print order)"`; answer `{changed, by, descending, plates:[{plate_index, object_ids}], object_id_changes}`.

- [ ] **Step 1: Failing tests** (`[McpObjectOrder]`): names map to the enum and `"weight"` is refused naming the four;
  `plate_in_order` true for objects already ascending by height, false for descending when asked ascending.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.** Handler: refuse while a UI job runs (the shared refusal) and `plate_index` with
  `all_plates`; make `plate_index` current first (as arrange_objects does); close an open toolbar tool
  (`closed_toolbar_tool`); if every plate asked is already in the requested direction → `changed:false`, no snapshot.
  Else inside one `Plater::SingleSnapshot`: call `order_objects`; if the result is not in the requested direction
  (upstream reversed), call it once more. Report `object_id_changes` from the whole model.
- [ ] **Step 4: Run** `"[McpObjectOrder]"` PASS; golden regenerated; counts 113/117.
- [ ] **Step 5: Docs:** reference.md; CLAUDE.md Plates row; `next_steps` builder: after a change on a by-object plate,
  `get_scene_info` "to read the print order" (case in `test_mcp_next_steps.cpp`).
- [ ] **Step 6: Commit** `mcp: order_objects sorts a plate's objects, the by-object print order`.

### Task 5: `print_order` in get_scene_info

Upstream a30ca5e54a and siblings: a by-object plate may print an object later than its list place to avoid a
collision; `Print::sequential_print_clearance_valid(print, nullptr, nullptr, &warning)` decides it and writes each
instance's print position into `ModelInstance::arrange_order`.

**Files:**
- Modify: `OrcaMCPObjectOrder.hpp/.cpp` (pure part), the plate description in `OrcaMCPServer.cpp` / `OrcaMCPCommon.cpp`
  where `layer_gcodes` is added per plate
- Test: `test_mcp_object_order.cpp`; `test_mcp_scene_description.cpp` holds `docs/tools/reference.md`'s example keys

**Interfaces:**
- Produces: `nlohmann::json print_order_json(const std::vector<std::pair<const ModelInstance*, int>>& by_position, const Model&, const std::string& warning);`
  → `[{object_id, instance_id}]` in print order; per plate keys `print_order` (array or null), `print_order_differs`
  (bool or null), `print_order_note` (upstream's warning text or null), `print_order_unknown` (null, or why:
  `"not by object"`, `"not the current plate"`).

- [ ] **Step 1: Failing tests:** `print_order_json` orders by position and maps object pointers to indices; a list
  equal to the object-list order gives `print_order_differs:false`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.** Only the current plate, by object, after `OrcaMCP::apply_pending_update`: run the check on
  its Print, read `arrange_order`. Other plates: `null` with the reason.
- [ ] **Step 4: Update** reference.md's `get_scene_info` example (after its `<!-- get_scene_info example` marker) with
  the four plate keys; run `"[McpSceneDescription]"` — PASS.
- [ ] **Step 5: Commit** `mcp: get_scene_info reports a by-object plate's print order`.

### Task 6: Why a setting is unavailable

Upstream 3573195780 and siblings: `Tab::option_state_registry()` (`OptionStateRegistry.hpp`), keyed by option name
without index, `hidden_reason` / `disabled_reason`.

**Files:**
- Modify: `OrcaMCPConfigValues.hpp/.cpp` (pure part), `OrcaMCPServer.cpp` (get_config_values `keys` form ~1276,
  apply_config answer ~1640)
- Test: `tests/slic3rutils/test_config_values.cpp`

**Interfaces:**
- Produces: `nlohmann::json unavailable_json(const std::optional<std::string>& hidden, const std::optional<std::string>& disabled);`
  → `null` when both empty, else `{hidden: reason|null, disabled: reason|null}` (an empty reason string becomes
  `"no reason given"`).
- get_config_values `keys`: each key's entry gains `unavailable`; apply_config's answer gains `unavailable: [{key, hidden, disabled}]`
  for applied keys a tab restricts, read after `UpdatePresetTabs()`.

- [ ] **Step 1: Failing tests** for `unavailable_json` (both empty → null; disabled only; empty reason text).
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement** and wire (the tab of each key's type: print, filament, printer).
- [ ] **Step 4: Run** `"[ConfigValues]"` PASS; golden regenerated (descriptions mention `unavailable`).
- [ ] **Step 5: Commit** `mcp: get_config_values and apply_config say why a setting is unavailable`.

### Task 7: apply_config refuses an invalid excluded bed area

Upstream c01ac8ff44: `is_valid_bed_exclude_area(const Pointfs&)` (PrintConfig.hpp:2581); the GUI field refuses a
crossing polygon.

**Files:** Modify `OrcaMCPConfigValues.hpp/.cpp`, `OrcaMCPPresetConfigUtils.cpp:~545` (before `set_deserialize`);
test `test_config_values.cpp`.

**Interfaces:** `std::optional<std::string> bed_exclude_area_refusal(const std::string& value_text);` — more than
one point and `!is_valid_bed_exclude_area` → `"the excluded bed area must be one simple polygon with an area: its edges must not cross"`.

- [ ] **Step 1: Failing tests:** a bow-tie `0x0,10x10,10x0,0x10` refused; a square and `""`/one point accepted.
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement**; in ApplyConfig a refusal goes to `rejected` with expected
  `"points x1xy1,x2xy2,... forming a simple polygon"`. **Step 4: Run** PASS.
- [ ] **Step 5: Commit** `mcp: apply_config refuses an excluded bed area whose edges cross`.

### Task 8: set_plate_settings name: the rename's refresh (move CH)

Upstream ab24043937: `Plater::rename_plate` invalidates a slice whose G-code uses `{plate_name}` and re-exports;
our tool calls `plate.set_plate_name` inside its own undo step (`OrcaMCPPlateTools.cpp:192`).

**Files:** Modify `Plater.hpp/.cpp` (`rename_plate`), `OrcaMCPPlateTools.cpp`, CLAUDE.md (probe CH + paragraph).

**Interfaces:** `void Plater::refresh_after_plate_rename(int plate_index, bool slice_was_valid);` — the body of
`rename_plate` after `set_plate_name`, unchanged; `rename_plate` calls it. set_plate_settings answers
`slice_invalidated: true` when the rename invalidated the plate's slice.

- [ ] **Step 1:** Move the refresh; `rename_plate` calls it. Build. **Step 2:** set_plate_settings records
  `is_slice_result_valid()` before the name, calls the refresh after, reports `slice_invalidated`.
- [ ] **Step 3: Verify live** (no unit seam: it needs the Plater): a plate whose filename format uses `{plate_name}`,
  sliced, renamed → `slice_invalidated:true` and `get_slicing_status` shows the re-export.
- [ ] **Step 4:** CLAUDE.md probe `CH ... rename_plate's refresh is inline (a move: 0 = expected, keep ours)` counting
  `refresh_after_plate_rename` upstream. **Step 5: Commit** `mcp: set_plate_settings refreshes after a rename as the plate's own rename does`.

### Task 9: send_to_printer: manual filament change starts without the station

Upstream 6d2869c430; our dialog already follows it (merge). `OrcaMCPPrinterTools.cpp:625` defaults
`use_material_station` to `status.has_material_station`.

**Files:** Modify `OrcaMCPPrintOptions.hpp/.cpp`, `OrcaMCPPrinterTools.cpp:625`; test `test_mcp_print_options.cpp`.

**Interfaces:** `bool default_use_material_station(bool printer_has_station, bool manual_filament_change);` — the
printer preset's `manual_filament_change` (`preset_bundle->printers.get_edited_preset().config.opt_bool(...)`).

- [ ] **Step 1: Failing tests** (`[McpPrintOptions]`): station + manual → false; station alone → true; no station → false.
- [ ] **Step 2: Run** FAIL. **Step 3: Implement** and use it as the default; the schema text says so.
- [ ] **Step 4: Run** PASS; golden regenerated. **Step 5: Commit** `mcp: send_to_printer leaves the material station off for manual filament change`.

### Task 10: Whole-branch check

- [ ] `cmake --build build/arm64 --config Release --target all`; `ctest -C Release -j 12` — 100% pass.
- [ ] The fork-only sources compile with the PCH off (`build/nopch`, the 177+ objects as in the sync).
- [ ] `python3 -m unittest discover -s scripts/tests -t scripts` — OK.
- [ ] `scripts/check-fork-customizations.sh` — intact.
- [ ] Live check on an isolated instance (`--datadir` in the scratchpad, `select_instance` by pid, never the user's
  window): load_model of `tests/data/named_compound_bodies.step` with `import_options {load_as:"separate_objects"}`
  (3 objects); add_volume cube `size {x:10,y:20,z:5}`; add_primitive_object torus; a by-object plate of two objects,
  order_objects `height` descending, get_scene_info `print_order`; apply_config a bow-tie `bed_exclude_area` (rejected);
  quit_app.
- [ ] Commit any doc fix-ups found; report to the user.
