# Painted Modifiers for Agents Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An agent adds, paints, sizes, configures, reads, draws and removes upstream's painted modifiers through the existing tools (`add_volume`, `paint_object`, `get_object_paint`, `clear_object_paint`, `get_object_info`, `render_plate_view`, `delete_object`, `set_volume_type`).

**Architecture:** A fifth paint mode, `PaintMode::PaintedModifier`, whose annotation is the modifier volume's own `painted_modifier_facets`. Its target is the one modifier volume, painted over its own mesh and frame, which after `ModelObject::sync_painted_modifiers()` are exactly its host's; so every selection form, the three-hop write, the undo step and the plate refresh of the other modes apply unchanged. The decisions apart from the app (which volume, which host, depth, refusals, report rows) live in a new `OrcaMCPPaintedModifier.{hpp,cpp}`, unit-tested without the app; the tools call the object list's own `ObjectList::add_painted_modifier(obj_idx, host, nullptr)`.

**Tech Stack:** C++17, nlohmann::json, Catch2 (`tests/slic3rutils`), wxWidgets main-thread gate (`run_on_main_thread`), libslic3r `TriangleSelector` / `FacetsAnnotation`.

**Spec:** `docs/superpowers/specs/2026-11-19-painted-modifiers-mcp-design.md`

**Where it runs:** on the 2026-11-19 sync's code (branch `sync-upstream-2026-11-19`, worktree `../OrcaMCP-sync-1109`), or on `mcp` once that branch is merged into it. `mcp` alone does not have `ModelVolumeType::PAINTED_MODIFIER`.

## Global Constraints

- Every change is one undo step, named as the GUI names it: `Add painted modifier` (ObjectList), `Change painted modifier depth` (the gizmo's Depth field), `Paint Object (painted_modifier)` and `Clear Object Paint (painted_modifier)` (the existing paint tools' pattern). Snapshot names use `_u8L(...)`.
- No undo step and no dirty flag for a call that changes nothing (`changed: false` / `annotation_changed: false`).
- An open toolbar tool is closed right before a change, and only for a change (`close_toolbar_tool_for_change` / `close_toolbar_tool_before_change`), reported as `closed_toolbar_tool`.
- A value a tool cannot act on is refused with `{"status": "error", "message"}` naming what to send instead, before anything changes. Never clamp.
- Painted modifiers go on **model parts only** (upstream's rule): a `host_volume_id` naming any other volume is refused.
- `depth_mm`: a number from 0 to 1000 inclusive (the gizmo's field range, `BBLDragFloat ... 0.f, 1000.f`); 0 = across the whole part to the nearest unpainted surface.
- Paint states for the mode: `none` = 0, `painted` = 1, `enforcer` = 1 (synonym, read back as `painted`); no blocker.
- A tool reads only arguments its schema declares; every new argument is declared (`tools/call` refuses others with -32602). The schema's `enum`s are not enforced, so the handler refuses a bad value itself.
- Every new next step is built in `OrcaMCPNextSteps.cpp`, with a case in `tests/slic3rutils/test_mcp_next_steps.cpp` whose arguments pass `tool_arguments_error`.
- After any schema or text change: regenerate `scripts/orcamcp_tools.json` (CLAUDE.md "Adding New Tools", step 4) and keep `[orcamcp][tools]` green; every tool summary stays one line of at most 40 characters.
- Never run `slic3rutils_tests` unfiltered in one process: run by tag (`"[McpPaint]"`, `"[PartEdits]"`, ...).
- `get_server_info`'s default response stays under 6 KB (names only; nothing to add there).
- Live checks run a dev build on its own data folder via `ORCAMCP_APP_PATH`, never the user's app; check `lsof -iTCP:13618-13627 -sTCP:LISTEN` before launching.

## Review Focus

1. **The modifier's mesh is stale after the host's mesh changed** (repair, reload from disk, replace, texture bake) and nothing has sliced since: the paint must land on the host's current facets, not the old ones. Every capture in this plan calls `ensure_painted_modifiers_synced` first (Task 2); tested in Task 2 ("a stale modifier is brought back onto its host's mesh before it is read").
2. **`selection: connected` seeded from `pick_facet`**: `pick_facet` names the host part's `volume_id`, not the modifier's. In this mode a seed naming the host is taken as the modifier's (same mesh, same facet numbers); tested in Task 4.
3. **A host that is no longer a part** (`set_volume_type` turned it into a modifier): the modifier then prints nothing. `set_volume_type` answers `orphaned_painted_modifiers` with a `delete_object` next step, and `paint_object` refuses with the GUI's `no_host` words; tested in Tasks 2 and 6.
4. **`paint_object` without `selection`** now passes the schema (depth-only calls): every other mode must still refuse it, by the handler, naming the selections; tested in Task 4.
5. **A painted modifier that changes nothing** (painted nowhere, or no settings of its own): every answer that leaves it so carries the next step that fixes it (paint, or `set_object_config`), and `get_object_info` shows `painted: false` / `settings: []`; tested in Tasks 2 and 4.

---

### Task 1: The paint mode, and our write equals the gizmo's

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp`
- Create: `tests/slic3rutils/test_painted_modifier_paint.cpp` (add to `tests/slic3rutils/CMakeLists.txt` beside `test_paint_remap.cpp`)

**Interfaces:**
- Produces: `PaintMode::PaintedModifier` (appended after `Texture`); `parse_paint_mode("painted_modifier")`; `paint_mode_name(PaintMode::PaintedModifier) == "painted_modifier"`; `annotation_for_mode(mv, PaintedModifier)` -> `mv.painted_modifier_facets`; `max_paint_state_for(PaintedModifier) == 1`; `parse_paint_state(PaintedModifier, ...)`: `none` 0, `painted` / `enforcer` 1, `blocker` refused; `paint_state_label(PaintedModifier, 0|1)` -> `"none"` / `"painted"`. `PaintChannel(PaintMode::PaintedModifier)` is a plain channel (no slot).

- [ ] **Step 1: Write the failing tests** (tag `[McpPaint][orcamcp]`)

```cpp
TEST_CASE("mode painted_modifier parses, names itself and reads its two states back", "[McpPaint][orcamcp]")
{
    PaintMode mode{};
    REQUIRE(parse_paint_mode("Painted_Modifier", mode));
    CHECK(mode == PaintMode::PaintedModifier);
    CHECK(std::string(paint_mode_name(mode)) == "painted_modifier");
    CHECK(max_paint_state_for(mode) == 1);
    int s = -1;
    CHECK((parse_paint_state(mode, "painted", s) && s == 1));
    CHECK((parse_paint_state(mode, "enforcer", s) && s == 1));
    CHECK((parse_paint_state(mode, "none", s) && s == 0));
    CHECK_FALSE(parse_paint_state(mode, "blocker", s));
    CHECK(paint_state_label(mode, 1) == "painted");
    CHECK(paint_state_label(mode, 0) == "none");
}

TEST_CASE("mode painted_modifier writes the modifier's own facets and no other annotation", "[McpPaint][orcamcp]")
// cube object (make_cube(20,20,20)), host = volumes[0], pm = object->add_painted_modifier(*host);
// apply_facet_states(*pm, PaintMode::PaintedModifier, states(+X facets = 1, else -1), true) == true;
// CHECK(!pm->painted_modifier_facets.empty()); CHECK(pm->mmu_segmentation_facets.empty());
// CHECK(host->painted_modifier_facets.empty()); CHECK(host->supported_facets.empty());

TEST_CASE("a painted modifier written by paint_object's path equals the gizmo's selector", "[McpPaint][orcamcp]")
// The same +X facets set on a TriangleSelector(pm->mesh()) with set_facet(f, EnforcerBlockerType::ENFORCER)
// for each, serialized: CHECK(pm->painted_modifier_facets.get_data() == selector.serialize()).

TEST_CASE("a cube painted on +X through paint_object's path slices with the modifier's walls on that side only", "[McpPaint][orcamcp]")
// Built as upstream's tests/fff_print/test_painted_modifier.cpp builds its scene (20 mm cube, layer_height 0.2,
// wall_loops 2 on the object), but the facets written by build_paint_write + apply_paint_data (mode PaintedModifier),
// pm->config wall_loops 5, depth 0. At z = 10: the area printed with 5 walls is within 1 mm2 of the side nearest +X
// (upstream's nearest_side_area for the same scene), the rest with 2.
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[McpPaint]"`
Expected: compile error, `PaintedModifier` is not a member of `PaintMode`.

- [ ] **Step 3: Add the mode** in `OrcaMCPPaintModel.{hpp,cpp}`: the enum value, its name in `parse_paint_mode` / `paint_mode_name`, its annotation, its bound and its two state names, each beside the `FuzzySkin` / `Texture` cases they mirror. Update the header comments that say "four" modes. Any `switch` over `PaintMode` elsewhere that the compiler now flags gets the case its neighbours imply (`grep -rn "PaintMode::" src/slic3r/GUI/OrcaMCP`); `UNKNOWN_MODE_MESSAGE` and `resolve_paint_selection`'s message list `painted_modifier` too.

- [ ] **Step 4: Run them to see them pass**

Run: `... slic3rutils_tests "[McpPaint]"` then `"[orcamcp][paint]"` and `"[orcamcp][PaintRemap]"`
Expected: all pass.

- [ ] **Step 5: Commit** — `git commit -m "MCP paint: mode painted_modifier writes a painted modifier's facets as the gizmo does"`

---

### Task 2: Painted-modifier decisions apart from the app, and their next steps

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintedModifier.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintedModifier.cpp` (add both to `src/slic3r/CMakeLists.txt` beside `OrcaMCPPartEdits`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPNextSteps.{hpp,cpp}`
- Test: `tests/slic3rutils/test_painted_modifier_paint.cpp` (same file as Task 1), `tests/slic3rutils/test_mcp_next_steps.cpp`

**Interfaces:**
- Consumes: Task 1's `PaintMode::PaintedModifier`, `read_volume_paint`.
- Produces (namespace `Slic3r::GUI::OrcaMCP`):
  - `std::vector<int> painted_modifier_ids(const ModelObject&)` — every painted modifier's volume_id, ascending.
  - `int host_volume_id(const ModelObject&, const ModelVolume& painted_modifier)` — the host's volume_id, -1 when `painted_modifier_host` is null.
  - `bool ensure_painted_modifiers_synced(ModelObject&)` — `object.sync_painted_modifiers()` when `has_painted_modifiers()`; returns whether anything moved. Main thread; takes no undo step (the app runs the same before every slice and save).
  - `std::optional<std::string> painted_modifier_target_refusal(int object_id, const ModelObject&, std::optional<int> volume_id)` — why a `mode: painted_modifier` call cannot address `volume_id`, else nullopt.
  - `std::optional<std::string> read_depth_mm(const nlohmann::json& params, std::optional<float>& out)` — reads `depth_mm` when present; the refusal text when it is not a number in [0, 1000].
  - `struct PaintedModifierRequest { int host_volume_id; std::optional<float> depth_mm; }` and `std::variant<PaintedModifierRequest, std::string> read_painted_modifier_request(int object_id, const ModelObject&, const nlohmann::json& params)` — `add_volume`'s `type: painted_modifier` arguments, or the refusal.
  - `std::optional<std::string> painted_modifier_argument_refusal(const std::string& type_name, const nlohmann::json& params)` — `host_volume_id` / `depth_mm` given with another `type`.
  - `nlohmann::json painted_modifier_row_fields(const ModelObject&, std::size_t volume_idx)` — `{}` for any other volume; else `{host_volume_id (null when gone), depth_mm, painted (bool, !facets.empty()), settings: config keys}`. No selector built.
  - `nlohmann::json painted_modifier_coverage_json(const ModelVolume&)` — `{painted_facets, coverage_percent}` of state 1, from `read_volume_paint` (builds a selector: only where asked for).
  - `std::vector<int> painted_modifiers_orphaned_by_type_change(const ModelObject&, int volume_id, ModelVolumeType to)` — the painted modifiers on `volume_id` when `to` is not `MODEL_PART`, else none.
  - Next steps: `painted_modifier_paint_next_steps(int object_id, int volume_id, bool painted, bool has_settings)`, `add_painted_modifier_next_steps(int object_id, int host_volume_id)`, `orphaned_painted_modifier_next_steps(int object_id, const std::vector<int>& ids)`.

- [ ] **Step 1: Write the failing tests** (tag `[McpPaint][orcamcp]`; cube object with `host = volumes[0]`, a second part, a mesh modifier, and `pm = add_painted_modifier(*host)`; ids read from `object.volumes` after `sort_volumes`)

```cpp
TEST_CASE("painted modifier refusals name what to send", "[McpPaint][orcamcp]")
// painted_modifier_target_refusal(0, object, std::nullopt) mentions "volume_id" and lists pm's id;
//   on an object with no painted modifier it mentions "add_volume" and "painted_modifier";
// a model part's id: mentions "is a part, not a painted modifier"; out of range: the volume range;
// pm after its host's type set to PARAMETER_MODIFIER: mentions "has been removed" (the gizmo's no_host words:
//   "The part this modifier was painted on has been removed.") and "delete_object".
// pm on its host: nullopt.

TEST_CASE("add_volume's painted_modifier arguments default as the object list's menu does", "[McpPaint][orcamcp]")
// {} -> host = the first model part, depth nullopt; {host_volume_id: second part} -> it;
// host_volume_id naming the mesh modifier -> refusal mentions "model part"; out of range -> refusal;
// depth_mm -1, 1000.5, "3" -> refusal mentions "0 to 1000"; depth_mm 0 and 1000 accepted;
// shape, size or file_path present -> refusal mentions "has no shape of its own";
// painted_modifier_argument_refusal("modifier", {host_volume_id: 0}) mentions "type painted_modifier".

TEST_CASE("a painted modifier's row says what it does and its coverage is opt-in", "[McpPaint][orcamcp]")
// fresh pm: row fields {host_volume_id: host id, depth_mm: 0, painted: false, settings: []};
// after painting +X and setting wall_loops: painted true, settings ["wall_loops"];
// coverage json: painted_facets > 0, coverage_percent within 0.5 of 100/6; the host's row fields == {}.

TEST_CASE("a stale modifier is brought back onto its host's mesh before it is read", "[McpPaint][orcamcp]")
// pm painted on +X; host->set_mesh(a new TriangleMesh built from the same cube): a new mesh pointer, as a repair leaves;
// CHECK(pm->mesh_ptr() != host->mesh_ptr()); CHECK(ensure_painted_modifiers_synced(object));
// CHECK(pm->mesh_ptr() == host->mesh_ptr()); CHECK_FALSE(ensure_painted_modifiers_synced(object)).

TEST_CASE("turning a host into anything but a part leaves its painted modifiers without one", "[McpPaint][orcamcp]")
// painted_modifiers_orphaned_by_type_change(object, host id, PARAMETER_MODIFIER) == {pm id};
// (..., MODEL_PART) empty; on the second part (no painted modifier) empty.
```

In `test_mcp_next_steps.cpp`, one case per builder (tag `[McpNextSteps]`), each step checked with the file's existing helper that its tool is real and its arguments pass `tool_arguments_error`:
- `painted_modifier_paint_next_steps(0, 2, false, false)`: `paint_object` with `{object_id: 0, volume_id: 2, mode: "painted_modifier", selection: "visible", view: {direction: "front"}, state: "painted"}`, then `set_object_config` `{object_id: 0, volume_id: 2}`; `(0, 2, true, true)` empty.
- `add_painted_modifier_next_steps(0, 1)`: `add_volume` `{object_id: 0, type: "painted_modifier", host_volume_id: 1}`.
- `orphaned_painted_modifier_next_steps(0, {3, 4})`: one `delete_object` `{object_id: 0, volume_id: 3}` whose `why` names 3 and 4.

- [ ] **Step 2: Run them to see them fail** — `... slic3rutils_tests "[McpPaint]"` and `"[McpNextSteps]"`: compile errors.

- [ ] **Step 3: Implement** the functions above. Refusal words: start with `Object N` / `volume N` as `OrcaMCPPartEdits` does (`object_text`, `volume_text`), and name the call that fixes it. The `why` of the paint step says to replace the selection with the faces to change (box, sphere, `connected` from a `pick_facet`, `visible` from a view).

- [ ] **Step 4: Run them to see them pass** — `"[McpPaint]"`, `"[McpNextSteps]"`, `"[PartEdits]"`.

- [ ] **Step 5: Commit** — `"MCP: painted modifier decisions apart from the app, and their next steps"`

---

### Task 3: `add_volume` with `type: "painted_modifier"`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPartTools.cpp` (`add_volume_on_main_thread`, its registration), `src/slic3r/GUI/OrcaMCP/OrcaMCPPartEdits.{hpp,cpp}` (`add_volume_type_names()`)
- Test: `tests/slic3rutils/test_part_edits.cpp`, golden file

**Interfaces:**
- Consumes: Task 2's `read_painted_modifier_request`, `painted_modifier_argument_refusal`, `painted_modifier_row_fields`, `painted_modifier_paint_next_steps`.
- Produces: `const std::vector<std::string>& add_volume_type_names()` = `volume_type_names()` + `"painted_modifier"`. `volume_type_names()` and `volume_type_from_name` stay as they are, so `set_volume_type` keeps refusing `painted_modifier` as a target.

- [ ] **Step 1: Write the failing test** (`[PartEdits][orcamcp]`): `add_volume_type_names()` equals `volume_type_names()` plus `"painted_modifier"` last, and the existing assertion on `volume_type_names()` is unchanged.

- [ ] **Step 2: Run it to see it fail.**

- [ ] **Step 3: Implement the branch.** In `add_volume_on_main_thread`, right after the object is resolved: `painted_modifier_argument_refusal` for every type; for `type_name == "painted_modifier"` call a new `add_painted_modifier_on_main_thread(plater, object_id, object, params)` and return. It: reads the request (Task 2); `ui_job_refusal`; `close_toolbar_tool_before_change`; inside one `Plater::TakeSnapshot(plater, _u8L("Add painted modifier"))` calls `wxGetApp().obj_list()->add_painted_modifier(object_id, *host, nullptr)` and, when given, sets `painted_modifier_depth`; finds the new volume's id by pointer **after** the call (the list re-sorts volumes); answers `{status, object_id, volume: volume_row_json (with Task 6's fields once it lands), volumes, host_volume_id, depth_mm, painted_facets: 0}`, `active_warnings`, `next_steps` = `painted_modifier_paint_next_steps(object_id, id, false, false)`. The registration's `type` enum becomes `add_volume_type_names()`, with `host_volume_id` (integer, minimum 0) and `depth_mm` (number, 0 to 1000) declared and described; the description says the type has no shape, size or file.

- [ ] **Step 4: Regenerate the golden file and run** `"[PartEdits]"`, `"[orcamcp][tools]"`, `"[McpToolArguments]"`.

- [ ] **Step 5: Commit** — `"add_volume: type painted_modifier, as the object list's Add Painted Modifier"`

---

### Task 4: `paint_object` with `mode: "painted_modifier"`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp`
- Test: `tests/slic3rutils/test_painted_modifier_paint.cpp`, golden file

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces: `resolve_paint_target(params, out, error, needs, const PaintChannel& channel = PaintMode::Color)` — for `PaintedModifier`, the target is exactly the one painted modifier `volume_id` names (`painted_modifier_target_refusal` first), after `ensure_painted_modifiers_synced(*object)`. All ten callers keep their behaviour; the paint_object hops, `paint_by_state` / `run_paint_remap`, `get_object_paint` and `clear_object_paint` pass the channel they parsed (parse it before resolving).
- Produces, in `OrcaMCPPaintedModifier.{hpp,cpp}` (pure, tested):
  - `std::optional<std::string> missing_selection_refusal(const PaintChannel&, bool has_selection, bool has_depth)` — nullopt when a selection is given, or for `PaintedModifier` with `depth_mm`; else the message listing every selection (the one `parse_paint_request` gives for an unknown one).
  - `std::optional<std::string> depth_argument_refusal(const PaintChannel&, const nlohmann::json& params)` — `depth_mm` given with any other mode, else `read_depth_mm`'s refusal, else nullopt.
  - `int seed_volume_for_mode(const ModelObject&, const PaintChannel&, int painted_modifier_id, int seed_volume_id)` — `painted_modifier_id` when the channel is `PaintedModifier` and `seed_volume_id` is that modifier's host, else `seed_volume_id`.

- [ ] **Step 1: Write the failing tests** (`[McpPaint][orcamcp]`):
  - `missing_selection_refusal`: Color without selection -> message mentions `bands, box, sphere, all, visible, connected, component or state`; PaintedModifier with depth and no selection -> nullopt; PaintedModifier without either -> the message.
  - `depth_argument_refusal`: `depth_mm` with mode `support` -> mentions `mode painted_modifier`; with `PaintedModifier`, -1 -> mentions `0 to 1000`; 3 -> nullopt.
  - `seed_volume_for_mode`: the host's id -> the modifier's; another part's id -> unchanged; mode Color -> unchanged.

- [ ] **Step 2: Run them to see them fail.**

- [ ] **Step 3: Implement in the tool:**
  - Schema: `mode` enum gains `painted_modifier`; `depth_mm` declared (number, 0–1000, "mode painted_modifier only; with or without a selection"); `selection` leaves `required` (the handler refuses its absence through `missing_selection_refusal`, before any hop); `volume_id`'s description says it is required for this mode and names a painted modifier; `state`'s description lists `painted` / `none` for it.
  - Depth only (no selection): one main-thread hop: resolve, then when the depth differs, close the toolbar tool for the change, `Plater::TakeSnapshot(_u8L("Change painted modifier depth"))`, set it, `refresh_after_paint(target)`; answer `{status, object_id, volume_id, host_volume_id, depth_mm, depth_changed, annotation_changed: false, painted_facets, coverage_percent}` (Task 2's coverage), `next_steps` from `painted_modifier_paint_next_steps`, and an `info_messages` line when it is painted nowhere ("depth changes nothing until the modifier is painted").
  - With a selection: the three hops unchanged; hop 1 maps a facet seed through `seed_volume_for_mode`; hop 3 counts a depth change as a change (snapshot taken when facets **or** depth change), sets it under the same `Paint Object (painted_modifier)` step; the answer adds `volume_id`, `host_volume_id`, `depth_mm`, and `next_steps` from `painted_modifier_paint_next_steps(object_id, id, painted_after, !config.empty())` in place of `paint_prerequisite_steps`.
  - `selection: "state"` works through `paint_by_state` with the channel-aware resolver; `remap_paint` keeps refusing the mode (its message lists `painted_modifier` with support and seam).

- [ ] **Step 4: Regenerate the golden file; run** `"[McpPaint]"`, `"[orcamcp][paint]"`, `"[orcamcp][tools]"`, `"[McpToolArguments]"`.

- [ ] **Step 5: Commit** — `"paint_object: mode painted_modifier, every selection over the host's surface, with depth"`

---

### Task 5: `get_object_paint` and `clear_object_paint` with the mode

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.cpp` (`resolve_paint_selection`: the mode is a named mode of its own, never in the no-mode default)
- Test: `tests/slic3rutils/test_painted_modifier_paint.cpp`

**Interfaces:**
- Consumes: Tasks 1, 2, 4 (channel-aware resolver).
- Behaviour: with `mode: painted_modifier`, `volume_id` is optional: one painted modifier, or every one of the object (none: success with `volumes: []` and the `add_volume` next step from `add_painted_modifier_next_steps`). Each volume entry adds `host_volume_id`, `depth_mm`, `painted_facets`, `coverage_percent` (of the host's surface), `settings`. Without `mode`, both tools behave as today: painted-modifier areas are neither read nor cleared (as texture masks are not cleared). Clear takes one snapshot `Clear Object Paint (painted_modifier)`, resets facets only, keeps the volumes and their depth.

- [ ] **Step 1: Write the failing test**: `resolve_paint_selection` with `{mode: "painted_modifier"}` gives `modes == {PaintedModifier}`, `named_mode`, label `painted_modifier`; without mode, `modes` is still the four print-paint modes.
- [ ] **Step 2: Run it to see it fail.**
- [ ] **Step 3: Implement** the resolver and the two tools' branches; schema enums and descriptions gain the mode.
- [ ] **Step 4: Golden file; run** `"[McpPaint]"`, `"[McpTexture]"`, `"[orcamcp][tools]"`.
- [ ] **Step 5: Commit** — `"get_object_paint and clear_object_paint read and clear painted modifiers"`

---

### Task 6: Volume rows, orphans on a type change, deleting one

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPartEdits.cpp` (`volume_rows_json`), `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` (get_object_info, ~line 5694), `src/slic3r/GUI/OrcaMCP/OrcaMCPPartTools.cpp` (set_volume_type)
- Test: `tests/slic3rutils/test_part_edits.cpp`

**Interfaces:**
- Consumes: Task 2's `painted_modifier_row_fields`, `painted_modifier_coverage_json`, `painted_modifiers_orphaned_by_type_change`, `orphaned_painted_modifier_next_steps`.
- Produces: `nlohmann::json volume_rows_json(const ModelObject&, bool with_paint_coverage = false)` — every row of a painted modifier carries the row fields; with the flag also the coverage. `get_object_info` passes `true`; every other caller (13) keeps the cheap rows.

- [ ] **Step 1: Write the failing tests** (`[PartEdits][orcamcp]`): a painted modifier's row has `type: "painted_modifier"`, `host_volume_id`, `depth_mm`, `painted`, `settings` and no `painted_facets`; with the flag it has `painted_facets` and `coverage_percent`; a part's row has none of these keys; `volume_delete_refusal` on a painted modifier is nullopt (deleting one is the object list's own Delete).
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.** In set_volume_type, read `painted_modifiers_orphaned_by_type_change` (as volume pointers) before the change; after it, report their new ids as `orphaned_painted_modifiers`, an `info_messages` line saying they print nothing without a part to paint on, and `orphaned_painted_modifier_next_steps`. `delete_object` with a painted modifier's `volume_id` needs no code: its `deleted_volume` row carries the fields (verified live in Task 8).
- [ ] **Step 4: Run** `"[PartEdits]"`, `"[McpSceneDescription]"` (get_scene_info's example must not change), `"[McpNextSteps]"`.
- [ ] **Step 5: Commit** — `"Volume rows describe painted modifiers; a host's type change reports the ones it leaves without a part"`

---

### Task 7: `render_plate_view` `paint: ["painted_modifier"]`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPRenderPaint.{hpp,cpp}`, `src/slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.cpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPRenderSchema.cpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.cpp`
- Test: `tests/slic3rutils/test_render_paint.cpp`, `tests/slic3rutils/test_render_request.cpp`

**Interfaces:**
- Consumes: Task 1's mode; Task 2's `host_volume_id`, `ensure_painted_modifiers_synced`.
- Produces:
  - `paint_channel_names()` gains `"painted_modifier"` (last).
  - `struct PaintedModifierRef { int object_id; int volume_id; int host_volume_id; std::vector<std::string> settings; }`.
  - `PaintTint` gains `int object_id = -1, volume_id = -1, host_volume_id = -1; std::vector<std::string> settings;` (painted_modifier only).
  - `paint_tints(channels, texture_slots, slots_with_layers, const std::vector<PaintedModifierRef>& painted_modifiers = {})` — one tint per painted modifier, in the given order, colour `texture_layer_color(i % 8)`, label `"painted modifier <volume_id> of object <object_id>"`.
  - `paint_channel_of(tint)` -> `PaintMode::PaintedModifier` for that channel.
  - `PaintVolumeInput` and `PaintFacets` gain `int draw_volume_id = -1` (the GLVolume the facets are drawn on: the host for a painted modifier, else `volume_id`).
  - `tint_legend_json`: a painted-modifier tint adds `object_id`, `volume_id`, `host_volume_id`, `settings`.

- [ ] **Step 1: Write the failing tests:** `[RenderPaint]`: two refs give two tints in order with distinct colours and their ids in the legend; `compute_paint_facets` carries `draw_volume_id` through to `PaintFacets`. `[RenderRequest]`: `paint: ["painted_modifier"]` is read; `["painted_modifier", "texture"]` together is refused ("they share colours: ask them in separate pictures"); with `layer_view` refused as the other channels are.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.** Capture (`OrcaMCPPlateUtils.cpp`, the loop around line 480): when the channel is asked, `ensure_painted_modifiers_synced` each object on the plate, collect its painted modifiers whose host is found into the refs, and for each make one input from the painted modifier volume (its data, its mesh, `volume_id` = its id, `draw_volume_id` = host id). The draw (around line 1127) matches `f.draw_volume_id` against the GLVolume's `composite_id.volume_id`. `paint_unchanged_since` also checks the host's `mesh_ptr()` still equals the input's mesh. A painted modifier with no host is skipped and named in `tint_notes`. Schema description: `painted_modifier` tints each painted modifier's area on its part, one colour each, legend per modifier with its settings.
- [ ] **Step 4: Golden file; run** `"[RenderPaint]"`, `"[RenderRequest]"`, `"[orcamcp][tools]"`.
- [ ] **Step 5: Commit** — `"render_plate_view: paint painted_modifier tints each painted modifier's area on its part"`

---

### Task 8: Docs, CLAUDE.md, and the live check

**Files:**
- Modify: `CLAUDE.md` (tool table rows: Models `add_volume`, `set_volume_type`; Painting `paint_object`, `get_object_paint`, `clear_object_paint`; Visualization `render_plate_view`), `docs/tools/reference.md` (each tool's new arguments and answer fields, "Next Steps" table), `docs/tools/workflows.md` (new "Painted Modifier Workflow: Strengthen Part of a Part" after "Parts Workflow: A Modifier", in that section's style, with the call sequence below)

- [ ] **Step 1: Write the docs.** The workflow: `add_volume {type: painted_modifier}` -> `pick_facet` or a box -> `paint_object {mode: painted_modifier, volume_id, selection, depth_mm}` -> `set_object_config {volume_id, settings: [{key: wall_loops, value: 6}]}` -> `render_plate_view {paint: [painted_modifier]}` -> `slice_all` / `wait_for_slice` -> `render_plate_view {layer_view: {layer: 5}}`.
- [ ] **Step 2: Build the app** (`cmake --build build/arm64 --config RelWithDebInfo --target all --`) and run the full tag set once: `"[McpPaint]"`, `"[PartEdits]"`, `"[McpNextSteps]"`, `"[RenderPaint]"`, `"[RenderRequest]"`, `"[orcamcp][tools]"`, `"[McpToolArguments]"`, `"[McpSceneDescription]"`, `"[McpTexture]"`.
- [ ] **Step 3: Live check** (dev build, own data folder via `ORCAMCP_APP_PATH`, through the `mcp__orca-slicer__*` tools, never curl), the spec's sequence: `add_primitive_object {shape: cube}`; `add_volume {type: painted_modifier}`; `paint_object {mode: painted_modifier, volume_id, selection: box, box: one face}`; `set_object_config` `wall_loops: 6` on that volume; `render_plate_view {paint: [painted_modifier]}` (the face tinted, legend lists the modifier with `wall_loops`); `slice_all`; `wait_for_slice`; `render_plate_view {layer_view: {layer: 5}}`: six walls on the painted face, the default elsewhere. Then: `paint_object` depth only (`depth_mm: 3`), `get_object_info` (row fields with coverage), `undo` back to the plain cube (one step per change), `redo`, `export_3mf` then `load_project`: the modifier, its area, depth and settings are back. Also: `pick_facet` on the face, then `selection: connected` seeded with its host volume_id; `set_volume_type` of the host to `modifier` on a copy reports `orphaned_painted_modifiers`, and `paint_object` on the orphan is refused; `delete_object` with the modifier's `volume_id` removes it in one step.
- [ ] **Step 4: Commit** — `"Docs: painted modifiers for agents"` (`[skip ci]` only if the commit holds docs alone).

---

## Notes against the spec

- **Spec Design 6, deleting a host:** the spec says the GUI keeps a deleted host's painted modifiers as orphans. Upstream's object list deletes them with the host (`GUI_ObjectList.cpp`, "Delete part"), and the sync already reports them as `deleted_painted_modifiers`. Orphans arise instead when a host's type changes; Task 6 reports those.
- **Spec Design 2, depth alone:** `paint_object`'s `selection` leaves the schema's `required` list so a depth-only call is possible; the handler refuses a missing selection for every other mode (Review Focus 4).
- **Spec Design 4, colours:** the GUI draws every painted modifier in the enforcer colour; the render gives each its own colour so the legend can tell them apart, and refuses `texture` in the same picture, whose layers use the same palette.
- **Spec Tests, slice check:** upstream's `tests/fff_print/test_painted_modifier.cpp` already proves the slicer honours a selector's data; Task 1 proves our write is that data and slices one cube through our path.
