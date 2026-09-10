# Batch 2, Plan 3 — Mesh editing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give MCP the four mesh-editing capabilities the GUI has and the API does not — `split_object` (to objects and to parts), an upgraded `cut_object` (arbitrary plane, keep-both/keep-as-parts, connectors, tongue-and-groove), `boolean_object`, and `simplify_object` — each of them reporting honestly what happened to the object's paint.

**Architecture:** A new tool group file `OrcaMCPMeshTools.cpp` registers all four tools through `OrcaMCPServer::register_mesh_tools()`, following the `OrcaMCPFilamentTools.cpp` / `OrcaMCPPrinterTools.cpp` pattern. The geometry and classification arithmetic that does not need a GUI lives in a new `OrcaMCPMeshUtils.hpp/.cpp` free-function pair with Catch2 coverage in `tests/slic3rutils/test_mesh_edit.cpp`. The tool handlers themselves drive libslic3r directly (`ModelObject::split`, `ModelVolume::split`, `Cut`, `MeshBoolean::mcut::make_boolean`, `its_quadric_edge_collapse`) rather than opening a gizmo, because a gizmo needs a canvas selection and an ImGui frame that an HTTP worker does not have.

**Tech Stack:** C++17, nlohmann::json, wxWidgets (main-thread marshalling only), Catch2 v3, CMake. libslic3r: `Model.hpp`, `CutUtils.hpp`, `MeshBoolean.hpp`, `QuadricEdgeCollapse.hpp`, `Geometry.hpp`.

**Spec:** docs/superpowers/plans/2026-09-10-batch-2-spec.md

---

## Global Constraints

Every task inherits these. The first eleven are copied from the spec's own Global
Constraints section; the rest are this plan's, established by reading the code.

- **Branch:** `sync-upstream-2.5`. Do not push. Do not create branches.
- **Commit style:** `fix:` / `feat:` + what changed, past tense. The body names the
  **root cause**, not the symptom, and lists related occurrences checked — *including
  the ones deliberately left alone, and why*. Read `git log -5` for the bar. Every
  commit ends with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
- **Threading:** every tool handler body runs inside `run_on_main_thread()`
  (`src/slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp:18`). Anything touching `Model`, `Plater`
  or the preset bundle must be inside it. A modal opened inside it hangs the GUI forever.
- **Dialogs:** never call `set_mcp_dialog_suppression()` directly.
  `McpDialogSuppressionGuard` (`OrcaMCPCommon.hpp:57`, RAII, nest-safe) is the only
  sanctioned way.
- **Pure logic goes in a free function with Catch2 coverage** under
  `tests/slic3rutils/`. Geometry maths must be testable without a GUI.
- **Undo/redo:** any tool that mutates the model must take a snapshot so `undo` works.
  `plater->take_snapshot(std::string)` (`Plater.hpp:617`), called **before** the first
  mutation. `set_object_printable` (`OrcaMCPServer.cpp:4076-4080`) is the in-repo example.
- **Registration:** tools are registered with `register_tool({...})` in
  `register_builtin_tools()` (`OrcaMCPServer.cpp`) or the per-area registrars. New tool
  groups get their own file following that pattern, added to `src/slic3r/CMakeLists.txt`.
- **Docs:** every new or changed tool updates `docs/tools/reference.md`, and the tool
  table plus the count in `CLAUDE.md`. The count command is in CLAUDE.md.
- **Response shape:** `{"status": "success"|"error"|"partial", ...}` plus
  `active_warnings` on anything that mutates the scene, matching existing tools.
- **Verify before reporting:**
  ```
  cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
  build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
  ```
  Baseline at the time of writing: **174 cases / 1195 assertions / 0 failures**. The
  skipped count varies run to run (bundled-Python and numpy cases) — that is expected,
  not a regression.
- **Do not contact the printer.** Nothing in this plan needs one.

This plan's own constraints:

- **Starting the app is allowed and required** for the tool tasks (Tasks 3, 4, 5, 6, 7,
  8, 9, 10, 11, 12, 13). The recipe, used verbatim wherever a task says "rebuild and
  reload":
  ```bash
  cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
      --target OrcaSlicer slic3rutils_tests -- -j8
  pkill -f 'RelWithDebInfo/OrcaSlicer.app' || true
  open -a /Users/hanan/Projects/OrcaMCP/build/arm64/src/RelWithDebInfo/OrcaSlicer.app
  ```
  Wait for the app window, then drive it with the `mcp__orca-slicer__*` tools.
- **Test the MCP interface with the MCP tools, never `curl`** (CLAUDE.md, Testing
  Guidelines). Direct HTTP bypasses the bridge and does not validate the integration.
- **Test model:** `resources/calib/pressure_advance/tower_with_seam.drc` ships in the
  repo and is a single connected body — good for cut, boolean and simplify. For split,
  use a multi-body file; if none is to hand, produce one by loading the tower twice and
  running `boolean_object` with `operation: "union"` once Task 11 lands, or use any
  multi-body STL the user names. A task that needs a multi-body model says so.
- **`keep_paint` defaults to `false` everywhere in this plan.** That is OrcaSlicer's own
  shipped default: the GUI reads `app_config->get_bool("keep_painting")`
  (`Plater.cpp:10177`, `GUI_ObjectList.cpp:2910`, `GLGizmoCut.cpp:3555`,
  `GLGizmoSimplify.cpp:539`), `AppConfig::get_bool` returns false for a key that was
  never set (`AppConfig.hpp:144-147`), and `AppConfig.cpp` sets no default for
  `keep_painting`. Preferences describes the remap as "Highly experimental! Slow and may
  create artifact." (`Preferences.cpp:2161`). MCP does not read the app config for this —
  an agent cannot see a GUI preference, so the tool's behaviour must not depend on one.
- **Paint must never be lost silently.** Every tool in this plan returns a `paint` object
  `{"had_paint": bool, "keep_paint": bool, "result": "none"|"remapped"|"discarded"}` and,
  when `result` is `"discarded"` or `"remapped"`, pushes a matching line into
  `info_messages`.
- **`GLGizmoAdvancedCut.cpp/.hpp` is dead code.** It is not in `src/slic3r/CMakeLists.txt`
  (the compiled cut gizmo is `GLGizmoCut.cpp`, listed at `CMakeLists.txt:165-166`) and it
  no longer compiles — it references `ModelObjectCutAttribute::CutToParts` and
  `ModelObject::get_connector_mesh`, neither of which exists. Read `GLGizmoCut.cpp` for
  cut behaviour and never copy from `GLGizmoAdvancedCut.cpp`.
- **`Plater::split_object(int obj_idx, bool auto_drop)` is declared
  (`Plater.hpp:737`) but never defined** — only the selection-driven
  `Plater::split_object(bool)` exists (`Plater.cpp:20592`). Calling the int overload
  fails to link. This plan does not call either.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp` (create) | Declarations for the GUI-free mesh-editing arithmetic: cut-plane matrix, paint-result classification, volume type naming, boolean op mapping, decimation target count. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp` (create) | Their implementations. No wxWidgets, no `Plater`. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` (create) | `OrcaMCPServer::register_mesh_tools()` — `split_object`, `cut_object` (moved here and extended), `boolean_object`, `simplify_object`. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp` (modify) | Declare `static void register_mesh_tools();` beside the other registrars. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` (modify) | Call `register_mesh_tools()`; delete the old `cut_object` registration; add `parts` to `get_object_info`; add the new tools to `get_server_info`'s catalogue. |
| `src/slic3r/CMakeLists.txt` (modify, after line 426) | Add the three new source files to `libslic3r_gui`. |
| `tests/slic3rutils/test_mesh_edit.cpp` (create) | Catch2 coverage for everything in `OrcaMCPMeshUtils`. |
| `tests/slic3rutils/CMakeLists.txt` (modify) | Add `test_mesh_edit.cpp` to the test binary. |
| `docs/tools/reference.md` (modify) | Tool entries; the quick-reference table at line 14. |
| `docs/tools/workflows.md` (modify) | One multi-tool pattern: split to parts, then colour each part. |
| `CLAUDE.md` (modify) | Tool table row and the registered/reachable counts. |

`cut_object` moves out of `OrcaMCPServer.cpp` (currently 4338 lines) into the new file so
the four mesh-editing tools live together and the extension diffs stay readable.

---

## Task 1: Mesh utils module — cut-plane matrix and paint-result classification

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp`
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp`
- Create: `tests/slic3rutils/test_mesh_edit.cpp`
- Modify: `src/slic3r/CMakeLists.txt:426` (after `GUI/OrcaMCP/MCPClientConfig.cpp`)
- Modify: `tests/slic3rutils/CMakeLists.txt` (the `add_executable` source list)

**Interfaces:**
- Consumes: nothing from earlier tasks. `Slic3r::Geometry::translation_transform`
  (`Geometry.hpp:372`), `Slic3r::Geometry::rotation_from_two_vectors`
  (`Geometry.hpp:405`).
- Produces:
  ```cpp
  namespace Slic3r { namespace GUI { namespace OrcaMCP {
  bool cut_plane_matrix(const Vec3d& plane_origin, const Vec3d& plane_normal,
                        const Vec3d& instance_offset, Transform3d& out);
  enum class PaintResult { None, Remapped, Discarded };
  PaintResult classify_paint_result(bool had_paint, bool keep_paint);
  std::string paint_result_message(PaintResult result, const std::string& operation);
  }}}
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/slic3rutils/test_mesh_edit.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp"

// The GUI-free half of the mesh-editing tools. Cut's matrix and the paint bookkeeping are
// arithmetic, so they are tested here rather than by driving a running slicer.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

TEST_CASE("cut_plane_matrix reproduces the horizontal cut the published tool already does",
          "[orcamcp][mesh]")
{
    // cut_object's current contract: a Z plane at z_height, expressed relative to the
    // instance offset (OrcaMCPServer.cpp, cut_object handler).
    Transform3d m;
    REQUIRE(cut_plane_matrix(Vec3d(0., 0., 30.), Vec3d::UnitZ(), Vec3d(120., 120., 0.), m));

    const Vec3d translation = m.translation();
    CHECK_THAT(translation.x(), WithinAbs(-120., 1e-9));
    CHECK_THAT(translation.y(), WithinAbs(-120., 1e-9));
    CHECK_THAT(translation.z(), WithinAbs(30., 1e-9));

    // No rotation for a horizontal cut.
    const Vec3d mapped = m.linear() * Vec3d::UnitZ();
    CHECK_THAT(mapped.z(), WithinAbs(1., 1e-9));
}

TEST_CASE("cut_plane_matrix rotates +Z onto an arbitrary normal", "[orcamcp][mesh]")
{
    Transform3d m;
    REQUIRE(cut_plane_matrix(Vec3d(10., 10., 5.), Vec3d(1., 0., 1.), Vec3d::Zero(), m));

    // GLGizmoCut3D::update_clipper (GLGizmoCut.cpp:451-453) defines the cut normal as
    // (rotation * UnitZ()).normalized(); the matrix must satisfy that for the requested normal.
    const Vec3d n = (m.linear() * Vec3d::UnitZ()).normalized();
    const Vec3d expected = Vec3d(1., 0., 1.).normalized();
    CHECK_THAT((n - expected).norm(), WithinAbs(0., 1e-9));
}

TEST_CASE("cut_plane_matrix does not care whether the normal is normalised", "[orcamcp][mesh]")
{
    Transform3d unit, scaled;
    REQUIRE(cut_plane_matrix(Vec3d::Zero(), Vec3d(0., 1., 0.), Vec3d::Zero(), unit));
    REQUIRE(cut_plane_matrix(Vec3d::Zero(), Vec3d(0., 7.5, 0.), Vec3d::Zero(), scaled));
    CHECK_THAT((unit.matrix() - scaled.matrix()).norm(), WithinAbs(0., 1e-9));
}

TEST_CASE("cut_plane_matrix refuses a normal that names no plane", "[orcamcp][mesh]")
{
    Transform3d m = Transform3d::Identity();
    CHECK_FALSE(cut_plane_matrix(Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero(), m));
    CHECK_FALSE(cut_plane_matrix(Vec3d::Zero(), Vec3d(1e-9, 0., 0.), Vec3d::Zero(), m));
    // Rejected means untouched, so a caller that ignores the return value gets identity,
    // not a half-built matrix.
    CHECK_THAT((m.matrix() - Transform3d::Identity().matrix()).norm(), WithinAbs(0., 1e-12));
}

TEST_CASE("classify_paint_result separates 'nothing to lose' from 'dropped it'",
          "[orcamcp][mesh]")
{
    CHECK(classify_paint_result(false, false) == PaintResult::None);
    CHECK(classify_paint_result(false, true)  == PaintResult::None);
    CHECK(classify_paint_result(true,  true)  == PaintResult::Remapped);
    CHECK(classify_paint_result(true,  false) == PaintResult::Discarded);
}

TEST_CASE("paint_result_message speaks only when there is something to report",
          "[orcamcp][mesh]")
{
    CHECK(paint_result_message(PaintResult::None, "split_object").empty());
    CHECK(paint_result_message(PaintResult::Discarded, "split_object") ==
          "split_object discarded this object's painted facets (colour/support/seam/fuzzy skin). "
          "Pass keep_paint: true to remap them onto the new meshes instead.");
    CHECK(paint_result_message(PaintResult::Remapped, "split_object") ==
          "split_object remapped this object's painted facets onto the new meshes. "
          "The remap is approximate; verify the result before printing.");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Add the file to the test binary first — `tests/slic3rutils/CMakeLists.txt`, in the
`add_executable` list, after `test_material_mapping.cpp`:

```cmake
    test_material_mapping.cpp
    test_mesh_edit.cpp
```

Run:
```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
```
Expected: FAIL — `'slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp' file not found`.

- [ ] **Step 3: Write the header**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp
#pragma once
#include <string>

#include "libslic3r/Point.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// The cut plane an agent describes -- a point it passes through and the normal it faces --
// turned into the matrix Cut wants. Cut's matrix is relative to the instance offset and its
// rotation is what maps +Z onto the cut normal: see GLGizmoCut3D::get_cut_matrix
// (GLGizmoCut.cpp:3444-3459) and GLGizmoCut3D::update_clipper (GLGizmoCut.cpp:449-453).
// `plane_origin` and `instance_offset` are in plate coordinates, the space get_object_info
// reports. The normal need not be normalised.
// Returns false and leaves `out` untouched when the normal names no plane (length < 1e-6,
// or not finite).
bool cut_plane_matrix(const Vec3d& plane_origin,
                      const Vec3d& plane_normal,
                      const Vec3d& instance_offset,
                      Transform3d& out);

// What happened to a volume's painted facets across a mesh-changing operation.
// None      -- the object carried no paint, so nothing was at stake.
// Remapped  -- paint was saved and remapped onto the new mesh (ModelVolume::restore_painting).
// Discarded -- the object was painted and the paint was dropped.
enum class PaintResult { None, Remapped, Discarded };

PaintResult classify_paint_result(bool had_paint, bool keep_paint);

// The info_messages line for a paint result. Empty for PaintResult::None -- there is nothing
// to tell a caller when nothing was painted. `operation` is the tool name, e.g. "split_object".
std::string paint_result_message(PaintResult result, const std::string& operation);

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 4: Write the implementation**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp
#include "OrcaMCPMeshUtils.hpp"

#include "libslic3r/Geometry.hpp"

#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

bool cut_plane_matrix(const Vec3d& plane_origin,
                      const Vec3d& plane_normal,
                      const Vec3d& instance_offset,
                      Transform3d& out)
{
    const double length = plane_normal.norm();
    if (!std::isfinite(length) || length < 1e-6)
        return false;

    const Vec3d normal = plane_normal / length;

    Vec3d    axis;
    double   phi = 0.;
    Matrix3d rotation = Matrix3d::Identity();
    Geometry::rotation_from_two_vectors(Vec3d::UnitZ(), normal, axis, phi, &rotation);

    out = Geometry::translation_transform(plane_origin - instance_offset) * Transform3d(rotation);
    return true;
}

PaintResult classify_paint_result(bool had_paint, bool keep_paint)
{
    if (!had_paint)
        return PaintResult::None;
    return keep_paint ? PaintResult::Remapped : PaintResult::Discarded;
}

std::string paint_result_message(PaintResult result, const std::string& operation)
{
    switch (result) {
    case PaintResult::Discarded:
        return operation + " discarded this object's painted facets "
               "(colour/support/seam/fuzzy skin). Pass keep_paint: true to remap them onto "
               "the new meshes instead.";
    case PaintResult::Remapped:
        return operation + " remapped this object's painted facets onto the new meshes. "
               "The remap is approximate; verify the result before printing.";
    case PaintResult::None:
    default:
        return {};
    }
}

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 5: Add both sources to the GUI library**

`src/slic3r/CMakeLists.txt`, immediately after line 426 (`GUI/OrcaMCP/MCPClientConfig.cpp`):

```cmake
    GUI/OrcaMCP/OrcaMCPMeshUtils.hpp
    GUI/OrcaMCP/OrcaMCPMeshUtils.cpp
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[mesh]"
```
Expected: PASS, 6 cases.

- [ ] **Step 7: Run the whole suite to confirm no regression**

```bash
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: 180 cases / 0 failures (174 baseline + 6 new).

- [ ] **Step 8: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp \
        src/slic3r/CMakeLists.txt \
        tests/slic3rutils/test_mesh_edit.cpp \
        tests/slic3rutils/CMakeLists.txt
git commit -m "$(cat <<'MSG'
feat: mesh editing needed geometry a test could reach without a slicer

The cut plane an agent describes is a point and a normal; Cut wants a matrix relative
to the instance offset whose rotation maps +Z onto that normal. That arithmetic lived
only inside GLGizmoCut3D::get_cut_matrix, where nothing without a canvas selection can
call it and no test can reach it. Extracted as a free function with Catch2 coverage,
alongside the paint bookkeeping every mesh-changing tool in this batch owes its caller.

Related occurrences checked: GLGizmoAdvancedCut.cpp has a near-copy of get_cut_matrix
and was left alone -- it is not in src/slic3r/CMakeLists.txt and no longer compiles
(it names ModelObjectCutAttribute::CutToParts and ModelObject::get_connector_mesh,
neither of which exists). GLGizmoCut3D::get_cut_matrix itself was left alone: it reads
the SLA shift off the selected GLVolume, which an HTTP worker has no business touching.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 2: `split_object` — split a multi-body object into separate objects

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp:66` (beside `register_printer_tools`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:4334-4335` (the `register_filament_tools(); register_printer_tools();` pair at the end of `register_builtin_tools()`)
- Modify: `src/slic3r/CMakeLists.txt` (after the two lines added in Task 1)
- Modify: `docs/tools/reference.md:14` and `docs/tools/reference.md:342-379` (the "Object Operations" section)
- Modify: `CLAUDE.md` (Models row of the tool table; the "70 registered, 71 reachable" heading)

**Interfaces:**
- Consumes: `classify_paint_result`, `paint_result_message`, `PaintResult` (Task 1).
- Produces:
  ```cpp
  // OrcaMCPServer.hpp, private section
  static void register_mesh_tools();   // defined in OrcaMCPMeshTools.cpp
  ```
  and the MCP tool `split_object` with parameters
  `{object_id: int (required), mode: "objects"|"parts", keep_paint: bool,
    include_preview: bool}`. Task 3 adds `mode: "parts"`; this task registers the
  parameter and rejects `"parts"` with a "not implemented in this build" error so the
  schema is stable from the first commit.

**Why this tool matters beyond itself:** `set_object_filament` already accepts a
`volume_id` (`OrcaMCPFilamentTools.cpp:139-143`) and nothing in the API can currently
produce parts to address. Task 3 closes that; this task establishes the tool.

**The API and its trap.** `ModelObject::split(ModelObjectPtrs* new_objects, bool remap_paint)`
(`Model.hpp:521`, implemented `Model.cpp:2117-2244`) does **not** return the new objects for
the caller to place — it calls `m_model->add_object()` (`Model.cpp:2183`) so they land in the
model that owns the object, *and* appends them to `new_objects`. Splitting the live model
therefore duplicates every part. `Plater::priv::split_object` handles this by splitting a copy
of the whole `Model` (`Plater.cpp:10174-10177`, with the comment explaining exactly this) and
loading the results back. This task does the same, then hands the result to the public
`Plater::apply_cut_object_to_model` (`Plater.hpp:545`, `Plater.cpp:17891-17914`), which deletes
the original, copies the new objects into the live model via `load_model_objects`, refreshes
the scene and selects them. That is the same call the compiled cut gizmo uses
(`GLGizmoCut.cpp:3623`).

**Two behaviours to document, not hide:**
1. `apply_cut_object_to_model` calls `load_model_objects(new_objects, false, false)` with
   `auto_drop` defaulted to `true`, so every new object gets `ensure_on_bed()`
   (`Plater.cpp:9615`). The GUI asks the user first ("Disable Auto-Drop to preserve Z
   positioning?", `Plater.cpp:10200-10206`); MCP cannot ask, so parts are dropped to the bed
   and the response says `"dropped_to_bed": true`.
2. That same call passes `split_object = false`, so assemble transformations are
   re-initialised (`Plater.cpp:9618-9628`) where the GUI's split passes `true` to skip it.
   Assembly view is explicitly out of scope for this batch (spec, Plan 4), so this is
   acceptable; note it in `reference.md`.

- [ ] **Step 1: Write the failing check — register the tool and prove it is reachable**

There is no unit test for a handler that needs a `Plater`; the failing check is that the
tool does not exist yet. Rebuild and reload (Global Constraints recipe), then run:

```
mcp__orca-slicer__get_server_info {}
```
Expected: the response's tool catalogue has no `split_object`. Record the tool count from
`OrcaMCPServer: Registered N tools` in the log, or from `get_server_info`; it is 70.

- [ ] **Step 2: Declare the registrar**

`src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp`, after line 66:

```cpp
    // Printer and physical-printer tools (OrcaMCPPrinterTools.cpp)
    static void register_printer_tools();
    // Mesh editing tools -- split, cut, boolean, simplify (OrcaMCPMeshTools.cpp)
    static void register_mesh_tools();
```

- [ ] **Step 3: Create the tool group file with `split_object`**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp
//
// Mesh editing: split, cut, boolean, simplify. These all replace an object's geometry, so
// every one of them takes an undo snapshot before the first mutation and reports what
// happened to the object's painted facets -- paint does not survive a mesh change unless it
// is explicitly saved and remapped, and losing it silently is worse than losing it loudly.
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPMeshUtils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// True when any volume of the object carries any of the four facet annotations.
// ModelVolume::is_any_painted (Model.hpp:1029) covers colour, support, seam and fuzzy skin.
bool object_is_painted(const Slic3r::ModelObject* object)
{
    for (const Slic3r::ModelVolume* volume : object->volumes)
        if (volume->is_any_painted())
            return true;
    return false;
}

// The {had_paint, keep_paint, result} block every tool in this file returns, plus the
// info_messages line that goes with it. `messages` is appended to, never replaced, so a
// suppressed-dialog message already collected survives.
void report_paint(nlohmann::json&           result,
                  std::vector<std::string>& messages,
                  bool                      had_paint,
                  bool                      keep_paint,
                  const std::string&        operation)
{
    const PaintResult outcome = classify_paint_result(had_paint, keep_paint);
    result["paint"] = {
        {"had_paint", had_paint},
        {"keep_paint", keep_paint},
        {"result", outcome == PaintResult::None      ? "none" :
                   outcome == PaintResult::Remapped  ? "remapped" : "discarded"}
    };
    const std::string message = paint_result_message(outcome, operation);
    if (!message.empty())
        messages.push_back(message);
}

// Split the object into one object per disconnected body.
//
// ModelObject::split adds the new objects to the model that owns the object (Model.cpp:2183),
// so it is run against a copy of the Model and the results are loaded back -- the same reason
// and the same shape as Plater::priv::split_object (Plater.cpp:10170-10177).
nlohmann::json split_to_objects(int object_id, bool keep_paint)
{
    Plater*        plater = wxGetApp().plater();
    Slic3r::Model& model  = plater->model();

    Slic3r::ModelObject* original   = model.objects[object_id];
    const std::string    name       = original->name;
    const bool           had_paint  = object_is_painted(original);

    Slic3r::Model            scratch = model;   // copy ctor, as Plater.cpp:10174 does
    Slic3r::ModelObjectPtrs  new_objects;
    scratch.objects[object_id]->split(&new_objects, keep_paint);

    if (new_objects.size() <= 1) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "Object '" + name + "' is a single connected body and cannot be split "
                        "into objects. Nothing was changed."}
        };
    }

    plater->take_snapshot("Split to Objects");
    plater->apply_cut_object_to_model(static_cast<size_t>(object_id), new_objects);

    // apply_cut_object_to_model deletes the original and appends the new objects, so they are
    // the last new_objects.size() entries of the model.
    const int count = static_cast<int>(new_objects.size());
    const int first = static_cast<int>(model.objects.size()) - count;
    nlohmann::json ids = nlohmann::json::array();
    for (int i = 0; i < count; ++i)
        ids.push_back(first + i);

    return nlohmann::json{
        {"status", "success"},
        {"mode", "objects"},
        {"original_object", name},
        {"new_object_ids", ids},
        {"new_objects_count", count},
        {"dropped_to_bed", true},
        {"had_paint", had_paint}
    };
}

} // namespace

void OrcaMCPServer::register_mesh_tools()
{
    register_tool({
        "split_object",
        "Split a multi-body object. mode \"objects\" turns each disconnected body into its own "
        "object; mode \"parts\" turns each body into a part (volume) of the same object, which "
        "is what set_object_filament's volume_id addresses. Painted facets (colour, support, "
        "seam, fuzzy skin) are DISCARDED unless keep_paint is true; the response always says "
        "which happened. In mode \"objects\" every new object is dropped onto the bed.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"objects", "parts"})},
                    {"description", "objects (default): separate objects. parts: volumes of one object."}
                }},
                {"keep_paint", {
                    {"type", "boolean"},
                    {"description", "Remap existing painted facets onto the split meshes. "
                                    "Slow and approximate. Default false, which discards them."}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", nlohmann::json::array({"object_id"})}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const int         object_id       = params.at("object_id");
            const std::string mode            = params.value("mode", "objects");
            const bool        keep_paint      = params.value("keep_paint", false);
            const bool        include_preview = params.value("include_preview", false);

            return run_on_main_thread([object_id, mode, keep_paint, include_preview]() -> nlohmann::json {
                McpDialogSuppressionGuard suppression_guard;

                Plater*        plater = wxGetApp().plater();
                Slic3r::Model& model  = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size()))
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));

                if (mode != "objects" && mode != "parts")
                    return nlohmann::json{{"status", "error"},
                                          {"message", "mode must be \"objects\" or \"parts\", got \"" + mode + "\""}};
                if (mode == "parts")
                    return nlohmann::json{{"status", "error"},
                                          {"message", "mode \"parts\" is not implemented in this build"}};

                nlohmann::json result = split_to_objects(object_id, keep_paint);
                if (result["status"] != "success")
                    return result;

                const bool had_paint = result["had_paint"];
                result.erase("had_paint");
                std::vector<std::string> messages = suppression_guard.messages();
                report_paint(result, messages, had_paint, keep_paint, "split_object");
                if (!messages.empty())
                    result["info_messages"] = messages;

                result["active_warnings"] = get_active_warnings_json(plater);
                add_turntable_preview_if_requested(result, include_preview);
                return result;
            });
        }
    });
}
```

- [ ] **Step 4: Call the registrar and add the file to CMake**

`src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp`, replacing lines 4334-4335:

```cpp
    register_filament_tools();
    register_printer_tools();
    register_mesh_tools();
```

`src/slic3r/CMakeLists.txt`, after the two `OrcaMCPMeshUtils` lines added in Task 1:

```cmake
    GUI/OrcaMCP/OrcaMCPMeshTools.cpp
```

- [ ] **Step 5: Rebuild and reload, then verify the tool works**

Run the Global Constraints rebuild-and-reload recipe. Then, with a **multi-body** model
loaded (see Global Constraints, "Test model"):

```
mcp__orca-slicer__get_scene_info {}
mcp__orca-slicer__split_object {"object_id": 0}
```
Expected: `status: "success"`, `mode: "objects"`, `new_objects_count` > 1, `new_object_ids`
listing consecutive indices, `paint.result: "none"` for an unpainted model, and
`active_warnings.count` present.

Then prove the snapshot works — this is the constraint the shipped `cut_object` violates:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__get_scene_info {}
```
Expected: the object count is back to what it was before the split.

Then prove the single-body rejection:
```
mcp__orca-slicer__split_object {"object_id": 0}   # against tower_with_seam.drc
```
Expected: `status: "error"` naming the object, and `get_scene_info` unchanged.

- [ ] **Step 6: Update the docs**

`docs/tools/reference.md:14`, extend the Object Ops row:

```markdown
| **Object Ops** | `clone_object`, `cut_object`, `split_object`, `delete_object`, `rename_object`, `transform_objects` |
```

`docs/tools/reference.md`, a new section immediately after `### cut_object` (which ends at
line 377, before the `---` at 378):

````markdown
### split_object
Split a multi-body object. `mode: "objects"` makes each disconnected body its own object;
`mode: "parts"` makes each body a part (volume) of the same object — the parts that
`set_object_filament`'s `volume_id` addresses.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `mode` | string | No | `"objects"` (default) or `"parts"` |
| `keep_paint` | boolean | No | Remap painted facets onto the new meshes. Default `false` — paint is discarded |
| `include_preview` | boolean | No | Return a turntable preview path |

**Paint:** a mesh change does not carry painted facets with it. By default `split_object`
discards colour, support, seam and fuzzy-skin paint, matching OrcaSlicer's own shipped
default (Preferences → "Keep painted feature after mesh change", off). Set `keep_paint: true`
to remap it instead; the remap is approximate and slow. Either way the response carries
`paint: {had_paint, keep_paint, result}` where `result` is `"none"`, `"remapped"` or
`"discarded"`, and a matching `info_messages` line whenever paint was at stake.

**Mode `"objects"` also:** drops every new object onto the bed (the GUI offers to preserve Z
instead; an MCP call cannot be asked), and re-initialises assemble transformations, so an
assembly view arrangement is not preserved.

**Example:**
```json
{"name": "split_object", "arguments": {"object_id": 0, "mode": "parts"}}
```

**Returns:** `status`, `mode`, `original_object`, `new_object_ids` (mode `"objects"`),
`part_count` (mode `"parts"`), `paint`, `active_warnings`.

**Errors:** an object that is a single connected body cannot be split; the call returns
`status: "error"` and changes nothing.
````

`CLAUDE.md`: change the heading `### MCP Tools (70 registered, 71 reachable)` to
`### MCP Tools (71 registered, 72 reachable)`, change the sentence "The server registers 70;"
to "The server registers 71;", and add `split_object` to the **Models** row of the tool table.

- [ ] **Step 7: Confirm the count command agrees**

```bash
cd /Users/hanan/Projects/OrcaMCP
grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'
```
Expected: `71`.

- [ ] **Step 8: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp \
        src/slic3r/CMakeLists.txt docs/tools/reference.md CLAUDE.md
git commit -m "$(cat <<'MSG'
feat: a multi-body model could be loaded over MCP but never taken apart

splitobjects and splitvolumes were the only top-toolbar items with no MCP tool at all,
and their absence had a second cost: set_object_filament has always accepted a volume_id
and nothing in the API could produce parts to address. This adds split_object with the
"objects" half; "parts" follows.

Root cause of the trap this had to avoid: ModelObject::split does not hand its results
back for the caller to place, it calls m_model->add_object() itself (Model.cpp:2183), so
splitting the live model duplicates every body. Split a copy and load the result back,
which is what Plater::priv::split_object does and says so in a comment.

Related occurrences checked: Plater::split_object(int, bool) is declared at Plater.hpp:737
and never defined, so it was not used -- only the selection-driven overload exists.
ObjectList::split() was not used either: it opens a native wxMessageBox for a
non-splittable object (GUI_ObjectList.cpp:2905), which McpDialogSuppressionGuard cannot
suppress because it is not a MsgDialog, and a modal inside run_on_main_thread never returns.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 3: `split_object` — split into parts, so `set_object_filament` has volumes to address

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` (anonymous namespace and the handler)
- Modify: `docs/tools/workflows.md` (append a pattern)

**Interfaces:**
- Consumes: `object_is_painted`, `report_paint` (Task 2, same file).
- Produces: `split_object` with `mode: "parts"` answering
  `{"status":"success","mode":"parts","object_id":N,"object_name":"...","part_count":K,
    "parts":[{"volume_id":0,"name":"tower_1"},...],"paint":{...},"active_warnings":{...}}`.

**The API.** `ModelVolume::split(unsigned int max_extruders, bool remap_paint)`
(`Model.hpp:946`, implemented `Model.cpp:2842-2908`) splits **in place**: the first body
replaces the volume's mesh, the rest are inserted after it in `object->volumes`. It returns the
number of volumes produced. It resets the annotations on the first volume
(`Model.cpp:2877`) and then calls `restore_painting(saved_painting)` on each new volume
(`Model.cpp:2890`) — where `saved_painting` is empty unless `remap_paint` is true
(`Model.cpp:2852-2853`). So `remap_paint == false` is a genuine discard, not a no-op.

`ModelVolume::is_splittable()` (`Model.hpp:935`, `Model.cpp:2600-2606`) is the cheap
pre-check; it caches `its_is_splittable`. `ObjectList::split()` checks it and then opens a
native `wxMessageBox` when it is false (`GUI_ObjectList.cpp:2905`) — that is why this tool
checks it itself and returns an error instead of calling `ObjectList::split()`.

**The `max_extruders` argument is inert.** `ModelVolume::split` sets every new volume's
extruder to the original's (`Model.cpp:2888`); the `auto_extruder_id(max_extruders, ...)` line
beside it is commented out (`Model.cpp:2889`). Pass the same value the GUI passes anyway
(the project's filament count) so the two paths cannot drift.

**Refreshing the object list.** A volume-count change is a structural change to the sidebar
tree, and `ObjectList`'s incremental updater is private. `ObjectList::reload_all_plates()`
(`GUI_ObjectList.hpp:472`, `GUI_ObjectList.cpp:6705-6743`) rebuilds the whole tree from the
model and calls `plater->update()` itself; it is what the undo/redo path uses
(`Plater.cpp:21680`, `Plater.cpp:21723`). Use it, and do not call `plater->update()` again.

- [ ] **Step 1: Write the failing check**

Rebuild and reload (Global Constraints recipe) if the running binary is not already Task 2's.
With a multi-body model loaded:

```
mcp__orca-slicer__split_object {"object_id": 0, "mode": "parts"}
```
Expected: `{"status":"error","message":"mode \"parts\" is not implemented in this build"}`.

- [ ] **Step 2: Add the split-to-parts helper**

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp`, in the anonymous namespace, after
`split_to_objects`:

```cpp
// Split the object's single volume into one volume per disconnected body, in place.
//
// ModelVolume::split (Model.cpp:2842) mutates object->volumes directly, so unlike
// split_to_objects there is no scratch Model. The sidebar tree has to be rebuilt afterwards:
// a volume-count change is structural and ObjectList's incremental updater is private, so
// reload_all_plates() (GUI_ObjectList.cpp:6705) does it -- it also calls plater->update().
nlohmann::json split_to_parts(int object_id, bool keep_paint)
{
    Plater*        plater = wxGetApp().plater();
    Slic3r::Model& model  = plater->model();

    Slic3r::ModelObject* object = model.objects[object_id];
    const std::string    name   = object->name;

    if (object->volumes.size() != 1) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "Object '" + name + "' already has " +
                        std::to_string(object->volumes.size()) +
                        " parts. split_object mode \"parts\" splits a single-part object; "
                        "call get_object_info to see the parts it already has."}
        };
    }

    Slic3r::ModelVolume* volume = object->volumes.front();
    if (!volume->is_splittable()) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "Object '" + name + "' is a single connected body and cannot be split "
                        "into parts. Nothing was changed."}
        };
    }

    const bool had_paint = volume->is_any_painted();

    // The GUI passes the project's filament count here (GUI_ObjectList.cpp:2897-2900).
    // ModelVolume::split does not currently use it -- it copies the original extruder onto
    // every new volume (Model.cpp:2888) -- but pass the same value so the paths cannot drift.
    const Slic3r::DynamicPrintConfig& printer_cfg =
        wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto* filament_colors = printer_cfg.option<Slic3r::ConfigOptionStrings>("filament_colour", false);
    const unsigned int filament_count =
        filament_colors == nullptr ? 1u : static_cast<unsigned int>(filament_colors->size());

    plater->take_snapshot("Split to parts");
    const size_t part_count = volume->split(filament_count, keep_paint);

    // The mesh no longer corresponds to the file it came from; the GUI clears this too
    // (GUI_ObjectList.cpp:2940).
    object->input_file.clear();

    wxGetApp().obj_list()->reload_all_plates();
    wxGetApp().obj_list()->update_info_items(static_cast<size_t>(object_id));

    nlohmann::json parts = nlohmann::json::array();
    for (size_t i = 0; i < object->volumes.size(); ++i)
        parts.push_back({{"volume_id", static_cast<int>(i)}, {"name", object->volumes[i]->name}});

    return nlohmann::json{
        {"status", "success"},
        {"mode", "parts"},
        {"object_id", object_id},
        {"object_name", name},
        {"part_count", static_cast<int>(part_count)},
        {"parts", parts},
        {"had_paint", had_paint}
    };
}
```

- [ ] **Step 3: Route `mode: "parts"` to it**

In the `split_object` handler, replace:

```cpp
                if (mode == "parts")
                    return nlohmann::json{{"status", "error"},
                                          {"message", "mode \"parts\" is not implemented in this build"}};

                nlohmann::json result = split_to_objects(object_id, keep_paint);
```

with:

```cpp
                nlohmann::json result = mode == "parts" ? split_to_parts(object_id, keep_paint)
                                                        : split_to_objects(object_id, keep_paint);
```

- [ ] **Step 4: Rebuild and reload, then verify**

Run the rebuild-and-reload recipe. With a multi-body model loaded:

```
mcp__orca-slicer__split_object {"object_id": 0, "mode": "parts"}
```
Expected: `status: "success"`, `mode: "parts"`, `part_count` > 1, `parts` listing
`volume_id` 0..N-1 with names ending `_1`, `_2`, …, and the scene still holding **one**
object (unlike mode `"objects"`).

Now the point of the whole task — address a part:
```
mcp__orca-slicer__set_object_filament {"object_id": 0, "volume_id": 1, "filament": 2}
```
Expected: `status: "success"`, `volume_id: 1`. Confirm in the app that only that part changed
colour.

Then the undo snapshot:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__undo {}
```
Expected: the filament assignment reverts, then the object is one part again.

Then the rejections:
```
mcp__orca-slicer__split_object {"object_id": 0, "mode": "parts"}   # already multi-part
```
Expected: `status: "error"` naming the existing part count.

- [ ] **Step 5: Add the workflow**

Append to `docs/tools/workflows.md`:

````markdown
## Colour a multi-body model per body

`set_object_filament` takes a `volume_id`, but a freshly loaded model is one volume. Split it
into parts first, then address each part.

```json
{"name": "split_object", "arguments": {"object_id": 0, "mode": "parts"}}
```
Read `parts` from the response — `volume_id` and the name of each part — then assign:
```json
{"name": "set_object_filament", "arguments": {"object_id": 0, "volume_id": 1, "filament": 2}}
```
Confirm with `get_object_info`, whose `parts` array reports each part's current filament.

Use `mode: "objects"` instead when the bodies should arrange and print as separate objects;
use `mode: "parts"` when they are one object printed together in different filaments.
````

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp docs/tools/workflows.md
git commit -m "$(cat <<'MSG'
feat: set_object_filament's volume_id had nothing to address

split_object mode "parts" is the other half of the volume_id parameter that has shipped
since the filament tools landed: an agent could name a part but nothing in the API could
create one, so per-body colour was unreachable over MCP even though the assignment path
worked.

ModelVolume::split mutates object->volumes in place, so this needs no scratch Model --
but a volume-count change is structural for the sidebar tree and ObjectList's incremental
updater is private, so the refresh goes through reload_all_plates(), the same call the
undo/redo path uses.

Related occurrences checked: ObjectList::split() was deliberately not reused. It opens a
native wxMessageBox when the volume is not splittable (GUI_ObjectList.cpp:2905); a native
dialog is not a MsgDialog, so McpDialogSuppressionGuard cannot suppress it, and a modal
inside run_on_main_thread hangs the GUI thread forever. The is_splittable() check is done
here instead and returns an error the caller can read.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 4: `get_object_info` reports the object's parts

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp` / `.cpp` (add `volume_type_name`)
- Modify: `tests/slic3rutils/test_mesh_edit.cpp` (add its coverage)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:3899-3948` (the `get_object_info` handler body)
- Modify: `docs/tools/reference.md:717-726` (the `get_object_info` entry)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  ```cpp
  // OrcaMCPMeshUtils.hpp
  const char* volume_type_name(ModelVolumeType type);
  ```
  and `get_object_info`'s response gains `part_count` (integer) and `parts` (array of
  `{volume_id, name, type, filament, triangles, painted:{color, support, seam, fuzzy_skin}}`).

**Why here:** Task 3 can create parts and Task 2's response lists them once, but there is no
way to read them back later. `get_object_info` (`OrcaMCPServer.cpp:3876-3951`) reports the
object's bounding box, transform and bed status and never mentions volumes at all, so an agent
that split an object in an earlier call cannot rediscover the `volume_id`s it needs.

- [ ] **Step 1: Write the failing test for `volume_type_name`**

Append to `tests/slic3rutils/test_mesh_edit.cpp`:

```cpp
#include "libslic3r/Model.hpp"

TEST_CASE("volume_type_name names every ModelVolumeType a caller can meet", "[orcamcp][mesh]")
{
    // Model.hpp:341-347. The names are the caller-facing vocabulary, not the enum spelling.
    CHECK(std::string(volume_type_name(ModelVolumeType::MODEL_PART))         == "part");
    CHECK(std::string(volume_type_name(ModelVolumeType::NEGATIVE_VOLUME))    == "negative");
    CHECK(std::string(volume_type_name(ModelVolumeType::PARAMETER_MODIFIER)) == "modifier");
    CHECK(std::string(volume_type_name(ModelVolumeType::SUPPORT_BLOCKER))    == "support_blocker");
    CHECK(std::string(volume_type_name(ModelVolumeType::SUPPORT_ENFORCER))   == "support_enforcer");
    CHECK(std::string(volume_type_name(ModelVolumeType::INVALID))            == "invalid");
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
```
Expected: FAIL — `use of undeclared identifier 'volume_type_name'`.

- [ ] **Step 3: Declare and implement it**

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp`, before the closing namespace, and add
`#include "libslic3r/Model.hpp"` to the header's includes:

```cpp
// The caller-facing name of a volume's type (Model.hpp:341-347). "part" is the printable
// geometry; everything else modifies it. Never returns nullptr.
const char* volume_type_name(ModelVolumeType type);
```

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp`:

```cpp
const char* volume_type_name(ModelVolumeType type)
{
    switch (type) {
    case ModelVolumeType::MODEL_PART:         return "part";
    case ModelVolumeType::NEGATIVE_VOLUME:    return "negative";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER:    return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER:   return "support_enforcer";
    case ModelVolumeType::INVALID:
    default:                                  return "invalid";
    }
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[mesh]"
```
Expected: PASS, 7 cases.

- [ ] **Step 5: Report the parts from `get_object_info`**

`src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp`, add the include beside the other OrcaMCP headers
at the top of the file:

```cpp
#include "OrcaMCPMeshUtils.hpp"
```

and in the `get_object_info` handler, immediately after the `result["on_bed"] = on_bed;` line
(`OrcaMCPServer.cpp:3947`) and before `return result;`:

```cpp
                // The object's parts (volumes). set_object_filament, set_object_config and
                // split_object all address a part by this volume_id, and nothing else in the
                // API reports it.
                nlohmann::json parts = nlohmann::json::array();
                for (size_t i = 0; i < obj->volumes.size(); ++i) {
                    const ModelVolume* volume = obj->volumes[i];
                    parts.push_back({
                        {"volume_id", static_cast<int>(i)},
                        {"name", volume->name},
                        {"type", volume_type_name(volume->type())},
                        {"filament", volume->config.has("extruder") ? volume->config.extruder() : 0},
                        {"triangles", static_cast<int>(volume->mesh().facets_count())},
                        {"painted", {
                            {"color", volume->is_mm_painted()},
                            {"support", volume->is_fdm_support_painted()},
                            {"seam", volume->is_seam_painted()},
                            {"fuzzy_skin", volume->is_fuzzy_skin_painted()}
                        }}
                    });
                }
                result["part_count"] = static_cast<int>(obj->volumes.size());
                result["parts"] = parts;
```

`filament` is 0 when the part inherits the object's filament rather than overriding it —
the same convention `ObjectList::split` uses (`GUI_ObjectList.cpp:2926`).

- [ ] **Step 6: Rebuild and reload, then verify**

Run the rebuild-and-reload recipe. With a multi-body model loaded:

```
mcp__orca-slicer__get_object_info {"object_id": 0}
mcp__orca-slicer__split_object {"object_id": 0, "mode": "parts"}
mcp__orca-slicer__get_object_info {"object_id": 0}
```
Expected: `part_count: 1` before, `part_count` > 1 after, with `parts[i].volume_id` matching
the ids `split_object` returned and every `type` reading `"part"`.

```
mcp__orca-slicer__set_object_filament {"object_id": 0, "volume_id": 1, "filament": 3}
mcp__orca-slicer__get_object_info {"object_id": 0}
```
Expected: `parts[1].filament == 3`, other parts unchanged.

- [ ] **Step 7: Update the docs**

`docs/tools/reference.md`, in the `### get_object_info` entry (line 717), append:

```markdown
**Returns:** `object_id`, `name`, `instance_count`, `position`, `bounding_box`,
`rotation_degrees`, `scale`, `on_bed`, `part_count`, `parts`.

`parts` lists the object's volumes in `volume_id` order — the id `set_object_filament`,
`set_object_config` and `split_object` address. Each entry carries `name`, `type`
(`part`, `negative`, `modifier`, `support_blocker`, `support_enforcer`), `filament`
(0 when the part inherits the object's filament), `triangles`, and `painted` flags for
`color`, `support`, `seam` and `fuzzy_skin`.
```

- [ ] **Step 8: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp \
        tests/slic3rutils/test_mesh_edit.cpp docs/tools/reference.md
git commit -m "$(cat <<'MSG'
fix: an object's parts could be created and addressed but never read back

get_object_info reported the bounding box, the transform and the bed status and never
mentioned volumes, so an agent that split an object in an earlier call had no way to
rediscover the volume_ids set_object_filament and set_object_config need. split_object's
own response listed them once and then they were gone.

Added part_count and parts to get_object_info, each part carrying the id, name, type,
filament override and the four paint flags -- so the read-back also answers "is this part
painted", which every mesh-editing tool in this batch has to warn about.

Related occurrences checked: get_scene_info was left alone. It reports every object on
every plate and adding a per-part array there would grow a response that T2 already found
too large for an MCP client; the per-object tool is the right place for per-part detail.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 5: Move `cut_object` beside the other mesh tools, and give it the undo snapshot it never had

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:4157-4254` (delete the `cut_object` registration and its `// ==================== OBJECT CUTTING ====================` banner)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` (receive it)
- Modify: `docs/tools/reference.md:363-377` (the `keep` default is documented wrong)

**Interfaces:**
- Consumes: `report_paint`, `object_is_painted` (Task 2, same file).
- Produces: `cut_object` unchanged in call shape and response fields, plus `paint`. This is a
  published contract (CLAUDE.md lists it among the 70 tools); `{"object_id":0,"z_height":25}`
  and `{"object_id":0,"z_height":25,"keep":"both"}` must behave exactly as before.

**Two defects this task fixes, both found by reading the shipped handler:**

1. **No undo snapshot.** `cut_object` (`OrcaMCPServer.cpp:4185-4253`) adds the cut results with
   `model.add_object`, removes the original with `plater->remove(object_id)` and calls
   `plater->update()` — with no `take_snapshot` anywhere. `undo` after a cut does not restore
   the object. Every other mutating path in the codebase wraps the cut in one:
   `GLGizmoCut3D::perform_cut` uses `Plater::TakeSnapshot snapshot(plater, _u8L("Cut by Plane"))`
   (`GLGizmoCut.cpp:3535`). The Global Constraints require it.
2. **The documented default is wrong.** `reference.md:371` says `keep` defaults to `"both"`;
   the code says `params.value("keep", "below")` (`OrcaMCPServer.cpp:4187`). The code is the
   published behaviour, so the doc is what changes.

**Also switch the placement call.** `plater->remove(object_id)` after `model.add_object` leaves
the object list and the plate assignment to be picked up by `plater->update()`.
`Plater::apply_cut_object_to_model(obj_idx, new_objects)` (`Plater.hpp:545`,
`Plater.cpp:17891`) is the call the compiled cut gizmo makes (`GLGizmoCut.cpp:3623`): it
deletes the original from both model and object list, loads the new objects, updates the scene,
refreshes the info items and selects the results. Same visible outcome, one supported call.

- [ ] **Step 1: Write the failing check**

Rebuild and reload if needed. Load `resources/calib/pressure_advance/tower_with_seam.drc` and:

```
mcp__orca-slicer__get_scene_info {}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10}
mcp__orca-slicer__undo {}
mcp__orca-slicer__get_scene_info {}
```
Expected today: the last `get_scene_info` still shows the cut result — `undo` did not restore
the object. Record the object names and count; that is the failing evidence.

- [ ] **Step 2: Delete the old registration**

`src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp`: delete lines 4157 through 4254 inclusive — the
`// ==================== OBJECT CUTTING ====================` banner, the `// cut_object - Cut
an object at a specified Z height` comment, and the whole `register_tool({ "cut_object", ... });`
block. Leave the `// ==================== G-CODE VIEW TYPE ====================` banner that
follows it.

- [ ] **Step 3: Add it to the mesh tools file, snapshotted**

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp`: add to the includes

```cpp
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
```

and register the tool inside `register_mesh_tools()`, after `split_object`:

```cpp
    register_tool({
        "cut_object",
        "Cut object at Z height. keep: below, above, or both.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"z_height", {
                    {"type", "number"},
                    {"description", "Cut height in mm"}
                }},
                {"keep", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"below", "above", "both"})},
                    {"description", "below (default), above, or both"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", nlohmann::json::array({"object_id", "z_height"})}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const int         object_id       = params.at("object_id");
            const double      z_height        = params.at("z_height");
            const std::string keep            = params.value("keep", "below");
            const bool        include_preview = params.value("include_preview", false);

            return run_on_main_thread([object_id, z_height, keep, include_preview]() -> nlohmann::json {
                McpDialogSuppressionGuard suppression_guard;

                Plater*        plater = wxGetApp().plater();
                Slic3r::Model& model  = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size()))
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));

                Slic3r::ModelObject* object    = model.objects[object_id];
                const std::string    name      = object->name;
                const bool           had_paint = object_is_painted(object);

                // The cut plane is expressed relative to the instance offset, as
                // GLGizmoCut3D::get_cut_matrix does (GLGizmoCut.cpp:3444-3459).
                const Slic3r::Vec3d instance_offset = object->instances[0]->get_offset();
                const Slic3r::Transform3d cut_matrix = Slic3r::Geometry::translation_transform(
                    Slic3r::Vec3d(0., 0., z_height - instance_offset.z()));

                // PlaceOnCut flips a piece so the cut face becomes the new bottom.
                // below: the lower piece is already on the bed, no flip.
                // above / both: flip so every kept piece is printable.
                Slic3r::ModelObjectCutAttributes attributes;
                if (keep == "above") {
                    attributes = Slic3r::ModelObjectCutAttribute::KeepUpper |
                                 Slic3r::ModelObjectCutAttribute::PlaceOnCutUpper;
                } else if (keep == "both") {
                    attributes = Slic3r::ModelObjectCutAttribute::KeepUpper |
                                 Slic3r::ModelObjectCutAttribute::KeepLower |
                                 Slic3r::ModelObjectCutAttribute::PlaceOnCutUpper |
                                 Slic3r::ModelObjectCutAttribute::PlaceOnCutLower;
                } else { // below (default)
                    attributes = Slic3r::ModelObjectCutAttribute::KeepLower;
                }

                Slic3r::Cut cut(object, 0, cut_matrix, attributes);
                const Slic3r::ModelObjectPtrs& new_objects = cut.perform_with_plane();
                const int count = static_cast<int>(new_objects.size());

                // Snapshot before the first mutation of the live model. Cut works on its own
                // copy, so this is the right point -- and it is the step the shipped tool
                // was missing, which is why undo never restored a cut object.
                plater->take_snapshot("Cut by Plane");
                plater->apply_cut_object_to_model(static_cast<size_t>(object_id), new_objects);

                nlohmann::json result = {
                    {"status", "success"},
                    {"original_object", name},
                    {"z_height", z_height},
                    {"kept", keep},
                    {"new_objects_count", count}
                };

                std::vector<std::string> messages = suppression_guard.messages();
                report_paint(result, messages, had_paint, /*keep_paint=*/false, "cut_object");
                if (!messages.empty())
                    result["info_messages"] = messages;

                result["active_warnings"] = get_active_warnings_json(plater);
                add_turntable_preview_if_requested(result, include_preview);
                return result;
            });
        }
    });
```

`keep_paint` is hard-coded `false` here so this task changes no behaviour — `Cut` only saves
paint when `ModelObjectCutAttribute::KeepPaint` is set (`CutUtils.cpp:337-347`), which the
shipped tool never set. Task 6 makes it a parameter.

- [ ] **Step 4: Rebuild and reload, then verify nothing changed except undo**

Run the rebuild-and-reload recipe. Load the tower and run each published call shape:

```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10}
```
Expected: `status: "success"`, `kept: "below"`, `new_objects_count: 1`, and one object on the
plate, shorter than before — identical to the recorded behaviour from Step 1, plus a
`paint: {"had_paint": false, "keep_paint": false, "result": "none"}` block.

```
mcp__orca-slicer__undo {}
mcp__orca-slicer__get_scene_info {}
```
Expected, and this is the fix: the uncut object is back.

Repeat for `"keep": "above"` and `"keep": "both"`, checking `new_objects_count` is 1 and 2
respectively and that every piece sits on the bed.

- [ ] **Step 5: Fix the documented default**

`docs/tools/reference.md:371`:

```markdown
| `keep` | string | No | "below", "above", or "both" (default: "below") |
```

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp docs/tools/reference.md
git commit -m "$(cat <<'MSG'
fix: undo could not take back a cut, because cut_object took no snapshot

cut_object added the cut results, removed the original and called plater->update() with no
take_snapshot anywhere (OrcaMCPServer.cpp, the shipped handler), so the undo stack had no
record of the object it destroyed and the undo tool silently did nothing useful. Every
other cut path in the tree wraps the operation in one -- GLGizmoCut3D::perform_cut uses
Plater::TakeSnapshot("Cut by Plane").

Also switched the placement from model.add_object + plater->remove to the public
Plater::apply_cut_object_to_model, the call the compiled cut gizmo makes: it removes the
original from the object list as well as the model, so the sidebar cannot be left holding
a row for an object that is gone.

Moved the tool to OrcaMCPMeshTools.cpp beside split_object, ahead of the arbitrary-plane
work, and corrected reference.md, which documented keep's default as "both" when the code
has always defaulted it to "below". The call shape is unchanged: it is published.

Related occurrences checked: the other mutating tools in OrcaMCPServer.cpp were audited for
the same missing snapshot. set_object_printable takes one; the transform tools go through
Plater helpers that take their own; arrange_objects and auto_orient run through jobs that
snapshot themselves. cut_object was the only one mutating the model directly without one.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 6: `cut_object` — an arbitrary plane, and a say in what happens to the paint

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` (the `cut_object` registration)
- Modify: `docs/tools/reference.md` (the `cut_object` entry)

**Interfaces:**
- Consumes: `cut_plane_matrix` (Task 1), `report_paint` (Task 2).
- Produces: `cut_object` gains
  `plane_origin: {x,y,z}`, `plane_normal: {x,y,z}`, `place_on_cut: bool`, `flip: bool`,
  `keep_paint: bool`, and a `plane` block in the response reporting the origin and the
  normalised normal actually used. `z_height` stays supported and stays the only required
  geometry parameter when `plane_origin` is absent.

**Backwards compatibility is the constraint.** `z_height` is a published parameter. The rule:
`plane_origin`/`plane_normal` and `z_height` are alternatives; supplying `z_height` alone is
exactly today's behaviour (origin `(0, 0, z_height)`, normal `+Z`); supplying both is an error
that names the conflict rather than silently preferring one.

**How the gizmo's extra attributes map.** `GLGizmoCut3D::perform_cut`
(`GLGizmoCut.cpp:3556-3566`) builds the attribute set from six independent flags. This task
exposes four of them:

| MCP | `ModelObjectCutAttribute` | Meaning |
|---|---|---|
| `keep: "below"` | `KeepLower` | keep the piece behind the plane normal |
| `keep: "above"` | `KeepUpper` | keep the piece the normal points into |
| `keep: "both"` | `KeepUpper \| KeepLower` | keep both |
| `place_on_cut` (default `true`) | `PlaceOnCutUpper` / `PlaceOnCutLower` | rest each kept piece on its cut face |
| `flip` (default `false`) | `FlipUpper` / `FlipLower` | turn each kept piece over |
| `keep_paint` (default `false`) | `KeepPaint` | save and remap painted facets (`CutUtils.cpp:337-347`) |

`place_on_cut` defaults preserve today's behaviour exactly: the shipped tool sets
`PlaceOnCutUpper` for `"above"` and both `PlaceOnCut*` for `"both"`, and neither for
`"below"`. Keep that asymmetry — for `"below"` the lower piece already sits on the bed, and
placing it on its cut face would turn it upside down. So `place_on_cut` is applied to the
upper piece always, and to the lower piece only when the caller asks for a non-default value.
State that in the description and in `reference.md`.

- [ ] **Step 1: Write the failing check**

Rebuild and reload if needed, load the tower, and run:

```
mcp__orca-slicer__cut_object {"object_id": 0, "plane_origin": {"x": 120, "y": 120, "z": 10}, "plane_normal": {"x": 1, "y": 0, "z": 1}}
```
Expected today: an exception from `params.at("z_height")` surfacing as a JSON-RPC error, or a
schema rejection — either way, no diagonal cut.

- [ ] **Step 2: Extend the schema**

In the `cut_object` registration, replace the description and add the new properties:

```cpp
        "cut_object",
        "Cut an object with a plane. Give either z_height (a horizontal cut, the original "
        "call shape) or plane_origin plus plane_normal for an arbitrary plane, both in plate "
        "coordinates. keep: below (default), above, or both -- \"above\" is the side the "
        "normal points into. Painted facets are discarded unless keep_paint is true; the "
        "response always says which happened.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"z_height", {
                    {"type", "number"},
                    {"description", "Horizontal cut height in mm. Equivalent to "
                                    "plane_origin {0,0,z_height} with plane_normal {0,0,1}. "
                                    "Cannot be combined with plane_origin."}
                }},
                {"plane_origin", {
                    {"type", "object"},
                    {"properties", {
                        {"x", {{"type", "number"}}},
                        {"y", {{"type", "number"}}},
                        {"z", {{"type", "number"}}}
                    }},
                    {"required", nlohmann::json::array({"x", "y", "z"})},
                    {"description", "A point the cut plane passes through, in plate coordinates "
                                    "(the space get_object_info reports)"}
                }},
                {"plane_normal", {
                    {"type", "object"},
                    {"properties", {
                        {"x", {{"type", "number"}}},
                        {"y", {{"type", "number"}}},
                        {"z", {{"type", "number"}}}
                    }},
                    {"required", nlohmann::json::array({"x", "y", "z"})},
                    {"description", "The direction the plane faces. Need not be normalised. "
                                    "Defaults to {0,0,1}. \"above\" is the side it points into."}
                }},
                {"keep", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"below", "above", "both"})},
                    {"description", "below (default), above, or both"}
                }},
                {"place_on_cut", {
                    {"type", "boolean"},
                    {"description", "Rest each kept piece on its cut face so it is printable. "
                                    "Default true for the upper piece; the lower piece is left "
                                    "as it is unless this is given explicitly."}
                }},
                {"flip", {
                    {"type", "boolean"},
                    {"description", "Turn each kept piece over. Default false."}
                }},
                {"keep_paint", {
                    {"type", "boolean"},
                    {"description", "Remap painted facets onto the cut meshes. Slow and "
                                    "approximate. Default false, which discards them."}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", nlohmann::json::array({"object_id"})}
        },
```

`z_height` leaves the `required` list; the handler enforces "one of the two" so the error can
explain itself instead of being a schema rejection.

- [ ] **Step 3: Read the plane in the handler**

Replace the parameter extraction at the top of the lambda:

```cpp
        [](const nlohmann::json& params) -> nlohmann::json {
            const int         object_id       = params.at("object_id");
            const std::string keep            = params.value("keep", "below");
            const bool        flip            = params.value("flip", false);
            const bool        keep_paint      = params.value("keep_paint", false);
            const bool        include_preview = params.value("include_preview", false);

            const bool has_z     = params.contains("z_height");
            const bool has_plane = params.contains("plane_origin");
            if (has_z && has_plane)
                return nlohmann::json{{"status", "error"},
                    {"message", "Give either z_height or plane_origin, not both. z_height is a "
                                "horizontal cut; plane_origin with plane_normal is an arbitrary one."}};
            if (!has_z && !has_plane)
                return nlohmann::json{{"status", "error"},
                    {"message", "cut_object needs a plane: either z_height, or plane_origin with "
                                "plane_normal."}};

            Slic3r::Vec3d origin = Slic3r::Vec3d::Zero();
            Slic3r::Vec3d normal = Slic3r::Vec3d::UnitZ();
            if (has_plane) {
                const nlohmann::json& o = params.at("plane_origin");
                origin = Slic3r::Vec3d(o.at("x").get<double>(), o.at("y").get<double>(), o.at("z").get<double>());
                if (params.contains("plane_normal")) {
                    const nlohmann::json& n = params.at("plane_normal");
                    normal = Slic3r::Vec3d(n.at("x").get<double>(), n.at("y").get<double>(), n.at("z").get<double>());
                }
            } else {
                origin = Slic3r::Vec3d(0., 0., params.at("z_height").get<double>());
            }

            // place_on_cut: absent means today's behaviour -- the upper piece is placed on its
            // cut face, the lower piece is left resting on the bed.
            const bool has_place_on_cut  = params.contains("place_on_cut");
            const bool place_on_cut      = params.value("place_on_cut", true);
            const double reported_z      = origin.z();

            return run_on_main_thread([object_id, origin, normal, keep, flip, keep_paint,
                                       has_place_on_cut, place_on_cut, reported_z,
                                       include_preview]() -> nlohmann::json {
```

- [ ] **Step 4: Build the matrix and the attributes**

Inside the lambda, replace the `cut_matrix` construction and the attribute block:

```cpp
                const Slic3r::Vec3d instance_offset = object->instances[0]->get_offset();
                Slic3r::Transform3d cut_matrix;
                if (!cut_plane_matrix(origin, normal, instance_offset, cut_matrix))
                    return nlohmann::json{{"status", "error"},
                        {"message", "plane_normal names no plane: it must be a non-zero, finite vector."}};

                const bool place_upper = has_place_on_cut ? place_on_cut : true;
                const bool place_lower = has_place_on_cut ? place_on_cut : false;

                Slic3r::ModelObjectCutAttributes attributes{};
                if (keep == "above" || keep == "both") {
                    attributes = attributes | Slic3r::ModelObjectCutAttribute::KeepUpper;
                    if (place_upper)
                        attributes = attributes | Slic3r::ModelObjectCutAttribute::PlaceOnCutUpper;
                    if (flip)
                        attributes = attributes | Slic3r::ModelObjectCutAttribute::FlipUpper;
                }
                if (keep == "below" || keep == "both") {
                    attributes = attributes | Slic3r::ModelObjectCutAttribute::KeepLower;
                    if (place_lower)
                        attributes = attributes | Slic3r::ModelObjectCutAttribute::PlaceOnCutLower;
                    if (flip)
                        attributes = attributes | Slic3r::ModelObjectCutAttribute::FlipLower;
                }
                if (keep_paint)
                    attributes = attributes | Slic3r::ModelObjectCutAttribute::KeepPaint;
```

and report the plane, replacing `{"z_height", z_height},` in the result:

```cpp
                    {"z_height", reported_z},
                    {"plane", {
                        {"origin", {{"x", origin.x()}, {"y", origin.y()}, {"z", origin.z()}}},
                        {"normal", {{"x", normal.normalized().x()},
                                    {"y", normal.normalized().y()},
                                    {"z", normal.normalized().z()}}}
                    }},
```

and pass the caller's choice to the paint report:

```cpp
                report_paint(result, messages, had_paint, keep_paint, "cut_object");
```

- [ ] **Step 5: Rebuild and reload, then verify old and new**

Run the rebuild-and-reload recipe. First, the published shapes must still behave:

```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10}
mcp__orca-slicer__undo {}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "keep": "both"}
mcp__orca-slicer__undo {}
```
Expected: `new_objects_count` 1 then 2, `plane.normal` `{0,0,1}`, `z_height: 10` echoed —
identical to Task 5's recorded results.

Then the new capability:
```
mcp__orca-slicer__cut_object {"object_id": 0, "plane_origin": {"x": 120, "y": 120, "z": 10}, "plane_normal": {"x": 1, "y": 0, "z": 1}, "keep": "both"}
```
Expected: `new_objects_count: 2` and, in the app, a visibly diagonal cut. Confirm
`plane.normal` came back normalised (`x ≈ 0.7071, y = 0, z ≈ 0.7071`).

Then the two rejections:
```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "plane_origin": {"x": 0, "y": 0, "z": 10}}
mcp__orca-slicer__cut_object {"object_id": 0, "plane_origin": {"x": 0, "y": 0, "z": 10}, "plane_normal": {"x": 0, "y": 0, "z": 0}}
```
Expected: `status: "error"` both times, with the "not both" and the "names no plane" messages,
and `get_scene_info` unchanged after each.

Finally, paint. Paint the tower with the GUI's colour gizmo (a few facets is enough), then:
```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "keep": "both"}
```
Expected: `paint: {"had_paint": true, "keep_paint": false, "result": "discarded"}` and an
`info_messages` line saying so. Undo, then repeat with `"keep_paint": true` and expect
`result: "remapped"` with paint visible on both halves.

- [ ] **Step 6: Rewrite the `cut_object` doc entry**

`docs/tools/reference.md`, replacing the body of `### cut_object` (lines 364-376):

````markdown
Cut an object with a plane and keep one or both sides.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `z_height` | number | One of | Horizontal cut height in mm — the original call shape |
| `plane_origin` | object | One of | `{x, y, z}`, a point the plane passes through, plate coordinates |
| `plane_normal` | object | No | `{x, y, z}`, the direction the plane faces. Need not be normalised. Default `{0,0,1}` |
| `keep` | string | No | "below", "above", or "both" (default: "below"). "above" is the side `plane_normal` points into |
| `place_on_cut` | boolean | No | Rest each kept piece on its cut face. Default: upper yes, lower no |
| `flip` | boolean | No | Turn each kept piece over. Default `false` |
| `keep_paint` | boolean | No | Remap painted facets onto the cut meshes. Default `false` — paint is discarded |
| `include_preview` | boolean | No | Return a turntable preview path |

Give **either** `z_height` **or** `plane_origin`; supplying both is an error.
`z_height: 25` is exactly `plane_origin: {x:0, y:0, z:25}` with the default normal.

**`place_on_cut` defaults are asymmetric on purpose.** With `keep: "below"` the lower piece is
already sitting on the bed, and placing it on its cut face would turn it upside down — so the
default only places the upper piece. Pass `place_on_cut: true` to place both, or
`place_on_cut: false` to place neither.

**Paint:** see `split_object` — the same `paint` block and the same `keep_paint` default.

**Examples:**
```json
{"name": "cut_object", "arguments": {"object_id": 0, "z_height": 25, "keep": "below"}}
```
```json
{"name": "cut_object", "arguments": {"object_id": 0,
  "plane_origin": {"x": 120, "y": 120, "z": 25},
  "plane_normal": {"x": 1, "y": 0, "z": 1}, "keep": "both"}}
```

**Returns:** `status`, `original_object`, `z_height`, `plane` (the origin and the normalised
normal actually used), `kept`, `new_objects_count`, `paint`, `active_warnings`.
````

- [ ] **Step 7: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp docs/tools/reference.md
git commit -m "$(cat <<'MSG'
feat: cut_object could only ever cut horizontally, and always threw the paint away

The cut gizmo has done arbitrary planes since it shipped; the MCP tool hard-coded a
translation along Z (Geometry::translation_transform of z_height minus the instance offset)
and had no way to express a normal at all, so "cut this at 30 degrees" was unreachable. It
also never set ModelObjectCutAttribute::KeepPaint, so a painted object came out of every
cut unpainted with nothing in the response saying so.

Added plane_origin/plane_normal as an alternative to z_height, plus place_on_cut, flip and
keep_paint, mapping onto the same attribute flags GLGizmoCut3D::perform_cut builds. The
published call shape is untouched: z_height alone still means a horizontal cut with the
same defaults, and supplying both spellings is refused rather than silently resolved.

place_on_cut's default is deliberately asymmetric -- upper yes, lower no -- because that is
what the shipped tool did, and for a reason: with keep "below" the lower piece already rests
on the bed and placing it on its cut face would invert it.

Related occurrences checked: the same plane-from-normal maths is needed by the tongue-and-
groove mode, so it went into OrcaMCPMeshUtils::cut_plane_matrix rather than inline here.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 7: `cut_object` — `keep: "parts"`, one object whose halves are parts

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` (the `cut_object` registration)
- Modify: `docs/tools/reference.md` (the `keep` row and the returns list of `cut_object`)

**Interfaces:**
- Consumes: everything from Task 6.
- Produces: `cut_object`'s `keep` enum gains `"parts"`; the response's `new_objects_count`
  is 1 for that mode and a new `part_count` field reports how many volumes the single result
  object has.

**The attribute.** `ModelObjectCutAttribute::KeepAsParts` (`CutUtils.hpp:13`) makes
`Cut::perform_with_plane` keep both sides as volumes of one object instead of two objects: it
skips cloning the lower object (`CutUtils.cpp:322`) and returns `upper` alone with both
halves' volumes on it (`CutUtils.cpp:361-372`). `GLGizmoCut3D::perform_cut` sets it alongside
`KeepUpper` and `KeepLower` (`GLGizmoCut.cpp:3557-3559`), and disables it when connectors are
present — connectors need two objects to join.

**Why an agent wants it.** It is the cheapest route to a painted two-tone object: cut a model
in half as parts, then `set_object_filament` each `volume_id`. It complements Task 3's
`split_object mode: "parts"`, which only separates bodies that were already disconnected.

- [ ] **Step 1: Write the failing check**

Rebuild and reload if needed. Load the tower and run:

```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "keep": "parts"}
```
Expected today: the schema's `enum` rejects `"parts"`, or the handler falls through to the
`"below"` branch and produces a single lower piece — either way, not two parts.

- [ ] **Step 2: Extend the enum and the attribute mapping**

In the `cut_object` schema, the `keep` property:

```cpp
                {"keep", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"below", "above", "both", "parts"})},
                    {"description", "below (default), above, both (two objects), or parts "
                                    "(one object whose two halves are parts, addressable by "
                                    "volume_id)"}
                }},
```

In the handler, replace the attribute block from Task 6 with:

```cpp
                const bool as_parts = (keep == "parts");

                const bool place_upper = has_place_on_cut ? place_on_cut : true;
                const bool place_lower = has_place_on_cut ? place_on_cut : false;

                Slic3r::ModelObjectCutAttributes attributes{};
                if (keep == "above" || keep == "both" || as_parts) {
                    attributes = attributes | Slic3r::ModelObjectCutAttribute::KeepUpper;
                    if (place_upper && !as_parts)
                        attributes = attributes | Slic3r::ModelObjectCutAttribute::PlaceOnCutUpper;
                    if (flip && !as_parts)
                        attributes = attributes | Slic3r::ModelObjectCutAttribute::FlipUpper;
                }
                if (keep == "below" || keep == "both" || as_parts) {
                    attributes = attributes | Slic3r::ModelObjectCutAttribute::KeepLower;
                    if (place_lower && !as_parts)
                        attributes = attributes | Slic3r::ModelObjectCutAttribute::PlaceOnCutLower;
                    if (flip && !as_parts)
                        attributes = attributes | Slic3r::ModelObjectCutAttribute::FlipLower;
                }
                if (as_parts)
                    attributes = attributes | Slic3r::ModelObjectCutAttribute::KeepAsParts;
                if (keep_paint)
                    attributes = attributes | Slic3r::ModelObjectCutAttribute::KeepPaint;
```

`place_on_cut` and `flip` are suppressed for `"parts"` because the halves stay assembled in
one object — moving one of them apart is what `keep: "both"` is for. Say so in the doc.

- [ ] **Step 3: Report the part count**

After `plater->apply_cut_object_to_model(...)` in the handler, before the result is built:

```cpp
                // KeepAsParts returns a single object carrying both halves as volumes
                // (CutUtils.cpp:361-372); apply_cut_object_to_model appended it last.
                const int part_count = model.objects.empty() ? 0
                    : static_cast<int>(model.objects.back()->volumes.size());
```

and add to the result, after `{"new_objects_count", count}`:

```cpp
                if (as_parts)
                    result["part_count"] = part_count;
```

- [ ] **Step 4: Rebuild and reload, then verify**

Run the rebuild-and-reload recipe. Load the tower and run:

```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "keep": "parts"}
mcp__orca-slicer__get_scene_info {}
mcp__orca-slicer__get_object_info {"object_id": 0}
```
Expected: `new_objects_count: 1`, `part_count: 2`; the scene holds one object; and
`get_object_info` (Task 4) lists two `parts` with `volume_id` 0 and 1.

Then the point of it:
```
mcp__orca-slicer__set_object_filament {"object_id": 0, "volume_id": 1, "filament": 2}
```
Expected: the upper half changes colour in the app and the lower does not.

Then confirm the other keep values are untouched:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__undo {}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "keep": "both"}
```
Expected: `new_objects_count: 2`, no `part_count` field.

- [ ] **Step 5: Update the doc**

`docs/tools/reference.md`, the `cut_object` `keep` row:

```markdown
| `keep` | string | No | "below" (default), "above", "both" (two objects), or "parts" (one object, two parts) |
```

and after the `place_on_cut` paragraph:

```markdown
**`keep: "parts"`** returns one object whose two halves are parts, addressable by
`volume_id` — the fastest route to a two-tone print: cut, then `set_object_filament` each
part. `place_on_cut` and `flip` are ignored in this mode, because the halves stay assembled;
use `keep: "both"` when they should become separate, separately placeable objects. The
response carries `part_count` instead of a meaningful `new_objects_count`.
```

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp docs/tools/reference.md
git commit -m "$(cat <<'MSG'
feat: cutting a model in two always produced two objects, never two parts

The cut gizmo's "keep as parts" checkbox maps to ModelObjectCutAttribute::KeepAsParts,
which makes Cut::perform_with_plane skip cloning the lower object and return one object
carrying both halves as volumes. MCP had no spelling for it, so the cheapest route to a
two-tone print -- cut in half, colour each half -- needed two objects and manual
re-alignment that the API cannot express.

place_on_cut and flip are suppressed in this mode on purpose: the halves stay assembled,
so re-seating one of them on its cut face would separate what the mode exists to keep
together. GLGizmoCut3D disables the checkbox when connectors are present for the same
reason, and the connector work in the next commit follows that.

Related occurrences checked: split_object mode "parts" (already shipped in this batch)
covers the other way to get parts -- separating bodies that were already disconnected.
The two are complementary and neither subsumes the other.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 8: `cut_object` — connectors (plug, dowel, snap)

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp` / `.cpp` (connector attribute parsing and mesh)
- Modify: `tests/slic3rutils/test_mesh_edit.cpp` (their coverage)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` (the `cut_object` registration)
- Modify: `docs/tools/reference.md` (the `cut_object` entry)

**Interfaces:**
- Consumes: `cut_plane_matrix` (Task 1).
- Produces:
  ```cpp
  // OrcaMCPMeshUtils.hpp
  bool parse_connector_attributes(const std::string& type,
                                  const std::string& style,
                                  const std::string& shape,
                                  CutConnectorAttributes& out,
                                  std::string& error);
  indexed_triangle_set connector_mesh(const CutConnectorAttributes& attribs,
                                      float snap_space_proportion = 0.3f,
                                      float snap_bulge_proportion = 0.15f);
  ```
  and `cut_object` gains a `connectors` array parameter.

**The mechanism, verified.** Connectors are not a `Cut` parameter — they are
`ModelObject::cut_connectors` (`Model.hpp:402`), turned into `NEGATIVE_VOLUME` volumes with a
`cut_info` before the cut runs, by `GLGizmoCut3D::apply_cut_connectors`
(`GLGizmoCut.cpp:4041-4066`). `Cut::perform_with_plane` then routes any volume that is not a
model part through `process_connector_cut` (`CutUtils.cpp:349-350`), which is what makes the
male peg on one half and the female socket on the other.

**Coordinate space.** `CutConnector::pos` is the world hit point minus the instance offset —
`unproject_on_cut_plane` says so explicitly ("recalculate hit to object's local position",
`GLGizmoCut.cpp:3681-3684`), and it is the same space `cut_plane_matrix` puts the plane origin
in. So the MCP parameter is a plate-coordinate point on the cut plane, converted the same way.

**The pre-cut adjustment.** `GLGizmoCut3D::perform_cut` (`GLGizmoCut.cpp:3430-3440`) does two
things to each connector before `apply_cut_connectors`:
- sets `connector.rotation_m` to the cut plane's rotation;
- for a **Dowel**: doubles the height if the style is `Prism`, and counts it towards
  `dowels_count`, which sets `ModelObjectCutAttribute::CreateDowels` (`GLGizmoCut.cpp:3563`)
  and makes each dowel a separate object;
- for anything else: shifts the position half a height along the cut normal, so the connector
  straddles the plane.

**Sizes.** `m_connector_size` is a **diameter**; the radius passed to `CutConnector` is half of
it (`GLGizmoCut.cpp:3829`). Same for the size tolerance. GUI defaults, from
`GLGizmoCut.hpp:141-146`: size 2.5 mm, depth 3.0 mm, size tolerance 0.0, depth tolerance 0.1,
angle 0. Snap proportions default to 0.3 and 0.15 (`GLGizmoCut.hpp:134-135`).

**Enum spellings** (`Model.hpp:250-271`): type `Plug | Dowel | Snap`, style `Prism | Frustum`,
shape `Triangle | Square | Hexagon | Circle`. Note the enum spells it `Prism`, while the dead
`GLGizmoAdvancedCut.cpp` spells it `Prizm` — another reason not to read that file.

**Out of scope, and say so in the doc:** validating that a connector actually lies inside the
cut contour. The gizmo does this with a raycast against the clipper contour
(`GLGizmoCut3D::is_outside_of_cut_contour`), which needs an object clipper the HTTP worker has
no access to. A connector placed off the cut face produces a bad cut rather than an error.

- [ ] **Step 1: Write the failing tests**

Append to `tests/slic3rutils/test_mesh_edit.cpp`:

```cpp
TEST_CASE("parse_connector_attributes accepts the vocabulary the tool advertises",
          "[orcamcp][mesh]")
{
    CutConnectorAttributes attribs;
    std::string            error;

    REQUIRE(parse_connector_attributes("plug", "prism", "circle", attribs, error));
    CHECK(attribs.type  == CutConnectorType::Plug);
    CHECK(attribs.style == CutConnectorStyle::Prism);
    CHECK(attribs.shape == CutConnectorShape::Circle);
    CHECK(error.empty());

    REQUIRE(parse_connector_attributes("dowel", "frustum", "hexagon", attribs, error));
    CHECK(attribs.type  == CutConnectorType::Dowel);
    CHECK(attribs.style == CutConnectorStyle::Frustum);
    CHECK(attribs.shape == CutConnectorShape::Hexagon);

    REQUIRE(parse_connector_attributes("snap", "prism", "square", attribs, error));
    CHECK(attribs.type  == CutConnectorType::Snap);
    CHECK(attribs.shape == CutConnectorShape::Square);

    REQUIRE(parse_connector_attributes("plug", "prism", "triangle", attribs, error));
    CHECK(attribs.shape == CutConnectorShape::Triangle);
}

TEST_CASE("parse_connector_attributes names what it did not understand", "[orcamcp][mesh]")
{
    CutConnectorAttributes attribs;
    std::string            error;

    CHECK_FALSE(parse_connector_attributes("peg", "prism", "circle", attribs, error));
    CHECK(error.find("peg") != std::string::npos);
    CHECK(error.find("plug") != std::string::npos);   // says what is accepted

    CHECK_FALSE(parse_connector_attributes("plug", "prizm", "circle", attribs, error));
    CHECK(error.find("prizm") != std::string::npos);  // the dead gizmo's misspelling

    CHECK_FALSE(parse_connector_attributes("plug", "prism", "octagon", attribs, error));
    CHECK(error.find("octagon") != std::string::npos);
}

TEST_CASE("connector_mesh builds a unit solid for every attribute combination",
          "[orcamcp][mesh]")
{
    // apply_cut_connectors scales a unit mesh by (radius, radius, height)
    // (GLGizmoCut.cpp:4055-4057), so every combination must produce a non-degenerate solid.
    for (const CutConnectorType type : {CutConnectorType::Plug, CutConnectorType::Dowel,
                                        CutConnectorType::Snap})
        for (const CutConnectorStyle style : {CutConnectorStyle::Prism, CutConnectorStyle::Frustum})
            for (const CutConnectorShape shape : {CutConnectorShape::Triangle,
                                                  CutConnectorShape::Square,
                                                  CutConnectorShape::Hexagon,
                                                  CutConnectorShape::Circle}) {
                const indexed_triangle_set its = connector_mesh({type, style, shape});
                INFO("type " << int(type) << " style " << int(style) << " shape " << int(shape));
                CHECK(its.vertices.size() >= 4);
                CHECK(its.indices.size()  >= 4);
            }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
```
Expected: FAIL — `use of undeclared identifier 'parse_connector_attributes'`.

- [ ] **Step 3: Declare them**

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp`, before the closing namespace:

```cpp
// The three connector enums (Model.hpp:250-271) from the lowercase words the tool advertises.
// Returns false with a message naming the word it did not understand and the words it accepts.
// Note the enum spells the style "Prism"; the uncompiled GLGizmoAdvancedCut.cpp spells it
// "Prizm", which is not accepted.
bool parse_connector_attributes(const std::string&      type,
                                const std::string&      style,
                                const std::string&      shape,
                                CutConnectorAttributes& out,
                                std::string&            error);

// The unit connector solid for a set of attributes -- radius 1, height 1, to be scaled by
// (radius, radius, height) the way apply_cut_connectors does (GLGizmoCut.cpp:4055-4057).
// Mirrors GLGizmoCut3D::get_connector_mesh (GLGizmoCut.cpp:4007-4038), which is a private
// member of a gizmo and so unreachable from a tool handler.
indexed_triangle_set connector_mesh(const CutConnectorAttributes& attribs,
                                    float snap_space_proportion = 0.3f,
                                    float snap_bulge_proportion = 0.15f);
```

- [ ] **Step 4: Implement them**

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp` — add `#include "libslic3r/TriangleMesh.hpp"`
and `#include <map>` to the includes, then:

```cpp
namespace {

template<typename T>
bool lookup(const std::map<std::string, T>& table,
            const std::string&              word,
            const std::string&              field,
            T&                              out,
            std::string&                    error)
{
    const auto it = table.find(word);
    if (it != table.end()) {
        out = it->second;
        return true;
    }
    std::string accepted;
    for (const auto& entry : table)
        accepted += (accepted.empty() ? "" : ", ") + entry.first;
    error = "connector " + field + " \"" + word + "\" is not one of: " + accepted;
    return false;
}

} // namespace

bool parse_connector_attributes(const std::string&      type,
                                const std::string&      style,
                                const std::string&      shape,
                                CutConnectorAttributes& out,
                                std::string&            error)
{
    static const std::map<std::string, CutConnectorType> types = {
        {"dowel", CutConnectorType::Dowel},
        {"plug",  CutConnectorType::Plug},
        {"snap",  CutConnectorType::Snap},
    };
    static const std::map<std::string, CutConnectorStyle> styles = {
        {"frustum", CutConnectorStyle::Frustum},
        {"prism",   CutConnectorStyle::Prism},
    };
    static const std::map<std::string, CutConnectorShape> shapes = {
        {"circle",   CutConnectorShape::Circle},
        {"hexagon",  CutConnectorShape::Hexagon},
        {"square",   CutConnectorShape::Square},
        {"triangle", CutConnectorShape::Triangle},
    };

    error.clear();
    CutConnectorType  t = CutConnectorType::Plug;
    CutConnectorStyle s = CutConnectorStyle::Prism;
    CutConnectorShape h = CutConnectorShape::Circle;
    if (!lookup(types, type, "type", t, error))   return false;
    if (!lookup(styles, style, "style", s, error)) return false;
    if (!lookup(shapes, shape, "shape", h, error)) return false;

    out = CutConnectorAttributes(t, s, h);
    return true;
}

indexed_triangle_set connector_mesh(const CutConnectorAttributes& attribs,
                                    float snap_space_proportion,
                                    float snap_bulge_proportion)
{
    int sector_count = 1;
    switch (attribs.shape) {
    case CutConnectorShape::Triangle: sector_count = 3;   break;
    case CutConnectorShape::Square:   sector_count = 4;   break;
    case CutConnectorShape::Hexagon:  sector_count = 6;   break;
    case CutConnectorShape::Circle:   sector_count = 360; break;
    default:                          break;
    }

    if (attribs.type == CutConnectorType::Snap)
        return its_make_snap(1.0, 1.0, snap_space_proportion, snap_bulge_proportion);
    if (attribs.style == CutConnectorStyle::Prism)
        return its_make_cylinder(1.0, 1.0, (2 * PI / sector_count));
    if (attribs.type == CutConnectorType::Plug)
        return its_make_frustum(1.0, 1.0, (2 * PI / sector_count));
    return its_make_frustum_dowel(1.0, 1.0, sector_count);
}
```

`its_make_cylinder` / `its_make_frustum` / `its_make_frustum_dowel` / `its_make_snap` are
declared at `TriangleMesh.hpp:338-345`.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[mesh]"
```
Expected: PASS, 10 cases.

- [ ] **Step 6: Add the `connectors` parameter to `cut_object`**

In the schema, after `flip`:

```cpp
                {"connectors", {
                    {"type", "array"},
                    {"description", "Joints to build into the cut. Each connector is a point ON "
                                    "the cut plane, in plate coordinates. Plugs and snaps join "
                                    "the two halves directly; dowels become separate objects "
                                    "you print and insert."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"x", {{"type", "number"}}},
                            {"y", {{"type", "number"}}},
                            {"z", {{"type", "number"}}},
                            {"type", {{"type", "string"},
                                      {"enum", nlohmann::json::array({"plug", "dowel", "snap"})},
                                      {"description", "Default plug"}}},
                            {"style", {{"type", "string"},
                                       {"enum", nlohmann::json::array({"prism", "frustum"})},
                                       {"description", "Default prism"}}},
                            {"shape", {{"type", "string"},
                                       {"enum", nlohmann::json::array({"circle", "hexagon", "square", "triangle"})},
                                       {"description", "Default circle"}}},
                            {"size", {{"type", "number"}, {"description", "Diameter in mm, default 2.5"}}},
                            {"depth", {{"type", "number"}, {"description", "Height in mm, default 3.0"}}},
                            {"size_tolerance", {{"type", "number"}, {"description", "Default 0.0"}}},
                            {"depth_tolerance", {{"type", "number"}, {"description", "Default 0.1"}}},
                            {"angle_deg", {{"type", "number"}, {"description", "Rotation about the connector axis, default 0"}}}
                        }},
                        {"required", nlohmann::json::array({"x", "y", "z"})}
                    }}
                }},
```

- [ ] **Step 7: Build the connectors in the handler**

Add to the file's includes:

```cpp
#include "libslic3r/TriangleMesh.hpp"
```

Read the array before entering `run_on_main_thread` — it is plain JSON, no GUI needed. Add
after the `plane_normal` parsing in Task 6's Step 3 block:

```cpp
            const nlohmann::json connectors_json =
                params.contains("connectors") ? params.at("connectors") : nlohmann::json::array();
```

and capture `connectors_json` in the lambda. Then, inside the lambda, immediately after
`cut_plane_matrix` succeeds and **before** the attribute block:

```cpp
                // Connectors are not a Cut parameter: they are ModelObject::cut_connectors
                // turned into NEGATIVE_VOLUME volumes before the cut, exactly as
                // GLGizmoCut3D::perform_cut + apply_cut_connectors do
                // (GLGizmoCut.cpp:3430-3440 and 4041-4066).
                const Slic3r::Vec3d unit_normal = normal.normalized();
                int  dowels_count = 0;
                object->cut_connectors.clear();
                for (const nlohmann::json& c : connectors_json) {
                    Slic3r::CutConnectorAttributes attribs;
                    std::string                    error;
                    if (!parse_connector_attributes(c.value("type", "plug"),
                                                    c.value("style", "prism"),
                                                    c.value("shape", "circle"),
                                                    attribs, error)) {
                        object->cut_connectors.clear();
                        return nlohmann::json{{"status", "error"}, {"message", error}};
                    }

                    // GUI defaults, GLGizmoCut.hpp:141-146. size is a diameter; CutConnector
                    // takes a radius (GLGizmoCut.cpp:3829).
                    const float size            = c.value("size", 2.5f);
                    const float depth           = c.value("depth", 3.0f);
                    const float size_tolerance  = c.value("size_tolerance", 0.0f);
                    const float depth_tolerance = c.value("depth_tolerance", 0.1f);
                    const float angle           = static_cast<float>(
                        Slic3r::Geometry::deg2rad(c.value("angle_deg", 0.0)));
                    if (size <= 0.f || depth <= 0.f) {
                        object->cut_connectors.clear();
                        return nlohmann::json{{"status", "error"},
                            {"message", "connector size and depth must both be greater than zero"}};
                    }

                    // pos is the point minus the instance offset, the space
                    // unproject_on_cut_plane produces (GLGizmoCut.cpp:3681-3684).
                    Slic3r::Vec3d pos(c.at("x").get<double>(),
                                      c.at("y").get<double>(),
                                      c.at("z").get<double>());
                    pos -= instance_offset;

                    float height = depth;
                    if (attribs.type == Slic3r::CutConnectorType::Dowel) {
                        if (attribs.style == Slic3r::CutConnectorStyle::Prism)
                            height *= 2.f;
                        ++dowels_count;
                    } else {
                        // Straddle the plane rather than sit on it.
                        pos += unit_normal * 0.5 * double(height);
                    }

                    object->cut_connectors.emplace_back(pos, cut_matrix.rotation(),
                                                        size * 0.5f, height,
                                                        size_tolerance * 0.5f, depth_tolerance,
                                                        angle, attribs);
                }

                const bool has_connectors = !object->cut_connectors.empty();

                // Turn the connectors into negative volumes on the object. This mutates the
                // model, so the snapshot has to come first -- earlier than it does for a
                // plain cut.
                if (has_connectors) {
                    plater->take_snapshot("Cut by Plane");
                    size_t connector_id = object->cut_id.connectors_cnt();
                    for (const Slic3r::CutConnector& connector : object->cut_connectors) {
                        Slic3r::TriangleMesh mesh(connector_mesh(connector.attribs));
                        Slic3r::ModelVolume* volume = object->add_volume(
                            std::move(mesh), Slic3r::ModelVolumeType::NEGATIVE_VOLUME);
                        volume->set_transformation(
                            Slic3r::Geometry::translation_transform(connector.pos) *
                            connector.rotation_m *
                            Slic3r::Geometry::rotation_transform(-connector.z_angle * Slic3r::Vec3d::UnitZ()) *
                            Slic3r::Geometry::scale_transform(Slic3r::Vec3f(connector.radius,
                                                                            connector.radius,
                                                                            connector.height).cast<double>()));
                        volume->cut_info = {connector.attribs.type,
                                            connector.radius_tolerance,
                                            connector.height_tolerance};
                        volume->name = "Connector-" + std::to_string(++connector_id);
                    }
                    object->cut_id.increase_connectors_cnt(object->cut_connectors.size());
                    object->cut_connectors.clear();
                }
```

`cut_matrix.rotation()` gives the plane's rotation without the translation, which is what
`connector.rotation_m` is (`GLGizmoCut.cpp:3433` assigns `m_rotation_m`).

In the attribute block, force both sides kept when connectors are present — the gizmo does the
same (`GLGizmoCut.cpp:3556-3557`, `has_connectors ? true : m_keep_upper`) — and add
`CreateDowels`:

```cpp
                const bool as_parts = (keep == "parts") && !has_connectors;
                const bool want_upper = has_connectors || keep == "above" || keep == "both" || as_parts;
                const bool want_lower = has_connectors || keep == "below" || keep == "both" || as_parts;
```
(use `want_upper` / `want_lower` in place of the `keep == ...` tests from Task 7), and after
the `KeepPaint` line:

```cpp
                if (dowels_count > 0)
                    attributes = attributes | Slic3r::ModelObjectCutAttribute::CreateDowels;
```

Finally, guard the second snapshot so it is not taken twice:

```cpp
                if (!has_connectors)
                    plater->take_snapshot("Cut by Plane");
```

and report what was built, after `{"new_objects_count", count}`:

```cpp
                result["connectors"] = {
                    {"count", static_cast<int>(connectors_json.size())},
                    {"dowels", dowels_count}
                };
```

- [ ] **Step 8: Rebuild and reload, then verify**

Run the rebuild-and-reload recipe. Load the tower, note its footprint from
`get_object_info` (`bounding_box.min` / `max`), and cut it with two plugs placed inside that
footprint on the cut plane:

```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "keep": "both",
  "connectors": [{"x": 118, "y": 120, "z": 10}, {"x": 122, "y": 120, "z": 10}]}
```
Expected: `status: "success"`, `new_objects_count: 2`, `connectors: {"count": 2, "dowels": 0}`,
and in the app a peg on one half and a matching socket on the other.

Dowels:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "keep": "both",
  "connectors": [{"x": 120, "y": 120, "z": 10, "type": "dowel", "size": 3, "depth": 4}]}
```
Expected: `connectors: {"count": 1, "dowels": 1}` and `new_objects_count: 3` — the two halves
plus the dowel as its own object, named with a `-Dowel-` suffix (`CutUtils.cpp:392`).

Snap, and each shape:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "keep": "both",
  "connectors": [{"x": 120, "y": 120, "z": 10, "type": "snap", "shape": "hexagon"}]}
```
Expected: success, and a hexagonal snap joint visible.

The rejection, and that it changes nothing:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10,
  "connectors": [{"x": 120, "y": 120, "z": 10, "style": "prizm"}]}
mcp__orca-slicer__get_object_info {"object_id": 0}
```
Expected: `status: "error"` naming `prizm`, and `part_count: 1` — no stray connector volume
was left on the object.

And that undo still works with connectors:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__get_scene_info {}
```
Expected: the uncut, connector-free object.

- [ ] **Step 9: Update the doc**

`docs/tools/reference.md`, in the `cut_object` entry, add the parameter row:

```markdown
| `connectors` | array | No | Joints built into the cut. See below |
```

and after the `keep: "parts"` paragraph:

```markdown
**Connectors** turn a cut into a joint. Each entry is a point **on the cut plane** in plate
coordinates plus a shape:

| Field | Default | Meaning |
|---|---|---|
| `x`, `y`, `z` | — | required; a point on the cut plane |
| `type` | `"plug"` | `plug` (peg + socket), `dowel` (a separate pin object to print and insert), `snap` |
| `style` | `"prism"` | `prism` (straight sides) or `frustum` (tapered) |
| `shape` | `"circle"` | `circle`, `hexagon`, `square`, `triangle` |
| `size` | `2.5` | diameter in mm |
| `depth` | `3.0` | height in mm |
| `size_tolerance` | `0.0` | clearance on the diameter |
| `depth_tolerance` | `0.1` | clearance on the height |
| `angle_deg` | `0` | rotation about the connector's own axis |

Passing any connector forces `keep` to behave as `"both"` — a joint needs two halves — and
`keep: "parts"` is ignored. Each dowel becomes its own object, so `new_objects_count` counts
them; the response's `connectors` block reports `count` and `dowels`.

**Not validated:** whether a connector actually lands inside the cut face. The gizmo checks
this by raycasting the cut contour, which needs a canvas; an MCP call cannot. A connector
placed off the cut face produces a bad cut, not an error — read the object's bounding box
first and place connectors well inside it.
```

- [ ] **Step 10: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp \
        tests/slic3rutils/test_mesh_edit.cpp docs/tools/reference.md
git commit -m "$(cat <<'MSG'
feat: a cut over MCP could never be a joint, only a break

Connectors are not a Cut parameter, which is why the MCP tool never had them: they are
ModelObject::cut_connectors turned into NEGATIVE_VOLUME volumes with a cut_info before the
cut runs, and the code that does that -- GLGizmoCut3D::apply_cut_connectors, and the mesh
factory it calls -- is private to a gizmo that needs a canvas selection. Reimplemented the
two pieces as free functions in OrcaMCPMeshUtils with Catch2 coverage over every
type/style/shape combination, and drove them from the tool.

The pre-cut adjustments the gizmo makes are reproduced deliberately, not by accident: a
Prism dowel's height is doubled, dowels set CreateDowels so each becomes its own object,
and every non-dowel connector is shifted half a height along the cut normal so it straddles
the plane instead of sitting on it. Connector positions are converted the same way
unproject_on_cut_plane converts a mouse hit -- world point minus the instance offset.

Deliberately not implemented: the gizmo's is_outside_of_cut_contour check, which raycasts
the clipper contour. There is no clipper without a canvas, so the doc says a connector off
the cut face produces a bad cut rather than an error.

Related occurrences checked: GLGizmoAdvancedCut.cpp has its own connector code and was
ignored -- it is not compiled and calls ModelObject::get_connector_mesh, which does not
exist. Its style enum is also misspelled "Prizm"; the parser accepts only "prism" and says
so when it sees the other.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 9: `cut_object` — tongue-and-groove (the dovetail joint)

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` (the `cut_object` registration)
- Modify: `docs/tools/reference.md` (the `cut_object` entry)

**Interfaces:**
- Consumes: everything from Tasks 6-8.
- Produces: `cut_object` gains a `groove` object parameter
  `{depth, width, flaps_angle_deg, angle_deg, depth_tolerance, width_tolerance, count, gap}`.
  Present ⇒ the cut runs through `Cut::perform_with_groove` instead of `perform_with_plane`.

**The API.** `Cut::perform_with_groove(const Groove& groove, const Transform3d& rotation_m,
int groove_count, float groove_gap, float m_radius, bool keep_as_parts = false)`
(`CutUtils.hpp:62-67`). `GLGizmoCut3D::perform_cut` selects it when
`CutMode(m_mode) == CutMode::cutTongueAndGroove` (`GLGizmoCut.cpp:3572-3573`).

`Cut::Groove` (`CutUtils.hpp:37-49`) carries ten fields, but `perform_with_groove` reads only
six of them — `depth`, `width`, `flaps_angle`, `angle`, `depth_tolerance`, `width_tolerance`
(`CutUtils.cpp:590`, `681-716`). The four `*_init` fields exist for the gizmo's "reset to
default" buttons and are never read by the cut. Leave them zero.

`flaps_angle` and `angle` are **radians**; the gizmo's own defaults are `PI/3` and `0`
(`GLGizmoCut.cpp:1928-1929`). Expose them as degrees, because every other angle in this API is
in degrees (`rotate_object`, `get_object_info.rotation_degrees`).

**`depth` and `width` must be supplied by the caller.** The gizmo derives them from
`get_grabber_mean_size(m_bounding_box)`, which under `ENABLE_FIXED_GRABBER` — defined to 1 at
`GLGizmoBase.hpp:17` — returns `32.0 * GLGizmoBase::INV_ZOOM` (`GLGizmoCut.cpp:737-745`). That
is a screen-space quantity: the gizmo's default groove depth depends on the camera zoom. There
is no meaningful headless equivalent, so requiring them is more honest than inventing one.
Every other groove field defaults to the value the gizmo or the struct uses:
`flaps_angle_deg` 60, `angle_deg` 0, `depth_tolerance` 0.1 and `width_tolerance` 0.1
(`CutUtils.hpp:47-48`), `count` 1 and `gap` 10 (`GLGizmoCut.hpp:129-130`).

**`m_radius`** is `box.radius()` of the object's transformed bounding box
(`GLGizmoCut.cpp:1916`). Headless, `object->instance_bounding_box(0).radius()` is the same
quantity for the instance being cut.

**Mutually exclusive with connectors.** The gizmo cannot have both — the mode switch is
exclusive, and `perform_with_groove` ignores `cut_connectors` entirely. Reject the combination
rather than silently dropping one.

- [ ] **Step 1: Write the failing check**

Rebuild and reload if needed. Load the tower and run:

```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "groove": {"depth": 2, "width": 8}}
```
Expected today: the `groove` key is ignored and a plain planar cut happens — a flat face where
a dovetail was asked for.

- [ ] **Step 2: Add the schema**

In the `cut_object` schema, after `connectors`:

```cpp
                {"groove", {
                    {"type", "object"},
                    {"description", "Cut with a tongue-and-groove (dovetail) joint instead of a "
                                    "flat plane. depth and width are required: the GUI derives "
                                    "them from the camera zoom, which has no headless meaning. "
                                    "Cannot be combined with connectors."},
                    {"properties", {
                        {"depth", {{"type", "number"}, {"description", "Groove depth in mm (required)"}}},
                        {"width", {{"type", "number"}, {"description", "Groove width in mm (required)"}}},
                        {"flaps_angle_deg", {{"type", "number"}, {"description", "Dovetail flare, 30-120, default 60"}}},
                        {"angle_deg", {{"type", "number"}, {"description", "Groove lean, 0-15, default 0"}}},
                        {"depth_tolerance", {{"type", "number"}, {"description", "Default 0.1"}}},
                        {"width_tolerance", {{"type", "number"}, {"description", "Default 0.1"}}},
                        {"count", {{"type", "integer"}, {"description", "Number of grooves, default 1"}}},
                        {"gap", {{"type", "number"}, {"description", "Distance between grooves in mm, default 10"}}}
                    }},
                    {"required", nlohmann::json::array({"depth", "width"})}
                }},
```

- [ ] **Step 3: Read it before the lambda**

Beside the `connectors_json` line from Task 8:

```cpp
            const bool has_groove = params.contains("groove");
            if (has_groove && !connectors_json.empty())
                return nlohmann::json{{"status", "error"},
                    {"message", "groove and connectors are alternatives: a tongue-and-groove cut "
                                "shapes the joint itself and ignores connectors. Pick one."}};

            Slic3r::Cut::Groove groove;
            int   groove_count = 1;
            float groove_gap   = 10.f;
            if (has_groove) {
                const nlohmann::json& g = params.at("groove");
                groove.depth           = g.at("depth").get<float>();
                groove.width           = g.at("width").get<float>();
                groove.flaps_angle     = static_cast<float>(Slic3r::Geometry::deg2rad(g.value("flaps_angle_deg", 60.0)));
                groove.angle           = static_cast<float>(Slic3r::Geometry::deg2rad(g.value("angle_deg", 0.0)));
                groove.depth_tolerance = g.value("depth_tolerance", 0.1f);
                groove.width_tolerance = g.value("width_tolerance", 0.1f);
                groove_count           = g.value("count", 1);
                groove_gap             = g.value("gap", 10.f);
                if (groove.depth <= 0.f || groove.width <= 0.f)
                    return nlohmann::json{{"status", "error"},
                        {"message", "groove depth and width must both be greater than zero"}};
                if (groove_count < 1)
                    return nlohmann::json{{"status", "error"},
                        {"message", "groove count must be at least 1"}};
            }
```

Capture `has_groove`, `groove`, `groove_count`, `groove_gap` in the lambda.

- [ ] **Step 4: Route the cut**

In the lambda, replace the single `cut.perform_with_plane()` call:

```cpp
                Slic3r::Cut cut(object, 0, cut_matrix, attributes);
                const Slic3r::ModelObjectPtrs& new_objects =
                    has_groove
                        ? cut.perform_with_groove(groove, cut_matrix.rotation(), groove_count,
                                                  groove_gap,
                                                  static_cast<float>(object->instance_bounding_box(0).radius()),
                                                  /*keep_as_parts=*/as_parts)
                        : cut.perform_with_plane();
                const int count = static_cast<int>(new_objects.size());
```

`instance_bounding_box(0)` matches the gizmo's `m_radius = box.radius()`
(`GLGizmoCut.cpp:1916`), which is taken from the transformed bounding box of the object being
cut.

A tongue-and-groove cut always keeps both halves — that is what a joint is — so force it in the
attribute block beside the connector case:

```cpp
                const bool want_upper = has_connectors || has_groove || keep == "above" || keep == "both" || as_parts;
                const bool want_lower = has_connectors || has_groove || keep == "below" || keep == "both" || as_parts;
```

and report it, beside the `connectors` block:

```cpp
                if (has_groove)
                    result["groove"] = {
                        {"depth", groove.depth},
                        {"width", groove.width},
                        {"count", groove_count},
                        {"gap", groove_gap}
                    };
```

- [ ] **Step 5: Rebuild and reload, then verify**

Run the rebuild-and-reload recipe. Load the tower, read its size from `get_object_info`, and
cut it with a groove roughly a fifth of its width:

```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10,
  "groove": {"depth": 2, "width": 8}}
```
Expected: `status: "success"`, `new_objects_count: 2`, a `groove` block echoing the values, and
in the app a dovetailed joint face rather than a flat one — the two halves visibly interlock.

Multiple grooves:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10,
  "groove": {"depth": 2, "width": 5, "count": 2, "gap": 8}}
```
Expected: two dovetails across the joint face.

Flare and lean:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10,
  "groove": {"depth": 2, "width": 8, "flaps_angle_deg": 45, "angle_deg": 5}}
```
Expected: success, and a visibly narrower flare than the 60-degree default.

The rejections:
```
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10,
  "groove": {"depth": 2, "width": 8}, "connectors": [{"x": 120, "y": 120, "z": 10}]}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "groove": {"depth": 0, "width": 8}}
```
Expected: `status: "error"` with the "alternatives" and the "greater than zero" messages, and
`get_scene_info` unchanged after each.

And undo:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__get_scene_info {}
```
Expected: the uncut object.

- [ ] **Step 6: Update the doc**

`docs/tools/reference.md`, in the `cut_object` entry, the parameter row:

```markdown
| `groove` | object | No | Cut a tongue-and-groove (dovetail) joint. See below |
```

and after the connectors section:

```markdown
**Tongue and groove** shapes the joint into the cut face itself — a dovetail rather than a
peg. `depth` and `width` are required, in mm: the GUI derives them from the camera zoom
(`32 / zoom`), which has no headless meaning, so guessing them here would be worse than
asking. Everything else defaults to the GUI's own values.

| Field | Default | Meaning |
|---|---|---|
| `depth` | — | required; how far the groove cuts into each half |
| `width` | — | required; the groove's neck width |
| `flaps_angle_deg` | `60` | the dovetail flare. The gizmo's slider allows 30–120 |
| `angle_deg` | `0` | the groove's lean. The gizmo's slider allows 0–15 |
| `depth_tolerance` | `0.1` | clearance on the depth |
| `width_tolerance` | `0.1` | clearance on the width |
| `count` | `1` | number of grooves across the joint |
| `gap` | `10` | distance between grooves in mm |

A groove cut always keeps both halves. `groove` and `connectors` are alternatives — passing
both is an error, because the gizmo's two modes are exclusive and `perform_with_groove`
ignores connectors entirely.

**Sizing:** start with a `depth` of about a tenth and a `width` of about a fifth of the
object's smaller horizontal dimension, read from `get_object_info`'s `bounding_box`. A groove
wider than the joint face produces a bad cut, not an error.
```

- [ ] **Step 7: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp docs/tools/reference.md
git commit -m "$(cat <<'MSG'
feat: the dovetail half of the cut gizmo had no MCP spelling at all

CutMode::cutTongueAndGroove routes the cut through Cut::perform_with_groove instead of
perform_with_plane; the MCP tool only ever called the latter, so an agent asking for an
interlocking joint got a flat face and no indication anything had been ignored.

depth and width are required rather than defaulted, and that is the interesting decision.
The gizmo derives them from get_grabber_mean_size, which under ENABLE_FIXED_GRABBER --
defined to 1 -- returns 32 / zoom: the default groove depth genuinely depends on how far
the camera is zoomed in. There is no headless equivalent, so the tool asks instead of
inventing one, and the doc says how to size them from the bounding box.

Everything else defaults to the value the GUI or the Groove struct already uses, and the
four *_init fields are left zero because perform_with_groove never reads them -- they exist
for the gizmo's reset buttons.

Related occurrences checked: groove and connectors are refused together rather than one
silently winning, because the gizmo's modes are exclusive and perform_with_groove ignores
cut_connectors entirely -- accepting both would have quietly discarded the connectors.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 10: `boolean_object` — union, difference, intersection

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp` / `.cpp` (`mcut_boolean_opts`)
- Modify: `tests/slic3rutils/test_mesh_edit.cpp` (its coverage)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` (register `boolean_object`)
- Modify: `docs/tools/reference.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: `object_is_painted`, `report_paint` (Task 2).
- Produces:
  ```cpp
  // OrcaMCPMeshUtils.hpp
  std::string mcut_boolean_opts(const std::string& operation);  // "" when unrecognised
  ```
  and the MCP tool `boolean_object` with parameters
  `{object_id: int (required), tool_object_id: int (required),
    operation: "union"|"difference"|"intersection" (required),
    delete_tool: bool, keep_paint: bool, include_preview: bool}`.

**Which backend.** `MeshBoolean` has two: `MeshBoolean::cgal` (`MeshBoolean.hpp:25-76`) and
`MeshBoolean::mcut` (`MeshBoolean.hpp:78-100`). Both are compiled — `libslic3r_cgal` links
`CGAL::CGAL`, `libigl` and `mcut` (`src/libslic3r/CMakeLists.txt:557`), and `libslic3r` links
`libslic3r_cgal` and `mcut` (`CMakeLists.txt:629-630`). **Use mcut.** It is what the boolean
gizmo calls (`MeshBoolean::mcut::make_boolean`, `GLGizmoMeshBoolean.cpp:360, 380, 400`), it
takes and returns `TriangleMesh` without a CGAL round trip, and it handles multi-volume inputs
(`MeshBoolean.cpp:848-890`). The cgal entry points throw on self-intersection
(`CGALParams::throw_on_self_intersection(true)`, `MeshBoolean.cpp:258`), which would turn a
merely imperfect model into an exception.

The `boolean_opts` strings mcut accepts are exactly `"UNION"`, `"A_NOT_B"`, `"B_NOT_A"` and
`"INTERSECTION"` (`MeshBoolean.cpp:728-731`). `make_boolean` reports failure by leaving
`dst_mesh` empty (`MeshBoolean.cpp:900-903`), never by throwing — the gizmo checks
`temp_mesh_resuls.size() != 0` and pushes a warning notification when it is empty
(`GLGizmoMeshBoolean.cpp:361-369`).

**Do not use `ModelObject::make_boolean`** (`Model.hpp:525`, `Model.cpp:1298-1316`). It calls
`this->mesh()`, which merges the object's raw mesh once **per instance**
(`Model.cpp:1581-1591`), so a two-instance object contributes a doubled mesh; and it applies no
instance transform to either operand, so two objects that are correctly positioned on the plate
are booleaned as if both sat at the origin. The gizmo does it correctly instead: it transforms
each operand's mesh by its world transform before the call
(`GLGizmoMeshBoolean.cpp:355-359`). Follow the gizmo.

**Paint.** The gizmo saves both operands' paint for union and intersection and only the
source's for difference (`GLGizmoMeshBoolean.cpp:363, 383, 403`), then remaps onto the result
with `restore_painting(saved, /*keep_existing_paint=*/true)` (`GLGizmoMeshBoolean.cpp:453`).
Mirror exactly that, under `keep_paint`.

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_mesh_edit.cpp`:

```cpp
TEST_CASE("mcut_boolean_opts maps the tool's words onto mcut's", "[orcamcp][mesh]")
{
    // MeshBoolean.cpp:728-731 is the whole accepted vocabulary.
    CHECK(mcut_boolean_opts("union")        == "UNION");
    CHECK(mcut_boolean_opts("difference")   == "A_NOT_B");
    CHECK(mcut_boolean_opts("intersection") == "INTERSECTION");
}

TEST_CASE("mcut_boolean_opts refuses anything mcut would not understand", "[orcamcp][mesh]")
{
    // An unrecognised opts string does not fail loudly inside mcut -- do_boolean falls
    // through its if/else chain and leaves the mesh untouched -- so it has to be caught here.
    CHECK(mcut_boolean_opts("subtract").empty());
    CHECK(mcut_boolean_opts("UNION").empty());     // the tool's vocabulary is lowercase
    CHECK(mcut_boolean_opts("A_NOT_B").empty());
    CHECK(mcut_boolean_opts("").empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
```
Expected: FAIL — `use of undeclared identifier 'mcut_boolean_opts'`.

- [ ] **Step 3: Declare and implement it**

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp`:

```cpp
// The mcut boolean_opts string for a tool operation word, or "" when the word is not one of
// union / difference / intersection. mcut's accepted strings are UNION, A_NOT_B, B_NOT_A and
// INTERSECTION (MeshBoolean.cpp:728-731); an unrecognised one is not an error inside mcut,
// do_boolean simply falls through and leaves the mesh alone, so it must be caught before the
// call. "difference" is A_NOT_B: the tool object is subtracted from the target.
std::string mcut_boolean_opts(const std::string& operation);
```

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp`:

```cpp
std::string mcut_boolean_opts(const std::string& operation)
{
    if (operation == "union")        return "UNION";
    if (operation == "difference")   return "A_NOT_B";
    if (operation == "intersection") return "INTERSECTION";
    return {};
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[mesh]"
```
Expected: PASS, 12 cases.

- [ ] **Step 5: Register `boolean_object`**

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` — add to the includes:

```cpp
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include <optional>
```

and register the tool inside `register_mesh_tools()`, after `cut_object`:

```cpp
    register_tool({
        "boolean_object",
        "Combine two objects with a mesh boolean. union merges them; difference subtracts the "
        "tool object from the target; intersection keeps only the overlap. The result replaces "
        "the target object's geometry, in place, keeping its position. Painted facets are "
        "discarded unless keep_paint is true.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "The target object (0-based). The result replaces this one."}
                }},
                {"tool_object_id", {
                    {"type", "integer"},
                    {"description", "The other operand (0-based). For difference, this is what "
                                    "gets subtracted."}
                }},
                {"operation", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"union", "difference", "intersection"})},
                    {"description", "union, difference (target minus tool), or intersection"}
                }},
                {"delete_tool", {
                    {"type", "boolean"},
                    {"description", "Remove the tool object afterwards. Default false, matching "
                                    "the GUI's own unchecked \"Delete input\"."}
                }},
                {"keep_paint", {
                    {"type", "boolean"},
                    {"description", "Remap painted facets onto the result. Slow and approximate. "
                                    "Default false, which discards them."}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", nlohmann::json::array({"object_id", "tool_object_id", "operation"})}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const int         object_id       = params.at("object_id");
            const int         tool_object_id  = params.at("tool_object_id");
            const std::string operation       = params.at("operation");
            const bool        delete_tool     = params.value("delete_tool", false);
            const bool        keep_paint      = params.value("keep_paint", false);
            const bool        include_preview = params.value("include_preview", false);

            const std::string opts = mcut_boolean_opts(operation);
            if (opts.empty())
                return nlohmann::json{{"status", "error"},
                    {"message", "operation \"" + operation + "\" is not one of: union, "
                                "difference, intersection"}};

            return run_on_main_thread([object_id, tool_object_id, operation, opts, delete_tool,
                                       keep_paint, include_preview]() -> nlohmann::json {
                McpDialogSuppressionGuard suppression_guard;

                Plater*        plater = wxGetApp().plater();
                Slic3r::Model& model  = plater->model();
                const int      count  = static_cast<int>(model.objects.size());

                if (object_id < 0 || object_id >= count)
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                if (tool_object_id < 0 || tool_object_id >= count)
                    throw std::runtime_error("Invalid tool_object_id: " + std::to_string(tool_object_id));
                if (object_id == tool_object_id)
                    return nlohmann::json{{"status", "error"},
                        {"message", "object_id and tool_object_id must be different objects"}};

                Slic3r::ModelObject* target = model.objects[object_id];
                Slic3r::ModelObject* tool   = model.objects[tool_object_id];
                if (target->volumes.empty() || tool->volumes.empty())
                    return nlohmann::json{{"status", "error"},
                        {"message", "both objects must have geometry"}};

                const bool had_paint = object_is_painted(target) ||
                                       (operation != "difference" && object_is_painted(tool));

                // Boolean in world space. ModelObject::make_boolean does not do this -- it
                // uses raw meshes with no instance transform and merges once per instance
                // (Model.cpp:1298-1316, 1581-1591) -- so follow the gizmo instead
                // (GLGizmoMeshBoolean.cpp:355-359) and transform each operand.
                const Slic3r::Transform3d target_trafo = target->instances[0]->get_transformation().get_matrix();
                const Slic3r::Transform3d tool_trafo   = tool->instances[0]->get_transformation().get_matrix();

                Slic3r::TriangleMesh target_mesh = target->raw_mesh();
                target_mesh.transform(target_trafo);
                Slic3r::TriangleMesh tool_mesh = tool->raw_mesh();
                tool_mesh.transform(tool_trafo);

                std::vector<Slic3r::TriangleMesh> results;
                Slic3r::MeshBoolean::mcut::make_boolean(target_mesh, tool_mesh, results, opts);
                if (results.empty())
                    return nlohmann::json{{"status", "error"},
                        {"message", "The boolean produced no geometry. mcut reports failure this "
                                    "way rather than by raising an error; the usual causes are "
                                    "objects that do not overlap (for difference or "
                                    "intersection) or a non-manifold mesh."}};

                // Save the paint the gizmo would save: both operands for union and
                // intersection, only the target for difference
                // (GLGizmoMeshBoolean.cpp:363, 383, 403).
                std::vector<std::optional<Slic3r::TriangleSelector::SavedPainting>> saved;
                if (keep_paint) {
                    for (Slic3r::ModelVolume* v : target->volumes)
                        saved.emplace_back(v->save_painting());
                    if (operation != "difference")
                        for (Slic3r::ModelVolume* v : tool->volumes)
                            saved.emplace_back(v->save_painting());
                }

                plater->take_snapshot("Mesh Boolean");

                // Replace the target's geometry. The result is in world space, so the object's
                // instance transform is reset to a plain offset -- keeping the object where it
                // was without transforming the already-transformed mesh a second time.
                const std::string  target_name = target->name;
                Slic3r::ModelVolume* first     = target->volumes.front();
                const Slic3r::ModelVolumeType type = first->type();
                const Slic3r::ModelConfigObject volume_config = first->config;

                target->clear_volumes();
                Slic3r::ModelVolume* result_volume =
                    target->add_volume(std::move(results.front()), type);
                result_volume->name = target_name + " - " + operation;
                result_volume->config.apply(volume_config);
                for (const auto& painting : saved)
                    result_volume->restore_painting(painting, /*keep_existing_paint=*/true);
                result_volume->set_new_unique_id();

                target->instances[0]->set_transformation(Slic3r::Geometry::Transformation());
                target->ensure_on_bed();
                target->invalidate_bounding_box();

                nlohmann::json result = {
                    {"status", "success"},
                    {"operation", operation},
                    {"object_id", object_id},
                    {"object_name", target->name},
                    {"tool_object_id", tool_object_id},
                    {"tool_deleted", delete_tool}
                };

                if (delete_tool) {
                    plater->remove(tool_object_id);
                    // remove() shifts every index after the removed one down by one.
                    if (tool_object_id < object_id)
                        result["object_id"] = object_id - 1;
                }

                wxGetApp().obj_list()->reload_all_plates();

                std::vector<std::string> messages = suppression_guard.messages();
                report_paint(result, messages, had_paint, keep_paint, "boolean_object");
                if (!messages.empty())
                    result["info_messages"] = messages;

                result["active_warnings"] = get_active_warnings_json(plater);
                add_turntable_preview_if_requested(result, include_preview);
                return result;
            });
        }
    });
```

`Plater::remove(size_t obj_idx)` and `Plater::model()` are already used by the shipped tools;
`ModelObject::raw_mesh` is `Model.hpp:466`; `ModelObject::ensure_on_bed` is used by the same
gizmo path (`GLGizmoSimplify.cpp:552`).

- [ ] **Step 6: Rebuild and reload, then verify**

Run the rebuild-and-reload recipe. Load the tower twice and overlap them:

```
mcp__orca-slicer__new_project {}
mcp__orca-slicer__load_model {"file_path": "/Users/hanan/Projects/OrcaMCP/resources/calib/pressure_advance/tower_with_seam.drc"}
mcp__orca-slicer__load_model {"file_path": "/Users/hanan/Projects/OrcaMCP/resources/calib/pressure_advance/tower_with_seam.drc"}
mcp__orca-slicer__get_object_info {"object_id": 0}
```
Read `position` from object 0, then move object 1 to overlap it by a few mm:
```
mcp__orca-slicer__move_object {"object_id": 1, "x": 5, "y": 0}
```

Union:
```
mcp__orca-slicer__boolean_object {"object_id": 0, "tool_object_id": 1, "operation": "union", "delete_tool": true}
mcp__orca-slicer__get_scene_info {}
```
Expected: `status: "success"`, `tool_deleted: true`, one object left, and in the app a single
merged solid.

Difference and intersection, each from a fresh two-object scene:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__boolean_object {"object_id": 0, "tool_object_id": 1, "operation": "difference"}
```
Expected: object 0 has a bite taken out of it and object 1 is still present
(`tool_deleted: false`, the default).

```
mcp__orca-slicer__undo {}
mcp__orca-slicer__boolean_object {"object_id": 0, "tool_object_id": 1, "operation": "intersection"}
```
Expected: object 0 is reduced to the overlap region.

The empty-result path — move them apart first so they do not touch:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__move_object {"object_id": 1, "x": 80, "y": 0}
mcp__orca-slicer__boolean_object {"object_id": 0, "tool_object_id": 1, "operation": "intersection"}
```
Expected: `status: "error"` with the "produced no geometry" message, and `get_scene_info`
unchanged — nothing was destroyed.

The rejections:
```
mcp__orca-slicer__boolean_object {"object_id": 0, "tool_object_id": 0, "operation": "union"}
mcp__orca-slicer__boolean_object {"object_id": 0, "tool_object_id": 1, "operation": "subtract"}
```
Expected: `status: "error"` naming the same-object and the unknown-operation problems.

And undo:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__get_scene_info {}
```
Expected: both objects back, unmodified.

- [ ] **Step 7: Update the docs**

`docs/tools/reference.md:14`, the Object Ops row gains `boolean_object`. A new section after
`### split_object`:

````markdown
### boolean_object
Combine two objects with a mesh boolean. The result replaces the target object's geometry in
place; the tool object is left alone unless `delete_tool` is set.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | The target. The result replaces this object |
| `tool_object_id` | integer | Yes | The other operand. For `difference`, what gets subtracted |
| `operation` | string | Yes | `"union"`, `"difference"` (target minus tool), `"intersection"` |
| `delete_tool` | boolean | No | Remove the tool object afterwards. Default `false` |
| `keep_paint` | boolean | No | Remap painted facets onto the result. Default `false` |
| `include_preview` | boolean | No | Return a turntable preview path |

Both meshes are transformed into plate coordinates before the boolean, so two objects that
look overlapped on the plate really are overlapped for the operation.

**Failure is quiet at the library level and loud here.** The mcut backend signals a failed
boolean by returning no mesh rather than by raising an error. `boolean_object` turns that into
`status: "error"` and changes nothing — the usual causes are operands that do not actually
overlap, or a non-manifold input.

**`delete_tool` shifts indices.** Removing the tool object renumbers everything after it, so
when `tool_object_id < object_id` the response's `object_id` reports the target's **new**
index. Read it rather than assuming.

**Example:**
```json
{"name": "boolean_object", "arguments": {"object_id": 0, "tool_object_id": 1,
  "operation": "difference", "delete_tool": true}}
```
````

`CLAUDE.md`: bump the counts to `72 registered, 73 reachable` (heading and sentence) and add
`boolean_object` to the **Transforms** row of the tool table.

- [ ] **Step 8: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp \
        tests/slic3rutils/test_mesh_edit.cpp docs/tools/reference.md CLAUDE.md
git commit -m "$(cat <<'MSG'
feat: union, difference and intersection existed in the tree but not in the API

The MeshBoolean gizmo has done all three since BBS added it; MCP had no tool, so combining
two models was one of the twelve toolbar capabilities an agent simply could not reach.

Backend choice, since MeshBoolean ships two: mcut, not cgal. It is what the gizmo calls, it
takes and returns TriangleMesh without a CGAL round trip, and it handles multi-volume
inputs. The cgal entry points pass throw_on_self_intersection(true), which would turn a
merely imperfect model into an exception rather than a reported failure.

Deliberately not used: ModelObject::make_boolean. It calls this->mesh(), which merges the
raw mesh once per instance, so a two-instance object contributes doubled geometry; and it
applies no instance transform to either operand, so two objects correctly positioned on the
plate would be booleaned as though both sat at the origin. The gizmo transforms each operand
by its world matrix first, and this follows the gizmo.

mcut reports a failed boolean by returning no mesh rather than by raising, so the empty
result is checked and turned into status "error" with the target left untouched -- otherwise
a non-overlapping difference would have silently emptied an object.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 11: `simplify_object` — mesh decimation without the gizmo

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp` / `.cpp` (`simplify_wanted_count`)
- Modify: `tests/slic3rutils/test_mesh_edit.cpp` (its coverage)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` (register `simplify_object`)
- Modify: `docs/tools/reference.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: `object_is_painted`, `report_paint` (Task 2).
- Produces:
  ```cpp
  // OrcaMCPMeshUtils.hpp
  uint32_t simplify_wanted_count(size_t triangle_count, float decimate_ratio);
  ```
  and the MCP tool `simplify_object` with parameters
  `{object_id: int (required), volume_id: int, decimate_ratio: number,
    target_triangles: integer, max_error: number, keep_paint: bool, include_preview: bool}`.

**What the gizmo actually calls.** `GLGizmoSimplify` is a progress bar around one libslic3r
function: `its_quadric_edge_collapse(indexed_triangle_set& its, uint32_t triangle_count,
float* max_error, std::function<void()> throw_on_cancel, std::function<void(int)> statusfn)`
(`QuadricEdgeCollapse.hpp:21-26`), called at `GLGizmoSimplify.cpp:513`. It takes a
**target triangle count** or a **max error**, never a ratio: the gizmo converts its percentage
slider with `Configuration::fix_count_by_ratio` (`GLGizmoSimplify.cpp:705-714`) and then
chooses which of the two to pass based on `use_count` (`GLGizmoSimplify.cpp:500-506`). Drive
the function directly; the gizmo adds nothing else.

**Threading, and why this task is shaped differently.** `its_quadric_edge_collapse` on a large
mesh takes seconds to minutes. Running it inside `run_on_main_thread` would freeze the GUI for
that whole time and risk the bridge's 120 s `ORCAMCP_TIMEOUT`. It does not touch `Model` or
`Plater` — it works on a plain `indexed_triangle_set` — so this handler uses **two**
`run_on_main_thread` calls with the decimation on the HTTP worker thread between them, the same
division of labour the gizmo makes with its worker (`GLGizmoSimplify.cpp:471-527`). The second
call re-validates `object_id` and `volume_id`, because the model can change while the worker
runs.

**Applying the result** mirrors `GLGizmoSimplify::apply_simplify`
(`GLGizmoSimplify.cpp:531-560`) step for step: snapshot,
`plater->clear_before_change_mesh(object_idx)` when paint is **not** kept, `save_painting()`
when it is, `set_mesh`, `restore_painting`, `calculate_convex_hull`,
`invalidate_convex_hull_2d`, `set_new_unique_id`, `invalidate_bounding_box`, `ensure_on_bed`,
`plater->changed_mesh(object_idx)`, `obj_list()->update_item_error_icon(object_idx, -1)`.
`clear_before_change_mesh` and `changed_mesh` are public (`Plater.hpp:578-579`).

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_mesh_edit.cpp`:

```cpp
TEST_CASE("simplify_wanted_count converts a removal percentage into a target count",
          "[orcamcp][mesh]")
{
    // Mirrors GLGizmoSimplify::Configuration::fix_count_by_ratio (GLGizmoSimplify.cpp:705-714).
    // decimate_ratio is the percentage of triangles to REMOVE.
    CHECK(simplify_wanted_count(1000, 50.f) == 500u);
    CHECK(simplify_wanted_count(1000, 90.f) == 100u);
    CHECK(simplify_wanted_count(1000, 25.f) == 750u);
    CHECK(simplify_wanted_count(999,  50.f) == 500u);   // rounds, not truncates
}

TEST_CASE("simplify_wanted_count clamps the ends the way the gizmo does", "[orcamcp][mesh]")
{
    // Ratio 0 removes nothing, so the target is the current count. Ratio 100 hands
    // its_quadric_edge_collapse a count of 0, which is its "no count given" sentinel --
    // that is the gizmo's behaviour and it is preserved deliberately.
    CHECK(simplify_wanted_count(1000, 0.f)   == 1000u);
    CHECK(simplify_wanted_count(1000, -5.f)  == 1000u);
    CHECK(simplify_wanted_count(1000, 100.f) == 0u);
    CHECK(simplify_wanted_count(1000, 150.f) == 0u);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
```
Expected: FAIL — `use of undeclared identifier 'simplify_wanted_count'`.

- [ ] **Step 3: Declare and implement it**

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp` (add `#include <cstdint>`):

```cpp
// The target triangle count for a removal percentage, mirroring
// GLGizmoSimplify::Configuration::fix_count_by_ratio (GLGizmoSimplify.cpp:705-714).
// `decimate_ratio` is the percentage of triangles to REMOVE, so 90 leaves a tenth.
// A ratio of 0 or less leaves the count alone; 100 or more returns 0, which
// its_quadric_edge_collapse reads as "no count given" -- the gizmo's own behaviour.
uint32_t simplify_wanted_count(size_t triangle_count, float decimate_ratio);
```

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp`:

```cpp
uint32_t simplify_wanted_count(size_t triangle_count, float decimate_ratio)
{
    if (decimate_ratio <= 0.f)
        return static_cast<uint32_t>(triangle_count);
    if (decimate_ratio >= 100.f)
        return 0u;
    return static_cast<uint32_t>(
        std::round(double(triangle_count) * (100. - double(decimate_ratio)) / 100.));
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[mesh]"
```
Expected: PASS, 14 cases.

- [ ] **Step 5: Register `simplify_object`**

`src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp` — add to the includes:

```cpp
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include <limits>
```

and register the tool inside `register_mesh_tools()`, after `boolean_object`:

```cpp
    register_tool({
        "simplify_object",
        "Reduce an object's triangle count by quadric edge collapse -- the Simplify gizmo's "
        "algorithm. Give one of decimate_ratio (percent to remove, default 50), "
        "target_triangles (an absolute count), or max_error (mm). Painted facets are discarded "
        "unless keep_paint is true. A large mesh can take a long time; see max_error below.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"volume_id", {
                    {"type", "integer"},
                    {"minimum", -1},
                    {"description", "Part index within the object (0-based). Omit, or pass -1, "
                                    "for a single-part object. Required when the object has "
                                    "more than one part."}
                }},
                {"decimate_ratio", {
                    {"type", "number"},
                    {"description", "Percent of triangles to REMOVE, 0-100. Default 50."}
                }},
                {"target_triangles", {
                    {"type", "integer"},
                    {"description", "Absolute triangle count to aim for. Alternative to decimate_ratio."}
                }},
                {"max_error", {
                    {"type", "number"},
                    {"description", "Collapse edges until the quadric error would exceed this, "
                                    "in mm. Alternative to decimate_ratio and target_triangles."}
                }},
                {"keep_paint", {
                    {"type", "boolean"},
                    {"description", "Remap painted facets onto the simplified mesh. Slow and "
                                    "approximate. Default false, which discards them."}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", nlohmann::json::array({"object_id"})}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const int  object_id       = params.at("object_id");
            const int  volume_id       = params.value("volume_id", -1);
            const bool keep_paint      = params.value("keep_paint", false);
            const bool include_preview = params.value("include_preview", false);

            const bool has_ratio  = params.contains("decimate_ratio");
            const bool has_count  = params.contains("target_triangles");
            const bool has_error  = params.contains("max_error");
            if (static_cast<int>(has_ratio) + static_cast<int>(has_count) + static_cast<int>(has_error) > 1)
                return nlohmann::json{{"status", "error"},
                    {"message", "Give only one of decimate_ratio, target_triangles or max_error."}};

            const double ratio        = params.value("decimate_ratio", 50.0);
            const int    target_count = params.value("target_triangles", 0);
            const double error_limit  = params.value("max_error", 0.0);
            if (has_count && target_count < 4)
                return nlohmann::json{{"status", "error"},
                    {"message", "target_triangles must be at least 4 -- fewer cannot enclose a volume."}};
            if (has_error && error_limit <= 0.0)
                return nlohmann::json{{"status", "error"},
                    {"message", "max_error must be greater than zero."}};

            // Phase 1 (main thread): find the volume and take a copy of its mesh.
            struct Snapshot { int resolved_volume; size_t before; indexed_triangle_set its; std::string error; };
            Snapshot snapshot;
            {
                const nlohmann::json picked = run_on_main_thread([object_id, volume_id, &snapshot]() -> nlohmann::json {
                    Plater*        plater = wxGetApp().plater();
                    Slic3r::Model& model  = plater->model();
                    if (object_id < 0 || object_id >= static_cast<int>(model.objects.size()))
                        throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));

                    Slic3r::ModelObject* object = model.objects[object_id];
                    int resolved = volume_id;
                    if (resolved < 0) {
                        if (object->volumes.size() != 1) {
                            snapshot.error = "Object has " + std::to_string(object->volumes.size()) +
                                             " parts; pass volume_id to say which one to simplify. "
                                             "get_object_info lists them.";
                            return nlohmann::json{{"ok", false}};
                        }
                        resolved = 0;
                    }
                    if (resolved >= static_cast<int>(object->volumes.size())) {
                        snapshot.error = "Invalid volume_id: " + std::to_string(resolved);
                        return nlohmann::json{{"ok", false}};
                    }

                    const Slic3r::ModelVolume* volume = object->volumes[resolved];
                    if (volume->mesh().its.indices.empty()) {
                        snapshot.error = "That part has no geometry to simplify.";
                        return nlohmann::json{{"ok", false}};
                    }

                    snapshot.resolved_volume = resolved;
                    snapshot.before          = volume->mesh().its.indices.size();
                    snapshot.its             = volume->mesh().its;   // copy, for the worker
                    return nlohmann::json{{"ok", true}};
                });
                if (!picked.at("ok").get<bool>())
                    return nlohmann::json{{"status", "error"}, {"message", snapshot.error}};
            }

            // Phase 2 (this HTTP worker thread): the expensive part. its_quadric_edge_collapse
            // touches neither Model nor Plater, so running it here instead of on the GUI thread
            // keeps the app responsive -- the same split GLGizmoSimplify makes with its worker
            // (GLGizmoSimplify.cpp:471-527).
            uint32_t wanted   = has_error ? 0u
                              : has_count ? static_cast<uint32_t>(target_count)
                                          : simplify_wanted_count(snapshot.before, static_cast<float>(ratio));
            float    max_error = has_error ? static_cast<float>(error_limit)
                                           : std::numeric_limits<float>::max();
            Slic3r::its_quadric_edge_collapse(snapshot.its, wanted, &max_error, nullptr, nullptr);
            const size_t after = snapshot.its.indices.size();

            // Phase 3 (main thread): apply it, the way GLGizmoSimplify::apply_simplify does.
            return run_on_main_thread([object_id, resolved_volume = snapshot.resolved_volume,
                                       before = snapshot.before, after, max_error, keep_paint,
                                       its = std::move(snapshot.its), include_preview]() mutable -> nlohmann::json {
                McpDialogSuppressionGuard suppression_guard;

                Plater*        plater = wxGetApp().plater();
                Slic3r::Model& model  = plater->model();

                // The model may have moved on while the worker ran.
                if (object_id >= static_cast<int>(model.objects.size()) ||
                    resolved_volume >= static_cast<int>(model.objects[object_id]->volumes.size()))
                    return nlohmann::json{{"status", "error"},
                        {"message", "The object changed while the mesh was being simplified; "
                                    "nothing was applied. Try again."}};

                Slic3r::ModelObject* object = model.objects[object_id];
                Slic3r::ModelVolume* volume = object->volumes[resolved_volume];
                const bool           had_paint = volume->is_any_painted();

                plater->take_snapshot("Simplify " + volume->name);
                if (!keep_paint)
                    plater->clear_before_change_mesh(object_id);

                std::optional<Slic3r::TriangleSelector::SavedPainting> saved;
                if (keep_paint)
                    saved = volume->save_painting();

                volume->set_mesh(std::move(its));
                volume->restore_painting(saved);
                volume->calculate_convex_hull();
                volume->invalidate_convex_hull_2d();
                volume->set_new_unique_id();
                object->invalidate_bounding_box();
                object->ensure_on_bed();

                plater->changed_mesh(object_id);
                wxGetApp().obj_list()->update_item_error_icon(object_id, -1);

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", object_id},
                    {"volume_id", resolved_volume},
                    {"object_name", object->name},
                    {"triangles_before", static_cast<int>(before)},
                    {"triangles_after", static_cast<int>(after)},
                    {"max_error", max_error}
                };

                std::vector<std::string> messages = suppression_guard.messages();
                report_paint(result, messages, had_paint, keep_paint, "simplify_object");
                if (!messages.empty())
                    result["info_messages"] = messages;

                result["active_warnings"] = get_active_warnings_json(plater);
                add_turntable_preview_if_requested(result, include_preview);
                return result;
            });
        }
    });
```

`its_quadric_edge_collapse` writes the error it actually reached back through `max_error`
(`QuadricEdgeCollapse.hpp:17-18`), which is why the response reports it.

- [ ] **Step 6: Rebuild and reload, then verify**

Run the rebuild-and-reload recipe. Load the tower and read its triangle count from Task 4's
`parts` array:

```
mcp__orca-slicer__get_object_info {"object_id": 0}
mcp__orca-slicer__simplify_object {"object_id": 0}
```
Expected: `status: "success"`, `triangles_after` ≈ half of `triangles_before` (the default
ratio is 50), and the model visibly faceted but still the same shape.

```
mcp__orca-slicer__undo {}
mcp__orca-slicer__simplify_object {"object_id": 0, "decimate_ratio": 90}
```
Expected: `triangles_after` ≈ a tenth of before.

```
mcp__orca-slicer__undo {}
mcp__orca-slicer__simplify_object {"object_id": 0, "target_triangles": 500}
```
Expected: `triangles_after` close to 500.

```
mcp__orca-slicer__undo {}
mcp__orca-slicer__simplify_object {"object_id": 0, "max_error": 0.1}
```
Expected: `status: "success"` and a `max_error` in the response at or below 0.1.

Confirm the GUI stayed responsive during the run — the point of the three-phase split. Then the
rejections:
```
mcp__orca-slicer__simplify_object {"object_id": 0, "decimate_ratio": 50, "target_triangles": 500}
mcp__orca-slicer__simplify_object {"object_id": 0, "target_triangles": 2}
```
Expected: `status: "error"` with the "only one of" and the "at least 4" messages.

Multi-part, using Task 3:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__split_object {"object_id": 0, "mode": "parts"}   # on a multi-body model
mcp__orca-slicer__simplify_object {"object_id": 0}
```
Expected: `status: "error"` naming the part count and pointing at `get_object_info`. Then:
```
mcp__orca-slicer__simplify_object {"object_id": 0, "volume_id": 1, "decimate_ratio": 80}
mcp__orca-slicer__get_object_info {"object_id": 0}
```
Expected: only `parts[1].triangles` dropped.

And undo:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__get_object_info {"object_id": 0}
```
Expected: the original triangle count is back.

- [ ] **Step 7: Update the docs**

`docs/tools/reference.md:14`, the Object Ops row gains `simplify_object`. A new section after
`### boolean_object`:

````markdown
### simplify_object
Reduce a mesh's triangle count by quadric edge collapse — the Simplify gizmo's algorithm,
driven directly.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `volume_id` | integer | No | Part index. Required when the object has more than one part |
| `decimate_ratio` | number | No | Percent of triangles to **remove**, 0–100. Default 50 |
| `target_triangles` | integer | No | Absolute count to aim for. Minimum 4 |
| `max_error` | number | No | Collapse until the quadric error would exceed this, in mm |
| `keep_paint` | boolean | No | Remap painted facets onto the simplified mesh. Default `false` |
| `include_preview` | boolean | No | Return a turntable preview path |

Give at most one of `decimate_ratio`, `target_triangles` and `max_error`; passing two is an
error.

**Returns:** `object_id`, `volume_id`, `object_name`, `triangles_before`, `triangles_after`,
`max_error` (the error actually reached), `paint`, `active_warnings`.

**Long runs.** Decimation happens off the GUI thread, so the app stays responsive, but the MCP
call itself blocks until it finishes. A multi-million-triangle mesh can exceed the bridge's
`ORCAMCP_TIMEOUT` (120 s); raise it for those, or decimate in two passes.

**Example:**
```json
{"name": "simplify_object", "arguments": {"object_id": 0, "decimate_ratio": 90}}
```
````

`CLAUDE.md`: bump the counts to `73 registered, 74 reachable` (heading and sentence) and add
`simplify_object` to the **Transforms** row of the tool table.

- [ ] **Step 8: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPMeshUtils.cpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp \
        tests/slic3rutils/test_mesh_edit.cpp docs/tools/reference.md CLAUDE.md
git commit -m "$(cat <<'MSG'
feat: a downloaded model could be loaded over MCP but never decimated

GLGizmoSimplify turns out to be a progress bar around one libslic3r call --
its_quadric_edge_collapse -- plus the arithmetic that converts its percentage slider into a
target triangle count. The tool drives that function directly and reimplements the
conversion with test coverage; nothing about the gizmo was needed.

The handler is split across three phases rather than one because of what the algorithm
costs. run_on_main_thread blocks the GUI thread for the whole call, and decimating a large
mesh takes seconds to minutes, which would freeze the app and risk the bridge's 120s
timeout. its_quadric_edge_collapse touches neither Model nor Plater, so phase 1 copies the
mesh on the GUI thread, phase 2 decimates on the HTTP worker, and phase 3 applies the result
on the GUI thread -- re-validating the indices first, since the model can change in between.

Related occurrences checked: the mesh-replacement sequence in phase 3 follows
GLGizmoSimplify::apply_simplify exactly, including clear_before_change_mesh when paint is
not kept and changed_mesh afterwards -- skipping either leaves stale SLA support points and
a stale error icon on the object.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Task 12: Final sweep — the in-app catalogue, the counts, and one end-to-end run

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` (`get_server_info`, five places)
- Modify: `CLAUDE.md` (verify the counts and the tool table)
- Modify: `docs/tools/reference.md` (verify the quick-reference table)

**Interfaces:**
- Consumes: all four tools.
- Produces: nothing new. This task makes the server describe itself correctly and proves the
  batch works together.

**Why it is its own task.** `get_server_info` (`OrcaMCPServer.cpp:357-812`) is a hand-written
catalogue an agent reads before it does anything else. It lists tools in five separate places,
and none of them is generated, so every one has to be visited deliberately. A tool that is
registered but absent from this catalogue is effectively undiscoverable.

- [ ] **Step 1: Write the failing check**

Rebuild and reload if needed, then:

```
mcp__orca-slicer__get_server_info {}
```
Expected: `split_object`, `boolean_object` and `simplify_object` appear nowhere in the
response, and `cut_object` is still described as "Cut object horizontally at Z height".

- [ ] **Step 2: Update the five places**

`src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:438`:

```cpp
                            {"tools", "move_object, rotate_object, scale_object, mirror_object, cut_object, split_object, boolean_object, simplify_object"},
```

`OrcaMCPServer.cpp:524-529` — replace the `cut_object` example block and add the three new
tools after it:

```cpp
                    {"cut_object", {
                        {"horizontal", R"({"object_id": 0, "z_height": 25, "keep": "both"})"},
                        {"arbitrary_plane", R"({"object_id": 0, "plane_origin": {"x": 120, "y": 120, "z": 25}, "plane_normal": {"x": 1, "y": 0, "z": 1}, "keep": "both"})"},
                        {"as_parts", R"({"object_id": 0, "z_height": 25, "keep": "parts"})"},
                        {"with_connectors", R"({"object_id": 0, "z_height": 25, "keep": "both", "connectors": [{"x": 118, "y": 120, "z": 25}]})"},
                        {"dovetail", R"({"object_id": 0, "z_height": 25, "groove": {"depth": 2, "width": 8}})"},
                        {"when_to_use", "Splitting a tall model to fit the bed, or making a joined two-piece print"},
                        {"tip", "keep parts gives one object with two parts, addressable by volume_id"}
                    }},
                    {"split_object", {
                        {"to_objects", R"({"object_id": 0, "mode": "objects"})"},
                        {"to_parts", R"({"object_id": 0, "mode": "parts"})"},
                        {"when_to_use", "A file with several disconnected bodies in one object"},
                        {"tip", "mode parts is what makes set_object_filament's volume_id usable"}
                    }},
                    {"boolean_object", {
                        {"example", R"({"object_id": 0, "tool_object_id": 1, "operation": "difference", "delete_tool": true})"},
                        {"when_to_use", "Merging two models, cutting a hole, or keeping only an overlap"},
                        {"tip", "difference subtracts tool_object_id from object_id; delete_tool shifts later indices"}
                    }},
                    {"simplify_object", {
                        {"example", R"({"object_id": 0, "decimate_ratio": 90})"},
                        {"when_to_use", "A downloaded model whose triangle count makes slicing slow"},
                        {"tip", "decimate_ratio is the percent REMOVED; use target_triangles for an absolute count"}
                    }},
```

`OrcaMCPServer.cpp:685` — replace the `cut_object` line and add three:

```cpp
                        {"cut_object", "Cut an object with a plane (horizontal or arbitrary), optionally as parts, with connectors, or as a dovetail joint"},
                        {"split_object", "Split a multi-body object into separate objects, or into parts of one object"},
                        {"boolean_object", "Union, difference or intersection of two objects"},
                        {"simplify_object", "Reduce an object's triangle count"},
```

`OrcaMCPServer.cpp:787` — the caution now needs to be true of all four:

```cpp
                        {"mesh_editing_caution", "cut_object, split_object, boolean_object and simplify_object all replace geometry. Each takes an undo snapshot, so undo restores the original. Painted facets are DISCARDED unless keep_paint is true -- the response's 'paint' block always says which happened."},
```
(delete the old `cut_object_caution` entry, which said "Cut removes original and creates new
object(s). Use undo if result is wrong." — undo now genuinely works, and the caution that
matters is the paint.)

`OrcaMCPServer.cpp:803` — the `supported_tools` list for `include_preview`:

```cpp
                            "flatten_object", "clone_object", "delete_object", "cut_object",
                            "split_object", "boolean_object", "simplify_object",
```

- [ ] **Step 3: Verify the counts agree everywhere**

```bash
cd /Users/hanan/Projects/OrcaMCP
grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'
grep -n "registered" CLAUDE.md
```
Expected: the command prints `73`, and `CLAUDE.md` says `73 registered, 74 reachable` in the
heading and "The server registers 73;" in the sentence beneath it. Fix whichever disagrees.

Check the CLAUDE.md tool table has all four in their rows:
```bash
grep -n "split_object\|boolean_object\|simplify_object\|cut_object" CLAUDE.md docs/tools/reference.md | head -20
```
Expected: `split_object` in **Models**, `boolean_object` and `simplify_object` in
**Transforms**, `cut_object` already in **Transforms**, and all four in `reference.md`'s
quick-reference table at line 14.

- [ ] **Step 4: Full build and the whole test suite**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo \
    --target OrcaSlicer slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: 0 failures, 188 cases (174 baseline + 14 from `test_mesh_edit.cpp`). A varying
skipped count is expected, not a regression.

- [ ] **Step 5: One end-to-end run that exercises the batch together**

Reload the app and run, in order:

```
mcp__orca-slicer__new_project {}
mcp__orca-slicer__load_model {"file_path": "/Users/hanan/Projects/OrcaMCP/resources/calib/pressure_advance/tower_with_seam.drc"}
mcp__orca-slicer__simplify_object {"object_id": 0, "decimate_ratio": 80}
mcp__orca-slicer__cut_object {"object_id": 0, "z_height": 10, "keep": "parts"}
mcp__orca-slicer__get_object_info {"object_id": 0}
mcp__orca-slicer__set_object_filament {"object_id": 0, "volume_id": 0, "filament": 1}
mcp__orca-slicer__set_object_filament {"object_id": 0, "volume_id": 1, "filament": 2}
mcp__orca-slicer__render_plate_view {"save_to_file": true}
mcp__orca-slicer__slice_all {}
mcp__orca-slicer__get_slicing_status {}
```
Expected: a two-tone, decimated, cut object that slices without error. Read the rendered image
and confirm the two halves are different colours.

Then walk it all back:
```
mcp__orca-slicer__undo {}
mcp__orca-slicer__undo {}
mcp__orca-slicer__undo {}
mcp__orca-slicer__undo {}
mcp__orca-slicer__get_object_info {"object_id": 0}
```
Expected: one part, the original triangle count, no filament overrides.

And confirm the catalogue:
```
mcp__orca-slicer__get_server_info {}
```
Expected: all four tools present in the object-operations list, the workflow tool list, the
examples and the preview-supported list, and the new `mesh_editing_caution`.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp CLAUDE.md docs/tools/reference.md
git commit -m "$(cat <<'MSG'
docs: three new tools were registered but the server did not admit they existed

get_server_info is the catalogue an agent reads before it does anything, and it is
hand-written in five separate places -- the transform workflow's tool list, the per-tool
examples, the object-operations descriptions, the common pitfalls and the include_preview
support list. split_object, boolean_object and simplify_object were registered and callable
but absent from all five, so an agent following the documentation would never have found
them.

Also replaced cut_object_caution, which told callers to use undo if a cut went wrong -- advice
that did not work until this batch added the missing snapshot. The caution that actually
matters now is that all four mesh tools discard painted facets unless keep_paint is set, so
mesh_editing_caution says that instead, for all four.

Related occurrences checked: the tool count in CLAUDE.md was reconciled against the count
command in the same file (73 registered, 74 reachable with the bridge's start_orca), and the
quick-reference table in docs/tools/reference.md against the per-tool sections.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
)"
```

---

## Out of scope, and why

Named here so an executor does not go looking for tasks that were left out by accident.

- **Cut by contour** (`Cut::perform_by_contour`, `CutUtils.hpp:63`). It takes a
  `std::vector<Cut::Part>` produced by `GLGizmoCut3D`'s `PartSelection`, which is built from
  the object clipper's contours — a canvas-side structure with no headless equivalent. The
  arbitrary plane, `keep: "parts"`, connectors and the groove cover every joint the gizmo can
  make without a mouse.
- **Connector contour validation** (`GLGizmoCut3D::is_outside_of_cut_contour`). Same reason:
  it raycasts the clipper. Documented as a caveat in Task 8 rather than faked.
- **`merge` / `merge_volumes`** (`Model.hpp:522`, `Model.hpp:527`). Real APIs, and the inverse
  of `split_object`, but the spec's Plan 3 does not list them and the toolbar audit (T6) does
  not record them as a gap. Worth raising with the user separately.
- **`MeshBoolean::cgal::segment`** (`MeshBoolean.hpp:70`), BBS's mesh segmentation. Not a
  toolbar item and not in the spec.
- **Progress reporting for `simplify_object`.** `its_quadric_edge_collapse` takes a `statusfn`
  callback, but MCP has no channel for progress on an in-flight call — `get_slicing_status`
  exists only because slicing is asynchronous. Passing `nullptr` is deliberate.

---

## Self-Review

**1. Spec coverage.** Plan 3 of the spec lists four items:

| Spec item | Task |
|---|---|
| `split_object` — `splitobjects` and `splitvolumes`, `remap_paint` decided, documented and reported | Tasks 2, 3 (plus Task 4 for the read-back that makes parts usable) |
| `boolean_object` — union/difference/intersection via `MeshBoolean` | Task 10 |
| `simplify_object` — locate what `GLGizmoSimplify` calls and drive that directly | Task 11 |
| `cut_object` upgrade — arbitrary plane, keep-both, dovetail/connector joints, current call shape preserved | Tasks 5, 6, 7, 8, 9 |

Global Constraints coverage: branch and commit style are in every commit block; threading is in
every handler; `McpDialogSuppressionGuard` scopes every handler; pure logic has Catch2 coverage
in Tasks 1, 4, 8, 10, 11; snapshots are in Tasks 2, 3, 5, 8, 10, 11 (and Task 5 exists because
one was missing); registration follows the per-area registrar pattern; docs and the count are
updated in Tasks 2, 10, 11 and reconciled in Task 12; the response shape and `active_warnings`
are in every handler; the verify command runs in Tasks 1, 4, 8, 10, 11 and in full in Task 12.

**2. Placeholder scan.** No "TBD", no "add error handling", no "similar to Task N", no test
described without its code. Every code step carries the code. The two places where a task says
"replace X with Y" quote both sides.

**3. Type consistency.**
- `cut_plane_matrix(const Vec3d&, const Vec3d&, const Vec3d&, Transform3d&) -> bool` — declared
  Task 1, called Task 6 and (via the same `cut_matrix`) Tasks 8 and 9.
- `PaintResult` / `classify_paint_result` / `paint_result_message` — declared Task 1, consumed
  only through `report_paint`, defined once in Task 2 and called in Tasks 2, 3, 5, 6, 10, 11
  with the tool name as `operation`.
- `object_is_painted(const ModelObject*)` — defined Task 2, called Tasks 2, 5, 10.
- `volume_type_name(ModelVolumeType) -> const char*` — Task 4 only.
- `parse_connector_attributes(...) -> bool` and `connector_mesh(...) -> indexed_triangle_set` —
  Task 8 only.
- `mcut_boolean_opts(const std::string&) -> std::string` — Task 10 only.
- `simplify_wanted_count(size_t, float) -> uint32_t` — Task 11 only.
- `register_mesh_tools()` — declared Task 2, defined Task 2, extended in Tasks 5, 10, 11.
- The `paint` response block has one shape everywhere: `{had_paint, keep_paint, result}`, with
  `result` one of `"none"`, `"remapped"`, `"discarded"`.
- `keep_paint` is the parameter name in all four tools; `remap_paint` is libslic3r's name for
  the same flag and appears only where a libslic3r signature is quoted.
- Task 7 introduces `as_parts`; Task 8 redefines it as `(keep == "parts") && !has_connectors`
  and replaces the `keep == ...` tests with `want_upper` / `want_lower`; Task 9 extends those
  two with `has_groove`. Each step quotes the line it replaces.

**4. Fixes made during review.**
- Task 8's snapshot ordering: connectors mutate the object before `Cut` runs, so the snapshot
  has to move earlier for that path. Added the `has_connectors` guard in both places rather
  than leaving a double snapshot.
- Task 10's `delete_tool` renumbering: `Plater::remove` shifts later indices, so the response
  reports the target's post-removal `object_id`. Called out in the doc too.
- Task 11's phase 3 re-validates `object_id` and `volume_id` because the worker phase runs
  with no lock on the model.
