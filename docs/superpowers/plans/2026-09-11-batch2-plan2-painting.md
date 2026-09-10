# Painting Implementation Plan (Batch 2, Plan 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give an agent a `paint_object` tool that writes the same per-triangle facet
annotations the GUI's paint gizmos write — colour (MMU segmentation), support, seam and
fuzzy skin — plus read-back, clearing, and a separate `set_brim_ears` tool, so the palette
an agent can already propose can finally be applied.

**Architecture:** Three new files split by what they depend on. `OrcaMCPPaintGeometry.*` is
pure maths over points and triangles — no `Model`, no wxWidgets — and carries the band and
region logic under Catch2. `OrcaMCPPaintModel.*` depends on `libslic3r`'s `Model` and
`TriangleSelector` but still not on wxWidgets, so the write path (`TriangleSelector::set_facet`
→ `FacetsAnnotation::set`) is unit-tested headlessly against a real `ModelVolume`.
`OrcaMCPPaintTools.cpp` is the only wx-aware file: parameter parsing, undo snapshots, UI
refresh and JSON. The write path deliberately mirrors `GLGizmoMmuSegmentation::update_model_object`
(`src/slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.cpp:711-723`) so MCP-painted data is
byte-identical to gizmo-painted data and round-trips through 3MF unchanged.

**Tech Stack:** C++17, `libslic3r` (`Model`, `TriangleSelector`, `FacetsAnnotation`,
`BrimPoint`), `nlohmann::json`, Catch2 v3, CMake, wxWidgets (tools layer only).

**Spec:** docs/superpowers/plans/2026-09-10-batch-2-spec.md

## Global Constraints

Copied verbatim from the spec's Global Constraints section. Every task inherits these.

- **Branch:** `sync-upstream-2.5`. Do not push. Do not create branches.
- **Commit style:** `fix:` / `feat:` + what changed, past tense. The body names the
  **root cause**, not the symptom, and lists related occurrences checked — *including the
  ones deliberately left alone, and why*. Read `git log -5` for the bar. Every commit ends with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
- **Threading:** every tool handler body runs inside `run_on_main_thread()`
  (`OrcaMCPCommon.hpp`). Anything touching `Model`, `Plater` or the preset bundle must be
  inside it. A modal opened inside it hangs the GUI forever.
- **Dialogs:** never call `set_mcp_dialog_suppression()` directly. `McpDialogSuppressionGuard`
  (RAII, nest-safe) is the only sanctioned way.
- **Pure logic goes in a free function with Catch2 coverage** under `tests/slic3rutils/`.
  This project tests logic without a printer wherever possible; geometry maths must be
  testable without a GUI.
- **Undo/redo:** any tool that mutates the model must take a snapshot so `undo` works, the
  way the existing transform tools do. Check how `move_object` does it and follow it.
- **Registration:** tools are registered with `register_tool({...})` in
  `register_builtin_tools()` (`OrcaMCPServer.cpp`) or the per-area registrars
  (`OrcaMCPFilamentTools.cpp`, `OrcaMCPPrinterTools.cpp`). New tool groups get their own
  file following that pattern, added to `src/slic3r/CMakeLists.txt`.
- **Docs:** every new or changed tool updates `docs/tools/reference.md`, and the tool table
  plus the count in `CLAUDE.md`. The count command is in CLAUDE.md.
- **Response shape:** `{"status": "success"|"error"|"partial", ...}` plus `active_warnings`
  on anything that mutates the scene, matching existing tools.
- **Verify before reporting:**
  ```
  cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
  build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
  ```
  Baseline at the time of writing: **174 cases / 1195 assertions / 0 failures**. The skipped
  count varies run to run (bundled-Python and numpy cases) — that is expected, not a regression.
- **Do not start the app or contact the printer** unless the plan says to. (Task 13 says to.)

---

## Three things the spec's own wording will send you looking for

**1. The spec says explicit band ranges are "in object coordinates". This plan uses plate
coordinates instead.** The same spec paragraph then requires the coordinates to "be consistent
with what `get_object_info` reports", and `get_object_info` reports plate coordinates only. The
two halves of that requirement cannot both hold, so the consistency half wins — see the next
section for the full argument. Every tool description and `reference.md` says "plate", loudly,
which is the part the spec cared about.

**2. The undo constraint says to follow `move_object`. `move_object` takes no snapshot.**
Check it: `move_object` spans `OrcaMCPServer.cpp:3077-3215` and never calls `take_snapshot` anywhere in
that range.
The tools that do it correctly are `set_object_printable` (`OrcaMCPServer.cpp:4075-4080`) and
`set_object_filament` (`OrcaMCPFilamentUtils.cpp:150-151`), both of which validate everything
first and only then call `plater->take_snapshot(...)`. That is the pattern every mutating tool
in this plan follows. Do not "fix" `move_object` here — it is a real gap, but it belongs to
whoever sweeps the transform tools, and folding it in would bury it in a painting change.

**3. No `McpDialogSuppressionGuard` appears in this plan, and that is correct.** None of the
four tools opens a modal: they write model data and call `plater->update()`. The guard is
mandatory when a handler *can* reach a dialog, not decoration on every handler — the existing
transform tools do not use it either. If a future step here ever reaches a file dialog or a
`MsgDialog`, it needs the guard, because a modal inside `run_on_main_thread` hangs the GUI
forever.

---

## The Coordinate Decision — read this before Task 1

**Every coordinate this feature accepts or reports is in the plate frame**, the same frame
`get_object_info` reports its `bounding_box` and `position` in
(`OrcaMCPServer.cpp:3902-3918`, which derives both from `ModelObject::bounding_box_approx()`;
`Model.cpp:1512-1522` shows that merges `ModelInstance::transform_bounding_box` over the
instances, i.e. world/plate millimetres — the same frame `PartPlate::get_plate_box()` is in,
which is why the `on_bed` check at `OrcaMCPServer.cpp:3940-3945` can compare them directly).

Facet data itself lives in *volume-local mesh* coordinates. The conversion is
`instance.get_matrix() * volume.get_matrix()` (`Model.hpp:1354` and `Model.hpp:1013`),
which is exactly what `ModelObject::instance_bounding_box` composes
(`Model.cpp:1685-1697`).

**Why plate, not object-local:** an agent has no way to learn an object's local extents.
`get_object_info` reports plate coordinates only, so object-local band boundaries would
force the caller to undo the instance rotation, scale and offset itself before it could
say "band 3 runs from 40 mm to 48 mm". That is the exact class of arithmetic an agent gets
wrong silently. Plate coordinates let the agent read `bounding_box.min.y` / `max.y` from
`get_object_info` and hand those numbers straight back.

**Multiple instances:** paint is volume data, so it applies to every instance of the object.
The plate frame is therefore ambiguous when an object has more than one instance. The tools
resolve it with an `instance_id` parameter (default `0`): that instance's transform defines
the frame. Every response echoes `"coordinate_frame": "plate"` and `"instance_id"`, and the
tool descriptions plus `docs/tools/reference.md` say so.

**Brim ears are the one place a conversion is stored.** `ModelObject::brim_points`
(`Model.hpp:390`, type `BrimPoints` from `src/libslic3r/BrimEarsPoint.hpp:63`) holds
**object-local** positions — `Brim.cpp:373-374` transforms each by the instance matrix to get
a world position and skips any whose world `z > 0`. `set_brim_ears` therefore takes plate
X/Y like everything else and stores `instance.get_matrix().inverse() * Vec3d(x, y, -0.0001)`,
which is character-for-character what `GLGizmoBrimEars.cpp:395-402` does.

---

## File Structure

**Create:**

| File | Responsibility |
|---|---|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp` | Pure geometry contract: axis, bands, box/sphere regions, facet centroids, surface area, per-facet assignment. No `Model`, no wx. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp` | Its implementation. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp` | Mode → annotation member, state naming, plate transform, the `TriangleSelector` write path, read-back, clearing. Depends on `libslic3r` only. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp` | Its implementation. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp` | `register_paint_tools()`: `paint_object`, `get_object_paint`, `clear_object_paint`, `set_brim_ears`. The only wx-aware file. |
| `tests/slic3rutils/test_paint_geometry.cpp` | Catch2 coverage for both new non-wx units. |

**Modify:**

| File | Change |
|---|---|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp:67` | Declare `static void register_paint_tools();` beside the other registrars. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:4332-4333` | Call `register_paint_tools();`. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:647-790` | Add the four tools to `get_server_info`'s `tools_by_category` catalog. |
| `src/slic3r/CMakeLists.txt:406-426` | Add the five new source/header files to the `libslic3r_gui` source list. |
| `tests/slic3rutils/CMakeLists.txt` | Add `test_paint_geometry.cpp`. |
| `docs/tools/reference.md` | New "Painting Tools" section, Quick Reference Table row. |
| `CLAUDE.md` | Tool table row and the 70 → 74 count. |

**Tool count after this plan:** 70 registered → **74 registered, 75 reachable**
(`paint_object`, `get_object_paint`, `clear_object_paint`, `set_brim_ears`).

---

### Task 1: Pure geometry unit — axis parsing and even band splitting

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp`
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp`
- Create: `tests/slic3rutils/test_paint_geometry.cpp`
- Modify: `src/slic3r/CMakeLists.txt` (add the two files after line 411, beside `OrcaMCPSliceEstimate.*`)
- Modify: `tests/slic3rutils/CMakeLists.txt` (add `test_paint_geometry.cpp` after `test_material_mapping.cpp`)

**Interfaces:**
- Consumes: nothing.
- Produces: `Slic3r::GUI::OrcaMCP::PaintAxis` (`enum class : X=0, Y=1, Z=2`),
  `bool parse_paint_axis(const std::string&, PaintAxis&)`,
  `const char* paint_axis_name(PaintAxis)`,
  `struct PaintBand { int state; double from; double to; }`,
  `std::vector<PaintBand> make_even_bands(const std::vector<int>& states, double axis_min, double axis_max)`.

- [ ] **Step 1: Write the failing test**

Create `tests/slic3rutils/test_paint_geometry.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp"

#include <string>
#include <vector>

// paint_object's geometry. Bands and regions are resolved here, away from the Model and the GUI,
// because this is the arithmetic that decides which triangle gets which filament -- the part that
// has to be right before anything is written to a FacetsAnnotation.

using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

TEST_CASE("parse_paint_axis takes the three axis names in any case", "[orcamcp][paint]")
{
    PaintAxis axis = PaintAxis::Z;
    REQUIRE(parse_paint_axis("x", axis));
    CHECK(axis == PaintAxis::X);
    REQUIRE(parse_paint_axis("Y", axis));
    CHECK(axis == PaintAxis::Y);
    REQUIRE(parse_paint_axis("z", axis));
    CHECK(axis == PaintAxis::Z);

    CHECK(std::string(paint_axis_name(PaintAxis::Y)) == "y");

    PaintAxis untouched = PaintAxis::Y;
    CHECK_FALSE(parse_paint_axis("length", untouched));
    CHECK(untouched == PaintAxis::Y);
    CHECK_FALSE(parse_paint_axis("", untouched));
}

TEST_CASE("make_even_bands splits a range into equal bands with no gap at either end",
          "[orcamcp][paint]")
{
    const std::vector<PaintBand> bands = make_even_bands({5, 6, 7}, 0.0, 12.0);

    REQUIRE(bands.size() == 3);
    CHECK(bands[0].state == 5);
    CHECK_THAT(bands[0].from, WithinAbs(0.0, 1e-12));
    CHECK_THAT(bands[0].to, WithinAbs(4.0, 1e-12));
    CHECK(bands[1].state == 6);
    CHECK_THAT(bands[1].from, WithinAbs(4.0, 1e-12));
    CHECK(bands[2].state == 7);
    CHECK_THAT(bands[2].from, WithinAbs(8.0, 1e-12));
    // The last boundary is the range end exactly, not the range end plus accumulated rounding.
    CHECK(bands[2].to == 12.0);
    // Adjacent bands share a boundary exactly, so no facet can fall between two of them.
    CHECK(bands[0].to == bands[1].from);
    CHECK(bands[1].to == bands[2].from);
}

TEST_CASE("make_even_bands refuses a degenerate request instead of guessing", "[orcamcp][paint]")
{
    CHECK(make_even_bands({}, 0.0, 12.0).empty());
    CHECK(make_even_bands({5}, 12.0, 12.0).empty());
    CHECK(make_even_bands({5, 6}, 12.0, 0.0).empty());

    const std::vector<PaintBand> single = make_even_bands({9}, -3.5, 2.5);
    REQUIRE(single.size() == 1);
    CHECK(single[0].state == 9);
    CHECK(single[0].from == -3.5);
    CHECK(single[0].to == 2.5);
}

TEST_CASE("make_even_bands handles the scraper's 14 bands along Y", "[orcamcp][paint]")
{
    // The print-bed scraper is 122 mm along Y and lands centred on a 300 mm plate, so its plate
    // Y extent is 89..211. Fourteen bands over slots 5..18 is the session's original request.
    std::vector<int> slots;
    for (int slot = 5; slot <= 18; ++slot)
        slots.push_back(slot);

    const std::vector<PaintBand> bands = make_even_bands(slots, 89.0, 211.0);

    REQUIRE(bands.size() == 14);
    CHECK(bands.front().state == 5);
    CHECK(bands.back().state == 18);
    CHECK(bands.front().from == 89.0);
    CHECK(bands.back().to == 211.0);
    for (size_t i = 1; i < bands.size(); ++i)
        CHECK(bands[i].from == bands[i - 1].to);
    CHECK_THAT(bands[0].to - bands[0].from, WithinAbs(122.0 / 14.0, 1e-9));
}
```

Register it in `tests/slic3rutils/CMakeLists.txt` by adding one line after `test_material_mapping.cpp`:

```cmake
    test_paint_geometry.cpp
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `fatal error: 'slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp' file not found`

- [ ] **Step 3: Write the header**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Which plate axis a band selection runs along. The value is the row of a Vec3d it indexes.
// Every coordinate in this header is in PLATE millimetres -- the frame get_object_info reports
// its bounding_box in -- never volume-local mesh coordinates.
enum class PaintAxis { X = 0, Y = 1, Z = 2 };

// "x" / "y" / "z", any case. Returns false and leaves `out` untouched for anything else.
bool parse_paint_axis(const std::string& name, PaintAxis& out);

// The lowercase name parse_paint_axis accepts, for echoing an axis back in a response.
const char* paint_axis_name(PaintAxis axis);

// One band along an axis, in plate millimetres. `state` is the raw EnforcerBlockerType value
// written to every facet whose centroid falls in the band: for mmu_segmentation that is the
// 1-based filament slot (TriangleSelector.hpp:23-32 numbers Extruder1..Extruder32 as 1..32),
// for the other three annotations it is 0 (none), 1 (enforcer) or 2 (blocker).
struct PaintBand
{
    int    state = 0;
    double from  = 0.0;
    double to    = 0.0;
};

// `states.size()` equal-width bands covering [axis_min, axis_max], in the order given.
// Boundaries are computed as axis_min + span * i / n rather than by repeated addition, so
// adjacent bands share a boundary exactly and the last band's `to` is exactly axis_max. Repeated
// addition would leave sub-micron gaps that facets fall through, and the failure would look like
// a handful of randomly unpainted triangles.
// Returns an empty vector when `states` is empty or axis_max <= axis_min. It does not decide what
// the caller should be told about that -- the tool layer turns an empty result into an error.
std::vector<PaintBand> make_even_bands(const std::vector<int>& states, double axis_min, double axis_max);

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 4: Write the implementation**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp
#include "OrcaMCPPaintGeometry.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

bool parse_paint_axis(const std::string& name, PaintAxis& out)
{
    if (name.size() != 1)
        return false;
    switch (std::tolower(static_cast<unsigned char>(name[0]))) {
    case 'x': out = PaintAxis::X; return true;
    case 'y': out = PaintAxis::Y; return true;
    case 'z': out = PaintAxis::Z; return true;
    default:  return false;
    }
}

const char* paint_axis_name(PaintAxis axis)
{
    switch (axis) {
    case PaintAxis::X: return "x";
    case PaintAxis::Y: return "y";
    case PaintAxis::Z: return "z";
    }
    return "z";
}

std::vector<PaintBand> make_even_bands(const std::vector<int>& states, double axis_min, double axis_max)
{
    std::vector<PaintBand> bands;
    if (states.empty() || !(axis_max > axis_min))
        return bands;

    const double span = axis_max - axis_min;
    const double n    = double(states.size());
    bands.reserve(states.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        PaintBand band;
        band.state = states[i];
        // Interpolate each boundary from the ends rather than accumulating a width, so
        // band[i].to and band[i+1].from are bit-identical and the last `to` is exactly axis_max.
        band.from = i == 0 ? axis_min : axis_min + span * (double(i) / n);
        band.to   = i + 1 == states.size() ? axis_max : axis_min + span * (double(i + 1) / n);
        bands.push_back(band);
    }
    return bands;
}

}}} // namespace Slic3r::GUI::OrcaMCP
```

Add both files to `src/slic3r/CMakeLists.txt` immediately after line 411
(`GUI/OrcaMCP/OrcaMCPSliceEstimate.cpp`):

```cmake
    GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp
    GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[paint]"
```
Expected: PASS — 4 test cases, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp \
        src/slic3r/CMakeLists.txt \
        tests/slic3rutils/test_paint_geometry.cpp \
        tests/slic3rutils/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: added the band arithmetic paint_object will resolve filaments with

Painting a model in even sections along an axis is the request that had no tool at
all (T5). The arithmetic that decides which triangle gets which filament is the part
that has to be right before anything touches a FacetsAnnotation, so it lands first as
a free function with Catch2 coverage, per the batch's "geometry maths must be testable
without a GUI" constraint.

Boundaries are interpolated from the ends instead of accumulated, so adjacent bands
share a boundary bit-exactly. Accumulating a width leaves sub-micron gaps that facet
centroids fall through, and that failure presents as a scatter of unpainted triangles
rather than as an arithmetic bug.

Coordinates are plate millimetres throughout, matching what get_object_info already
reports (OrcaMCPServer.cpp:3902-3918). Object-local was the alternative and was
rejected: no MCP tool reports object-local extents, so a caller would have to undo the
instance transform itself to name a band boundary.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Band lookup — which band a facet centroid belongs to

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp` (append after `make_even_bands`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp` (append after `make_even_bands`)
- Test: `tests/slic3rutils/test_paint_geometry.cpp` (append)

**Interfaces:**
- Consumes: `PaintBand`, `make_even_bands` from Task 1.
- Produces: `int band_index_for_value(const std::vector<PaintBand>& bands, double value)` —
  index into `bands`, or `-1` when the value belongs to no band.

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_paint_geometry.cpp`:

```cpp
TEST_CASE("band_index_for_value puts a shared boundary in the upper band, once", "[orcamcp][paint]")
{
    const std::vector<PaintBand> bands = make_even_bands({5, 6, 7}, 0.0, 12.0);

    CHECK(band_index_for_value(bands, 0.0) == 0);
    CHECK(band_index_for_value(bands, 3.999) == 0);
    // Exactly on the boundary: the upper band, never both, never neither.
    CHECK(band_index_for_value(bands, 4.0) == 1);
    CHECK(band_index_for_value(bands, 8.0) == 2);
    // The far end of the range is inside the last band, or the whole far face goes unpainted.
    CHECK(band_index_for_value(bands, 12.0) == 2);
}

TEST_CASE("band_index_for_value reports a value outside every band", "[orcamcp][paint]")
{
    const std::vector<PaintBand> bands = make_even_bands({5, 6}, 0.0, 10.0);

    CHECK(band_index_for_value(bands, -0.001) == -1);
    CHECK(band_index_for_value(bands, 10.001) == -1);
    CHECK(band_index_for_value({}, 1.0) == -1);
}

TEST_CASE("band_index_for_value honours explicit ranges as the caller wrote them",
          "[orcamcp][paint]")
{
    // Explicit ranges need not tile the object: a caller may paint two stripes and leave the
    // rest alone. A value in the gap belongs to nothing.
    const std::vector<PaintBand> gapped = {{5, 0.0, 2.0}, {6, 10.0, 12.0}};
    CHECK(band_index_for_value(gapped, 1.0) == 0);
    CHECK(band_index_for_value(gapped, 5.0) == -1);
    CHECK(band_index_for_value(gapped, 11.0) == 1);
    // The largest `to` in the set is inclusive even when the bands do not tile.
    CHECK(band_index_for_value(gapped, 12.0) == 1);
    // ... but the smaller band's `to` is not, so 2.0 belongs to no band here.
    CHECK(band_index_for_value(gapped, 2.0) == -1);

    // Overlapping ranges are a caller's choice, not an error: the first match wins.
    const std::vector<PaintBand> overlapping = {{5, 0.0, 8.0}, {6, 4.0, 12.0}};
    CHECK(band_index_for_value(overlapping, 5.0) == 0);
    CHECK(band_index_for_value(overlapping, 9.0) == 1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `error: use of undeclared identifier 'band_index_for_value'`

- [ ] **Step 3: Declare it in the header**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp`, after `make_even_bands`:

```cpp
// Index of the band `value` belongs to, or -1 when it belongs to none.
// Bands are half-open [from, to), so a value sitting exactly on the boundary between two adjacent
// bands lands in the upper one and never in both. The one exception is the largest `to` in the
// set: a value there (within 1e-9) lands in that band, because otherwise every facet on the far
// face of the object would go unpainted. Bands need not tile and need not be sorted; overlapping
// bands are a caller's choice rather than an error, and the first match wins.
int band_index_for_value(const std::vector<PaintBand>& bands, double value);
```

- [ ] **Step 4: Implement it**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp`:

```cpp
int band_index_for_value(const std::vector<PaintBand>& bands, double value)
{
    for (std::size_t i = 0; i < bands.size(); ++i)
        if (value >= bands[i].from && value < bands[i].to)
            return int(i);

    // Nothing matched. The far end of the painted range is the one closed boundary in the set,
    // so a facet centroid sitting exactly on it belongs to the band that ends there.
    int    best_idx = -1;
    double best_to  = 0.0;
    for (std::size_t i = 0; i < bands.size(); ++i)
        if (best_idx < 0 || bands[i].to > best_to) {
            best_idx = int(i);
            best_to  = bands[i].to;
        }
    if (best_idx >= 0 && std::abs(value - best_to) <= 1e-9)
        return best_idx;
    return -1;
}
```

Add `#include <cmath>` to the top of the .cpp beside `<algorithm>`.

- [ ] **Step 5: Run the test to verify it passes**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[paint]"
```
Expected: PASS — 7 test cases, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp \
        tests/slic3rutils/test_paint_geometry.cpp
git commit -m "$(cat <<'EOF'
feat: added the band lookup that decides which filament a triangle gets

make_even_bands produces boundaries; something has to decide which side of one a facet
centroid sits on. Half-open [from, to) is the rule, so a centroid exactly on a shared
boundary lands in exactly one band rather than in both or in neither.

The far end of the range is the deliberate exception. Closing every band at both ends
would double-assign shared boundaries; leaving the last one open would drop every facet
whose centroid sits exactly on the object's far face -- which for a flat part is a whole
face, not an edge case.

Explicit ranges are left as the caller wrote them: not required to tile, not sorted, and
overlaps resolved first-match rather than rejected. A caller painting two stripes and
leaving the rest alone is a legitimate request, and the response reports how many facets
landed nowhere so the caller can tell the difference.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Region predicates — box and sphere

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp`
- Test: `tests/slic3rutils/test_paint_geometry.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `struct PaintBox { Vec3d min; Vec3d max; }`,
  `struct PaintSphere { Vec3d center; double radius; }`,
  `bool point_in_box(const PaintBox&, const Vec3d&)`,
  `bool point_in_sphere(const PaintSphere&, const Vec3d&)`,
  `bool paint_box_is_valid(const PaintBox&)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_paint_geometry.cpp`:

```cpp
TEST_CASE("point_in_box is inclusive on every face", "[orcamcp][paint]")
{
    const PaintBox box{Vec3d(0.0, 0.0, 0.0), Vec3d(10.0, 20.0, 5.0)};

    CHECK(point_in_box(box, Vec3d(5.0, 10.0, 2.5)));
    // A facet centroid exactly on a face is inside: excluding it would silently drop the
    // triangles a caller most obviously meant to include when the box is the object's own bbox.
    CHECK(point_in_box(box, Vec3d(0.0, 0.0, 0.0)));
    CHECK(point_in_box(box, Vec3d(10.0, 20.0, 5.0)));

    CHECK_FALSE(point_in_box(box, Vec3d(-0.001, 10.0, 2.5)));
    CHECK_FALSE(point_in_box(box, Vec3d(5.0, 20.001, 2.5)));
    CHECK_FALSE(point_in_box(box, Vec3d(5.0, 10.0, 5.001)));
}

TEST_CASE("paint_box_is_valid rejects an inverted or flat box", "[orcamcp][paint]")
{
    CHECK(paint_box_is_valid({Vec3d(0.0, 0.0, 0.0), Vec3d(10.0, 20.0, 5.0)}));
    // min > max on any axis is a caller mistake, not an empty selection: report it.
    CHECK_FALSE(paint_box_is_valid({Vec3d(10.0, 0.0, 0.0), Vec3d(0.0, 20.0, 5.0)}));
    CHECK_FALSE(paint_box_is_valid({Vec3d(0.0, 0.0, 6.0), Vec3d(10.0, 20.0, 5.0)}));
    // Zero thickness on one axis is legal -- it selects the facets whose centroid lies in a plane.
    CHECK(paint_box_is_valid({Vec3d(0.0, 0.0, 5.0), Vec3d(10.0, 20.0, 5.0)}));
}

TEST_CASE("point_in_sphere includes the surface", "[orcamcp][paint]")
{
    const PaintSphere sphere{Vec3d(100.0, 100.0, 3.0), 5.0};

    CHECK(point_in_sphere(sphere, Vec3d(100.0, 100.0, 3.0)));
    CHECK(point_in_sphere(sphere, Vec3d(105.0, 100.0, 3.0)));
    CHECK(point_in_sphere(sphere, Vec3d(103.0, 104.0, 3.0)));   // 3-4-5
    CHECK_FALSE(point_in_sphere(sphere, Vec3d(105.001, 100.0, 3.0)));
    // A radius of zero selects nothing but the exact centre, and never crashes.
    CHECK(point_in_sphere({Vec3d(0.0, 0.0, 0.0), 0.0}, Vec3d(0.0, 0.0, 0.0)));
    CHECK_FALSE(point_in_sphere({Vec3d(0.0, 0.0, 0.0), 0.0}, Vec3d(0.1, 0.0, 0.0)));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `error: unknown type name 'PaintBox'`

- [ ] **Step 3: Declare them in the header**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp`:

```cpp
// An axis-aligned box in plate millimetres.
struct PaintBox
{
    Vec3d min = Vec3d::Zero();
    Vec3d max = Vec3d::Zero();
};

// A sphere in plate millimetres.
struct PaintSphere
{
    Vec3d  center = Vec3d::Zero();
    double radius = 0.0;
};

// min <= max on every axis. A box that is flat on one axis is valid (it selects the facets whose
// centroid lies in that plane); a box whose min exceeds its max is a caller mistake, and the tool
// layer reports it rather than treating it as an empty selection.
bool paint_box_is_valid(const PaintBox& box);

// Inclusive on every face: a facet centroid exactly on a boundary is inside. Excluding it would
// silently drop the facets a caller most obviously meant to include when the box is the object's
// own bounding box.
bool point_in_box(const PaintBox& box, const Vec3d& p);

// Inclusive of the surface.
bool point_in_sphere(const PaintSphere& sphere, const Vec3d& p);
```

- [ ] **Step 4: Implement them**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp`:

```cpp
bool paint_box_is_valid(const PaintBox& box)
{
    return box.min.x() <= box.max.x() && box.min.y() <= box.max.y() && box.min.z() <= box.max.z();
}

bool point_in_box(const PaintBox& box, const Vec3d& p)
{
    return p.x() >= box.min.x() && p.x() <= box.max.x() &&
           p.y() >= box.min.y() && p.y() <= box.max.y() &&
           p.z() >= box.min.z() && p.z() <= box.max.z();
}

bool point_in_sphere(const PaintSphere& sphere, const Vec3d& p)
{
    // Compare squared lengths so a zero radius needs no special case and no sqrt is taken
    // once per facet of a mesh that can carry hundreds of thousands of them.
    return (p - sphere.center).squaredNorm() <= sphere.radius * sphere.radius;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[paint]"
```
Expected: PASS — 10 test cases, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp \
        tests/slic3rutils/test_paint_geometry.cpp
git commit -m "$(cat <<'EOF'
feat: added the box and sphere predicates paint_object selects regions with

Bands cover the 80% case, but the spec asks for region selection too, and it is the same
per-facet decision with a different predicate. Both are inclusive of their boundary: a
caller who passes the object's own bounding box as the box means "all of it", and an
exclusive test would drop every facet centroid that landed exactly on a face.

point_in_sphere compares squared lengths. A zero radius then needs no special case, and
no square root is taken once per facet on meshes that routinely carry hundreds of
thousands of them.

An inverted box is reported rather than treated as an empty selection, because "nothing
was painted" and "your min and max are the wrong way round" are different answers to a
caller. A flat box is left legal -- it selects the facets whose centroid lies in a plane.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Mesh maths — facet centroids in the plate frame, and surface area

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp`
- Test: `tests/slic3rutils/test_paint_geometry.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  `std::vector<Vec3d> facet_centroids(const indexed_triangle_set& its, const Transform3d& to_plate)`,
  `double its_surface_area(const indexed_triangle_set& its)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_paint_geometry.cpp` (and add `#include "libslic3r/TriangleMesh.hpp"`
to the top of the file if it is not already pulled in by the geometry header — it is, but the
explicit include documents the dependency):

```cpp
namespace {

// Two right triangles forming a 4 x 6 rectangle in the z = 0 plane. Small enough to check
// every number by hand, which is the point: the centroid is what decides a facet's band.
indexed_triangle_set two_triangle_rectangle()
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(4.f, 0.f, 0.f), Vec3f(4.f, 6.f, 0.f), Vec3f(0.f, 6.f, 0.f)};
    its.indices  = {Vec3i32(0, 1, 2), Vec3i32(0, 2, 3)};
    return its;
}

} // namespace

TEST_CASE("facet_centroids returns one plate-frame centroid per facet, in facet order",
          "[orcamcp][paint]")
{
    const indexed_triangle_set its = two_triangle_rectangle();

    const std::vector<Vec3d> identity = facet_centroids(its, Transform3d::Identity());
    REQUIRE(identity.size() == 2);
    // (0,0,0) (4,0,0) (4,6,0) -> mean is (8/3, 2, 0)
    CHECK_THAT(identity[0].x(), WithinAbs(8.0 / 3.0, 1e-9));
    CHECK_THAT(identity[0].y(), WithinAbs(2.0, 1e-9));
    CHECK_THAT(identity[0].z(), WithinAbs(0.0, 1e-9));
    // (0,0,0) (4,6,0) (0,6,0) -> mean is (4/3, 4, 0)
    CHECK_THAT(identity[1].x(), WithinAbs(4.0 / 3.0, 1e-9));
    CHECK_THAT(identity[1].y(), WithinAbs(4.0, 1e-9));

    // The transform is what turns mesh coordinates into the plate coordinates a caller
    // names its bands in, so it has to be applied to the centroid, not ignored.
    Transform3d to_plate = Transform3d::Identity();
    to_plate.translate(Vec3d(150.0, 150.0, 0.0));
    const std::vector<Vec3d> moved = facet_centroids(its, to_plate);
    REQUIRE(moved.size() == 2);
    CHECK_THAT(moved[0].x(), WithinAbs(150.0 + 8.0 / 3.0, 1e-9));
    CHECK_THAT(moved[1].y(), WithinAbs(154.0, 1e-9));

    CHECK(facet_centroids(indexed_triangle_set(), Transform3d::Identity()).empty());
}

TEST_CASE("its_surface_area totals the facet areas", "[orcamcp][paint]")
{
    // 4 x 6 rectangle split into two triangles: 24 mm2 total.
    CHECK_THAT(its_surface_area(two_triangle_rectangle()), WithinAbs(24.0, 1e-9));
    CHECK_THAT(its_surface_area(indexed_triangle_set()), WithinAbs(0.0, 1e-12));

    // A degenerate facet contributes nothing rather than a NaN.
    indexed_triangle_set degenerate;
    degenerate.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(2.f, 0.f, 0.f)};
    degenerate.indices  = {Vec3i32(0, 1, 2)};
    CHECK_THAT(its_surface_area(degenerate), WithinAbs(0.0, 1e-12));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `error: use of undeclared identifier 'facet_centroids'`

- [ ] **Step 3: Declare them in the header**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp`:

```cpp
// Centroid of every facet of `its`, in the frame `to_plate` maps the mesh into -- for this
// feature always instance.get_matrix() * volume.get_matrix(), i.e. plate millimetres.
// Index i of the result is facet i of `its`, which is the same index
// TriangleSelector::set_facet takes (TriangleSelector.hpp:361), so the two line up directly.
// A facet belongs to the band or region containing its centroid: one facet, one answer, no
// partially painted triangles and no subdivision.
std::vector<Vec3d> facet_centroids(const indexed_triangle_set& its, const Transform3d& to_plate);

// Total area of `its` in the mesh's own units. Used only as the denominator of a coverage
// ratio, so the units cancel and a scaled instance reports the same coverage as an unscaled one.
double its_surface_area(const indexed_triangle_set& its);
```

- [ ] **Step 4: Implement them**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp`:

```cpp
std::vector<Vec3d> facet_centroids(const indexed_triangle_set& its, const Transform3d& to_plate)
{
    std::vector<Vec3d> centroids;
    centroids.reserve(its.indices.size());
    for (const Vec3i32& face : its.indices) {
        const Vec3d a = its.vertices[face[0]].cast<double>();
        const Vec3d b = its.vertices[face[1]].cast<double>();
        const Vec3d c = its.vertices[face[2]].cast<double>();
        // Transform the centroid rather than the three vertices: the transform is affine, so
        // the two agree, and this is one matrix multiply per facet instead of three.
        centroids.push_back(to_plate * ((a + b + c) / 3.0));
    }
    return centroids;
}

double its_surface_area(const indexed_triangle_set& its)
{
    double area = 0.0;
    for (const Vec3i32& face : its.indices) {
        const Vec3d a = its.vertices[face[0]].cast<double>();
        const Vec3d b = its.vertices[face[1]].cast<double>();
        const Vec3d c = its.vertices[face[2]].cast<double>();
        area += 0.5 * (b - a).cross(c - a).norm();
    }
    return area;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[paint]"
```
Expected: PASS — 12 test cases, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp \
        tests/slic3rutils/test_paint_geometry.cpp
git commit -m "$(cat <<'EOF'
feat: added the facet centroid and area maths the paint selections run on

A facet belongs to the band or region containing its centroid: one facet, one answer, no
partially painted triangles and no subdivision of the mesh. Facet data lives in
volume-local mesh coordinates while every coordinate this API takes is in plate
millimetres, so the centroid is transformed by instance.get_matrix() *
volume.get_matrix() before anything is compared against a band boundary.

The centroid is transformed rather than the three vertices. The transform is affine, so
the results agree, and it is one matrix multiply per facet instead of three on meshes
that routinely carry hundreds of thousands.

its_surface_area exists only as the denominator of the coverage ratio the read-back
reports. Facet counts alone are misleading, because a triangle a gizmo split earlier
counts the same as one that covers a whole face; an area ratio is honest and, being a
ratio, is unaffected by a scaled instance.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Per-facet assignment — the whole selection resolved in one pure call

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp`
- Test: `tests/slic3rutils/test_paint_geometry.cpp`

**Interfaces:**
- Consumes: `PaintAxis`, `PaintBand`, `make_even_bands`, `band_index_for_value`, `PaintBox`,
  `PaintSphere`, `point_in_box`, `point_in_sphere` from Tasks 1-3.
- Produces:
  `struct FacetAssignment { std::vector<int> states; std::vector<int> band_counts; int unassigned; }`,
  `FacetAssignment assign_bands(const std::vector<Vec3d>& centroids, PaintAxis axis, const std::vector<PaintBand>& bands)`,
  `FacetAssignment assign_box(const std::vector<Vec3d>& centroids, const PaintBox& box, int state)`,
  `FacetAssignment assign_sphere(const std::vector<Vec3d>& centroids, const PaintSphere& sphere, int state)`,
  `FacetAssignment assign_all(std::size_t facet_count, int state)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_paint_geometry.cpp`:

```cpp
namespace {

// A 40 x 122 x 6 box centred at plate (150, 150), lying flat with its length along Y --
// the print-bed scraper's shape, tessellated coarsely enough to check by hand. Plate extents
// are x 130..170, y 89..211, z 0..6.
indexed_triangle_set scraper_like_box()
{
    indexed_triangle_set its;
    const double x0 = 130.0, x1 = 170.0, y0 = 89.0, y1 = 211.0, z1 = 6.0;
    // Two facets per band-worth of length, so every band is guaranteed a facet: 14 slabs.
    for (int i = 0; i < 14; ++i) {
        const double ya = y0 + (y1 - y0) * (double(i) / 14.0);
        const double yb = y0 + (y1 - y0) * (double(i + 1) / 14.0);
        const int    base = int(its.vertices.size());
        its.vertices.push_back(Vec3f(float(x0), float(ya), float(z1)));
        its.vertices.push_back(Vec3f(float(x1), float(ya), float(z1)));
        its.vertices.push_back(Vec3f(float(x1), float(yb), float(z1)));
        its.vertices.push_back(Vec3f(float(x0), float(yb), float(z1)));
        its.indices.push_back(Vec3i32(base, base + 1, base + 2));
        its.indices.push_back(Vec3i32(base, base + 2, base + 3));
    }
    return its;
}

} // namespace

TEST_CASE("assign_bands paints the scraper in 14 even bands along Y with slots 5-18",
          "[orcamcp][paint]")
{
    // This is the session's original request (T5) reduced to arithmetic: 14 even sections
    // along the 122 mm length, one mixed filament slot each, slots 5 through 18.
    const indexed_triangle_set its       = scraper_like_box();
    const std::vector<Vec3d>   centroids = facet_centroids(its, Transform3d::Identity());

    std::vector<int> slots;
    for (int slot = 5; slot <= 18; ++slot)
        slots.push_back(slot);
    const std::vector<PaintBand> bands = make_even_bands(slots, 89.0, 211.0);

    const FacetAssignment assignment = assign_bands(centroids, PaintAxis::Y, bands);

    REQUIRE(assignment.states.size() == its.indices.size());
    // Every facet of a mesh that spans exactly the banded range lands in a band. A single
    // unassigned facet here would mean a boundary gap, which is the failure this guards.
    CHECK(assignment.unassigned == 0);
    REQUIRE(assignment.band_counts.size() == 14);
    for (int count : assignment.band_counts)
        CHECK(count == 2);
    // Slot order follows Y: the first band along Y is slot 5, the last is slot 18.
    CHECK(assignment.states.front() == 5);
    CHECK(assignment.states.back() == 18);
    for (int i = 0; i < 14; ++i) {
        CHECK(assignment.states[size_t(i) * 2] == 5 + i);
        CHECK(assignment.states[size_t(i) * 2 + 1] == 5 + i);
    }
}

TEST_CASE("assign_bands leaves a facet outside every band alone", "[orcamcp][paint]")
{
    const std::vector<Vec3d> centroids = {Vec3d(0.0, 1.0, 0.0), Vec3d(0.0, 50.0, 0.0),
                                          Vec3d(0.0, 11.0, 0.0)};
    const std::vector<PaintBand> bands = {{5, 0.0, 10.0}, {6, 10.0, 20.0}};

    const FacetAssignment assignment = assign_bands(centroids, PaintAxis::Y, bands);

    REQUIRE(assignment.states.size() == 3);
    CHECK(assignment.states[0] == 5);
    // -1 means "this selection does not cover this facet", which the writer leaves untouched.
    CHECK(assignment.states[1] == -1);
    CHECK(assignment.states[2] == 6);
    CHECK(assignment.unassigned == 1);
    REQUIRE(assignment.band_counts.size() == 2);
    CHECK(assignment.band_counts[0] == 1);
    CHECK(assignment.band_counts[1] == 1);
}

TEST_CASE("assign_bands reads the axis it was given", "[orcamcp][paint]")
{
    const std::vector<Vec3d>     centroids = {Vec3d(1.0, 50.0, 90.0)};
    const std::vector<PaintBand> bands     = {{7, 0.0, 10.0}};

    CHECK(assign_bands(centroids, PaintAxis::X, bands).states[0] == 7);
    CHECK(assign_bands(centroids, PaintAxis::Y, bands).states[0] == -1);
    CHECK(assign_bands(centroids, PaintAxis::Z, bands).states[0] == -1);
}

TEST_CASE("assign_box, assign_sphere and assign_all report what they covered",
          "[orcamcp][paint]")
{
    const std::vector<Vec3d> centroids = {Vec3d(1.0, 1.0, 1.0), Vec3d(50.0, 50.0, 50.0)};

    const FacetAssignment boxed = assign_box(centroids, {Vec3d(0, 0, 0), Vec3d(10, 10, 10)}, 3);
    CHECK(boxed.states[0] == 3);
    CHECK(boxed.states[1] == -1);
    CHECK(boxed.unassigned == 1);
    CHECK(boxed.band_counts.empty());

    const FacetAssignment sphered = assign_sphere(centroids, {Vec3d(0, 0, 0), 2.0}, 2);
    CHECK(sphered.states[0] == 2);
    CHECK(sphered.states[1] == -1);
    CHECK(sphered.unassigned == 1);

    const FacetAssignment everything = assign_all(2, 0);
    CHECK(everything.states.size() == 2);
    CHECK(everything.states[0] == 0);
    CHECK(everything.states[1] == 0);
    CHECK(everything.unassigned == 0);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `error: unknown type name 'FacetAssignment'`

- [ ] **Step 3: Declare them in the header**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp`:

```cpp
// What a selection resolved to, per facet of the volume's mesh.
struct FacetAssignment
{
    // One entry per facet, in facet order. -1 means "this selection does not cover this facet",
    // which the writer leaves at whatever state it already had.
    std::vector<int> states;
    // Facets that landed in each band, parallel to the `bands` argument. Empty for the region
    // and whole-volume selections, which have no bands to count.
    std::vector<int> band_counts;
    // Facets that landed in no band, or outside the region. Reported so a caller can tell an
    // empty selection from a selection whose coordinates missed the object.
    int              unassigned = 0;
};

FacetAssignment assign_bands(const std::vector<Vec3d>&     centroids,
                             PaintAxis                     axis,
                             const std::vector<PaintBand>& bands);
FacetAssignment assign_box(const std::vector<Vec3d>& centroids, const PaintBox& box, int state);
FacetAssignment assign_sphere(const std::vector<Vec3d>& centroids, const PaintSphere& sphere, int state);
FacetAssignment assign_all(std::size_t facet_count, int state);
```

- [ ] **Step 4: Implement them**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp`:

```cpp
namespace {

// Shared shape for the three predicate-driven selections: every facet the predicate accepts
// gets `state`, every other facet is left to whatever it already was.
template<typename Predicate>
FacetAssignment assign_by_predicate(const std::vector<Vec3d>& centroids, int state, Predicate&& inside)
{
    FacetAssignment assignment;
    assignment.states.assign(centroids.size(), -1);
    for (std::size_t i = 0; i < centroids.size(); ++i) {
        if (inside(centroids[i]))
            assignment.states[i] = state;
        else
            ++assignment.unassigned;
    }
    return assignment;
}

} // namespace

FacetAssignment assign_bands(const std::vector<Vec3d>&     centroids,
                             PaintAxis                     axis,
                             const std::vector<PaintBand>& bands)
{
    FacetAssignment assignment;
    assignment.states.assign(centroids.size(), -1);
    assignment.band_counts.assign(bands.size(), 0);

    const int row = int(axis);
    for (std::size_t i = 0; i < centroids.size(); ++i) {
        const int band = band_index_for_value(bands, centroids[i][row]);
        if (band < 0) {
            ++assignment.unassigned;
            continue;
        }
        assignment.states[i] = bands[std::size_t(band)].state;
        ++assignment.band_counts[std::size_t(band)];
    }
    return assignment;
}

FacetAssignment assign_box(const std::vector<Vec3d>& centroids, const PaintBox& box, int state)
{
    return assign_by_predicate(centroids, state, [&box](const Vec3d& p) { return point_in_box(box, p); });
}

FacetAssignment assign_sphere(const std::vector<Vec3d>& centroids, const PaintSphere& sphere, int state)
{
    return assign_by_predicate(centroids, state,
                               [&sphere](const Vec3d& p) { return point_in_sphere(sphere, p); });
}

FacetAssignment assign_all(std::size_t facet_count, int state)
{
    FacetAssignment assignment;
    assignment.states.assign(facet_count, state);
    return assignment;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[paint]"
```
Expected: PASS — 16 test cases, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp \
        tests/slic3rutils/test_paint_geometry.cpp
git commit -m "$(cat <<'EOF'
feat: resolved a whole paint selection to per-facet states without a GUI

The four selections -- bands, box, sphere, whole volume -- now produce one
FacetAssignment: a state per facet, the facets each band took, and the facets the
selection missed. That last number is what lets a caller tell "you painted nothing"
from "your coordinates missed the object", which the old plan of returning only a
success flag could not.

-1 for an uncovered facet, rather than 0, is deliberate: 0 is a real state (unpainted),
so a selection that does not reach a facet has to be distinguishable from one that
deliberately clears it.

The acceptance case from the testing session is covered here as arithmetic: a
40 x 122 x 6 box lying flat with its length along Y, 14 even bands, slots 5 through 18,
asserting every band takes its facets and none fall through a boundary. That is the
request that had no tool at all, now provable without starting the slicer.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Model-level vocabulary — mode, state names, and the plate transform

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp`
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp`
- Modify: `src/slic3r/CMakeLists.txt` (add both after the `OrcaMCPPaintGeometry.*` lines)
- Test: `tests/slic3rutils/test_paint_geometry.cpp`

**Interfaces:**
- Consumes: `PaintAxis`, `PaintBand` and friends from `OrcaMCPPaintGeometry.hpp`.
- Produces: `enum class PaintMode { Color, Support, Seam, FuzzySkin }`,
  `bool parse_paint_mode(const std::string&, PaintMode&)`,
  `const char* paint_mode_name(PaintMode)`,
  `FacetsAnnotation& annotation_for_mode(ModelVolume&, PaintMode)` and its const overload,
  `bool parse_paint_state(PaintMode, const std::string&, int&)`,
  `std::string paint_state_label(PaintMode, int)`,
  `int max_paint_state()`,
  `Transform3d volume_to_plate(const ModelObject&, const ModelVolume&, std::size_t instance_idx)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_paint_geometry.cpp` (and add
`#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp"` and `#include "libslic3r/Model.hpp"`
to the top of the file):

```cpp
namespace {

// A Model built in memory: no Plater, no wxWidgets, no OpenGL. The paint write path is
// libslic3r-only by design so it can be exercised exactly like this.
struct HeadlessObject
{
    Model         model;
    ModelObject*  object = nullptr;
    ModelVolume*  volume = nullptr;
};

HeadlessObject make_headless_object(const indexed_triangle_set& its, const Vec3d& instance_offset)
{
    HeadlessObject built;
    built.object = built.model.add_object();
    // modify_to_center_geometry = false: recentring would move the mesh under the volume
    // matrix and the hand-checked plate coordinates below would stop being hand-checkable.
    const TriangleMesh mesh(its);
    built.volume = built.object->add_volume(mesh, false);
    ModelInstance* instance = built.object->add_instance();
    instance->set_offset(instance_offset);
    return built;
}

} // namespace

TEST_CASE("parse_paint_mode covers the four FacetsAnnotation members", "[orcamcp][paint]")
{
    PaintMode mode = PaintMode::Seam;
    REQUIRE(parse_paint_mode("color", mode));
    CHECK(mode == PaintMode::Color);
    REQUIRE(parse_paint_mode("support", mode));
    CHECK(mode == PaintMode::Support);
    REQUIRE(parse_paint_mode("seam", mode));
    CHECK(mode == PaintMode::Seam);
    REQUIRE(parse_paint_mode("fuzzy_skin", mode));
    CHECK(mode == PaintMode::FuzzySkin);

    CHECK(std::string(paint_mode_name(PaintMode::FuzzySkin)) == "fuzzy_skin");

    PaintMode untouched = PaintMode::Color;
    CHECK_FALSE(parse_paint_mode("mmu", untouched));
    CHECK_FALSE(parse_paint_mode("brim_ear", untouched));   // brim ears are not facets
    CHECK(untouched == PaintMode::Color);
}

TEST_CASE("annotation_for_mode selects the member the GUI gizmo writes", "[orcamcp][paint]")
{
    HeadlessObject built = make_headless_object(two_triangle_rectangle(), Vec3d(0, 0, 0));

    // Identity by address: the four members are the same type, so a wrong mapping here would
    // compile, run, and quietly paint supports when the caller asked for colour.
    CHECK(&annotation_for_mode(*built.volume, PaintMode::Color) == &built.volume->mmu_segmentation_facets);
    CHECK(&annotation_for_mode(*built.volume, PaintMode::Support) == &built.volume->supported_facets);
    CHECK(&annotation_for_mode(*built.volume, PaintMode::Seam) == &built.volume->seam_facets);
    CHECK(&annotation_for_mode(*built.volume, PaintMode::FuzzySkin) == &built.volume->fuzzy_skin_facets);
}

TEST_CASE("parse_paint_state names the enforcer/blocker states, and refuses the wrong ones",
          "[orcamcp][paint]")
{
    int state = -99;
    REQUIRE(parse_paint_state(PaintMode::Support, "none", state));
    CHECK(state == 0);
    REQUIRE(parse_paint_state(PaintMode::Support, "enforcer", state));
    CHECK(state == 1);
    REQUIRE(parse_paint_state(PaintMode::Support, "blocker", state));
    CHECK(state == 2);
    REQUIRE(parse_paint_state(PaintMode::Seam, "blocker", state));
    CHECK(state == 2);

    // FuzzySkin has no blocker: TriangleSelector.hpp:19 aliases FUZZY_SKIN to ENFORCER and
    // GLGizmoFuzzySkin.hpp:30 paints NONE with the right button. Accepting "blocker" would
    // write state 2, which the fuzzy skin code has no meaning for.
    CHECK_FALSE(parse_paint_state(PaintMode::FuzzySkin, "blocker", state));
    REQUIRE(parse_paint_state(PaintMode::FuzzySkin, "enforcer", state));
    CHECK(state == 1);

    // Colour states are filament slot numbers, not names.
    CHECK_FALSE(parse_paint_state(PaintMode::Color, "enforcer", state));
    CHECK_FALSE(parse_paint_state(PaintMode::Support, "yes", state));
}

TEST_CASE("paint_state_label reads a raw state back the way its mode means it", "[orcamcp][paint]")
{
    CHECK(paint_state_label(PaintMode::Color, 0) == "unpainted");
    CHECK(paint_state_label(PaintMode::Color, 5) == "filament 5");
    CHECK(paint_state_label(PaintMode::Support, 0) == "none");
    CHECK(paint_state_label(PaintMode::Support, 1) == "enforcer");
    CHECK(paint_state_label(PaintMode::Support, 2) == "blocker");
    CHECK(paint_state_label(PaintMode::FuzzySkin, 1) == "fuzzy_skin");

    // EnforcerBlockerType stops at Extruder32 (TriangleSelector.hpp:31-32), so a project with
    // more filament slots than that cannot paint the ones above it, and the tool must say so.
    CHECK(max_paint_state() == 32);
}

TEST_CASE("volume_to_plate composes the instance and volume transforms", "[orcamcp][paint]")
{
    HeadlessObject built = make_headless_object(two_triangle_rectangle(), Vec3d(150.0, 150.0, 0.0));

    const Transform3d to_plate = volume_to_plate(*built.object, *built.volume, 0);
    const std::vector<Vec3d> centroids = facet_centroids(built.volume->mesh().its, to_plate);

    REQUIRE(centroids.size() == 2);
    // The mesh centroid (8/3, 2, 0) sits at (150 + 8/3, 152, 0) once the instance offset applies.
    CHECK_THAT(centroids[0].x(), WithinAbs(150.0 + 8.0 / 3.0, 1e-9));
    CHECK_THAT(centroids[0].y(), WithinAbs(152.0, 1e-9));

    // An out-of-range instance falls back to instance 0 rather than reading past the vector.
    CHECK(volume_to_plate(*built.object, *built.volume, 99).isApprox(to_plate));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `fatal error: 'slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp' file not found`

- [ ] **Step 3: Write the header**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "OrcaMCPPaintGeometry.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Which of the four FacetsAnnotation members on a ModelVolume a call addresses. All four are the
// same type and the same machinery (Model.hpp:875-884: supported_facets, seam_facets,
// mmu_segmentation_facets, fuzzy_skin_facets); only the meaning of the per-facet state differs,
// which is why one tool with a mode parameter closes four gizmo-shaped gaps.
enum class PaintMode { Color, Support, Seam, FuzzySkin };

// "color" | "support" | "seam" | "fuzzy_skin", exactly. Returns false and leaves `out` alone
// otherwise. Brim ears are deliberately not a mode: they are BrimPoints on the ModelObject
// (Model.hpp:390), not facets, and have their own tool.
bool        parse_paint_mode(const std::string& name, PaintMode& out);
const char* paint_mode_name(PaintMode mode);

FacetsAnnotation&       annotation_for_mode(ModelVolume& mv, PaintMode mode);
const FacetsAnnotation& annotation_for_mode(const ModelVolume& mv, PaintMode mode);

// The largest state a facet can hold: int(EnforcerBlockerType::ExtruderMax), 32
// (TriangleSelector.hpp:31-32). A project with more filament slots than that cannot paint
// the ones above it, and the tool layer reports that rather than writing a state the
// slicer would clamp away in ModelVolume::update_extruder_count (Model.cpp:2641-2648).
int max_paint_state();

// Raw EnforcerBlockerType value for a caller-supplied state name, for every mode except Color:
// "none" -> 0, "enforcer" -> 1, "blocker" -> 2. FuzzySkin rejects "blocker" (TriangleSelector.hpp:19
// aliases FUZZY_SKIN to ENFORCER, and GLGizmoFuzzySkin.hpp:29-30 paints only FUZZY_SKIN and NONE).
// Color rejects every name: its states are filament slot numbers, which the tool takes as an
// integer, so a name there is a caller mistake worth reporting.
bool parse_paint_state(PaintMode mode, const std::string& name, int& out_state);

// How a raw state reads back for a given mode. Color: "unpainted" / "filament <n>".
// Support and Seam: "none" / "enforcer" / "blocker". FuzzySkin: "none" / "fuzzy_skin".
std::string paint_state_label(PaintMode mode, int state);

// instance.get_matrix() * volume.get_matrix() (Model.hpp:1354 and Model.hpp:1013): volume-local
// mesh coordinates -> plate coordinates. This is the same composition
// ModelObject::instance_bounding_box uses (Model.cpp:1685-1697), which is why the result lines up
// with the bounding_box get_object_info reports. An out-of-range or absent instance falls back to
// instance 0; an object with no instance at all yields the volume matrix alone.
Transform3d volume_to_plate(const ModelObject& obj, const ModelVolume& mv, std::size_t instance_idx);

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 4: Write the implementation**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp
#include "OrcaMCPPaintModel.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

bool parse_paint_mode(const std::string& name, PaintMode& out)
{
    if (name == "color")      { out = PaintMode::Color;     return true; }
    if (name == "support")    { out = PaintMode::Support;   return true; }
    if (name == "seam")       { out = PaintMode::Seam;      return true; }
    if (name == "fuzzy_skin") { out = PaintMode::FuzzySkin; return true; }
    return false;
}

const char* paint_mode_name(PaintMode mode)
{
    switch (mode) {
    case PaintMode::Color:     return "color";
    case PaintMode::Support:   return "support";
    case PaintMode::Seam:      return "seam";
    case PaintMode::FuzzySkin: return "fuzzy_skin";
    }
    return "color";
}

FacetsAnnotation& annotation_for_mode(ModelVolume& mv, PaintMode mode)
{
    switch (mode) {
    case PaintMode::Support:   return mv.supported_facets;
    case PaintMode::Seam:      return mv.seam_facets;
    case PaintMode::FuzzySkin: return mv.fuzzy_skin_facets;
    case PaintMode::Color:     break;
    }
    return mv.mmu_segmentation_facets;
}

const FacetsAnnotation& annotation_for_mode(const ModelVolume& mv, PaintMode mode)
{
    return annotation_for_mode(const_cast<ModelVolume&>(mv), mode);
}

int max_paint_state() { return int(EnforcerBlockerType::ExtruderMax); }

bool parse_paint_state(PaintMode mode, const std::string& name, int& out_state)
{
    if (mode == PaintMode::Color)
        return false;
    if (name == "none")     { out_state = int(EnforcerBlockerType::NONE);     return true; }
    if (name == "enforcer") { out_state = int(EnforcerBlockerType::ENFORCER); return true; }
    if (name == "blocker" && mode != PaintMode::FuzzySkin) {
        out_state = int(EnforcerBlockerType::BLOCKER);
        return true;
    }
    return false;
}

std::string paint_state_label(PaintMode mode, int state)
{
    if (mode == PaintMode::Color)
        return state <= 0 ? std::string("unpainted") : "filament " + std::to_string(state);
    if (state == int(EnforcerBlockerType::NONE))
        return "none";
    if (state == int(EnforcerBlockerType::ENFORCER))
        return mode == PaintMode::FuzzySkin ? "fuzzy_skin" : "enforcer";
    if (state == int(EnforcerBlockerType::BLOCKER))
        return "blocker";
    return "state " + std::to_string(state);
}

Transform3d volume_to_plate(const ModelObject& obj, const ModelVolume& mv, std::size_t instance_idx)
{
    if (obj.instances.empty())
        return mv.get_matrix();
    const std::size_t idx = instance_idx < obj.instances.size() ? instance_idx : 0;
    return obj.instances[idx]->get_matrix() * mv.get_matrix();
}

}}} // namespace Slic3r::GUI::OrcaMCP
```

Add both files to `src/slic3r/CMakeLists.txt` immediately after the `OrcaMCPPaintGeometry.*` lines:

```cmake
    GUI/OrcaMCP/OrcaMCPPaintModel.hpp
    GUI/OrcaMCP/OrcaMCPPaintModel.cpp
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[paint]"
```
Expected: PASS — 21 test cases, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp \
        src/slic3r/CMakeLists.txt \
        tests/slic3rutils/test_paint_geometry.cpp
git commit -m "$(cat <<'EOF'
feat: named the four paint annotations so one tool can address all of them

The four gizmos the MCP layer was missing -- MmSegmentation, FdmSupports, Seam and
FuzzySkin -- write four FacetsAnnotation members of identical type (Model.hpp:875-884).
That is why they are one tool with a mode parameter rather than four tools, and this
commit is the mapping that makes the choice safe. The members being the same type means
a wrong mapping compiles, runs, and quietly paints supports when the caller asked for
colour, so the test asserts the mapping by address.

fuzzy_skin rejects "blocker" rather than accepting it: TriangleSelector.hpp:19 aliases
FUZZY_SKIN to ENFORCER and GLGizmoFuzzySkin.hpp:29-30 paints only FUZZY_SKIN and NONE,
so state 2 has no meaning there. colour rejects every state name, because its states are
filament slot numbers.

Brim ears were deliberately left out of the mode list even though the toolbar audit
groups them with the painting gizmos: they are BrimPoints on the ModelObject
(Model.hpp:390) in object-local coordinates, not facets, and get their own tool.

volume_to_plate composes the same two matrices ModelObject::instance_bounding_box does
(Model.cpp:1685-1697), so plate coordinates here mean exactly what get_object_info's
bounding_box already means.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: The write path — set facets, read them back, clear them

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp`
- Test: `tests/slic3rutils/test_paint_geometry.cpp`

**Interfaces:**
- Consumes: `PaintMode`, `annotation_for_mode`, `volume_to_plate` (Task 6);
  `FacetAssignment`, `facet_centroids`, `its_surface_area`, `assign_bands`, `make_even_bands` (Tasks 1-5).
- Produces:
  `bool apply_facet_states(ModelVolume& mv, PaintMode mode, const std::vector<int>& states, bool replace)`,
  `struct PaintedStateInfo { int state; int facet_count; double area_ratio; }`,
  `std::vector<PaintedStateInfo> read_volume_paint(const ModelVolume& mv, PaintMode mode)`,
  `bool clear_volume_paint(ModelVolume& mv, PaintMode mode)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_paint_geometry.cpp`:

```cpp
TEST_CASE("apply_facet_states writes paint the gizmo's own read path can see", "[orcamcp][paint]")
{
    HeadlessObject built = make_headless_object(two_triangle_rectangle(), Vec3d(0, 0, 0));

    CHECK_FALSE(built.volume->is_mm_painted());
    CHECK(apply_facet_states(*built.volume, PaintMode::Color, {5, 6}, true));
    CHECK(built.volume->is_mm_painted());

    // ModelVolume::get_extruders (Model.cpp:2610-2639) is what the slicer and the object list
    // read painted filaments through. If it does not see 5 and 6, nothing downstream will.
    const std::vector<int> extruders = built.volume->get_extruders();
    CHECK(std::find(extruders.begin(), extruders.end(), 5) != extruders.end());
    CHECK(std::find(extruders.begin(), extruders.end(), 6) != extruders.end());
}

TEST_CASE("apply_facet_states leaves -1 facets at whatever they already were", "[orcamcp][paint]")
{
    HeadlessObject built = make_headless_object(two_triangle_rectangle(), Vec3d(0, 0, 0));

    REQUIRE(apply_facet_states(*built.volume, PaintMode::Color, {5, 6}, true));
    // replace = false and -1 for the second facet: facet 0 becomes 7, facet 1 stays 6.
    REQUIRE(apply_facet_states(*built.volume, PaintMode::Color, {7, -1}, false));

    const std::vector<PaintedStateInfo> painted = read_volume_paint(*built.volume, PaintMode::Color);
    REQUIRE(painted.size() == 2);
    CHECK(painted[0].state == 6);
    CHECK(painted[0].facet_count == 1);
    CHECK(painted[1].state == 7);
    CHECK(painted[1].facet_count == 1);

    // replace = true starts from a blank selector, so the earlier paint is gone entirely.
    REQUIRE(apply_facet_states(*built.volume, PaintMode::Color, {9, -1}, true));
    const std::vector<PaintedStateInfo> replaced = read_volume_paint(*built.volume, PaintMode::Color);
    REQUIRE(replaced.size() == 2);
    CHECK(replaced[0].state == 0);          // the -1 facet fell back to unpainted
    CHECK(replaced[1].state == 9);
}

TEST_CASE("read_volume_paint reports facet counts and an area ratio that sums to one",
          "[orcamcp][paint]")
{
    HeadlessObject built = make_headless_object(two_triangle_rectangle(), Vec3d(0, 0, 0));
    REQUIRE(apply_facet_states(*built.volume, PaintMode::Color, {5, 6}, true));

    const std::vector<PaintedStateInfo> painted = read_volume_paint(*built.volume, PaintMode::Color);
    REQUIRE(painted.size() == 2);
    double total = 0.0;
    for (const PaintedStateInfo& info : painted)
        total += info.area_ratio;
    CHECK_THAT(total, WithinAbs(1.0, 1e-9));
    // The two triangles of the 4 x 6 rectangle are equal halves.
    CHECK_THAT(painted[0].area_ratio, WithinAbs(0.5, 1e-9));

    // An unpainted volume reports one entry: every facet at state 0.
    HeadlessObject blank = make_headless_object(two_triangle_rectangle(), Vec3d(0, 0, 0));
    const std::vector<PaintedStateInfo> none = read_volume_paint(*blank.volume, PaintMode::Color);
    REQUIRE(none.size() == 1);
    CHECK(none[0].state == 0);
    CHECK(none[0].facet_count == 2);
}

TEST_CASE("clear_volume_paint resets only the mode it was asked for", "[orcamcp][paint]")
{
    HeadlessObject built = make_headless_object(two_triangle_rectangle(), Vec3d(0, 0, 0));
    REQUIRE(apply_facet_states(*built.volume, PaintMode::Color, {5, 6}, true));
    REQUIRE(apply_facet_states(*built.volume, PaintMode::Support, {1, 1}, true));

    CHECK(clear_volume_paint(*built.volume, PaintMode::Color));
    CHECK_FALSE(built.volume->is_mm_painted());
    // The support paint is untouched: clearing one annotation must not clear its siblings.
    CHECK(built.volume->supported_facets.has_facets(*built.volume, EnforcerBlockerType::ENFORCER));

    // Clearing an already-empty annotation reports that there was nothing to clear.
    CHECK_FALSE(clear_volume_paint(*built.volume, PaintMode::Seam));
}

TEST_CASE("the scraper's 14 bands survive the round trip through FacetsAnnotation",
          "[orcamcp][paint]")
{
    // The acceptance case end to end, minus the GUI: build the part, band it along plate Y,
    // write it, read it back. This is the shape the 3MF writer serialises.
    HeadlessObject built = make_headless_object(scraper_like_box(), Vec3d(0.0, 0.0, 0.0));

    const Transform3d        to_plate  = volume_to_plate(*built.object, *built.volume, 0);
    const std::vector<Vec3d> centroids = facet_centroids(built.volume->mesh().its, to_plate);

    std::vector<int> slots;
    for (int slot = 5; slot <= 18; ++slot)
        slots.push_back(slot);
    const FacetAssignment assignment =
        assign_bands(centroids, PaintAxis::Y, make_even_bands(slots, 89.0, 211.0));
    REQUIRE(assignment.unassigned == 0);
    REQUIRE(apply_facet_states(*built.volume, PaintMode::Color, assignment.states, true));

    const std::vector<PaintedStateInfo> painted = read_volume_paint(*built.volume, PaintMode::Color);
    REQUIRE(painted.size() == 14);
    for (int i = 0; i < 14; ++i) {
        CHECK(painted[size_t(i)].state == 5 + i);
        CHECK(painted[size_t(i)].facet_count == 2);
        CHECK_THAT(painted[size_t(i)].area_ratio, WithinAbs(1.0 / 14.0, 1e-6));
    }
}
```

Add `#include <algorithm>` to the top of the test file for `std::find`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `error: use of undeclared identifier 'apply_facet_states'`

- [ ] **Step 3: Declare them in the header**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp`:

```cpp
// Writes `states` (one raw EnforcerBlockerType value per facet of mv.mesh(), -1 = leave alone)
// into the annotation `mode` selects, the same way GLGizmoMmuSegmentation::update_model_object
// does (GLGizmoMmuSegmentation.cpp:711-723): drive a TriangleSelector with set_facet, then hand
// it to FacetsAnnotation::set. Going through the selector rather than writing the bitstream
// directly is what makes MCP-painted data byte-identical to gizmo-painted data, so it renders
// in the gizmo and round-trips through 3MF unchanged.
// `replace` starts from a blank selector, discarding whatever the volume already carried for
// this mode; otherwise the existing paint is the base and only the listed facets move.
// Returns true when the annotation actually changed (FacetsAnnotation::set's own return).
bool apply_facet_states(ModelVolume& mv, PaintMode mode, const std::vector<int>& states, bool replace);

// One state present on one volume.
struct PaintedStateInfo
{
    int    state       = 0;
    // Leaf triangles at this state. A facet a gizmo split earlier counts more than once, which
    // is why area_ratio and not this number is the honest measure of how much is painted.
    int    facet_count = 0;
    // 0..1 of the volume's total mesh area. A ratio, so a scaled instance reports the same value.
    double area_ratio  = 0.0;
};

// Every state carrying at least one facet, ascending by state, state 0 (unpainted) included so a
// caller can see how much of the volume is still bare. An unpainted volume returns exactly one
// entry, {0, all facets, 1.0}.
std::vector<PaintedStateInfo> read_volume_paint(const ModelVolume& mv, PaintMode mode);

// FacetsAnnotation::reset() on the member `mode` selects, leaving the other three alone.
// Returns false when the annotation was already empty, so a caller can tell "cleared" from
// "there was nothing to clear".
bool clear_volume_paint(ModelVolume& mv, PaintMode mode);
```

- [ ] **Step 4: Implement them**

Append to `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp` (add `#include <algorithm>` at the top):

```cpp
bool apply_facet_states(ModelVolume& mv, PaintMode mode, const std::vector<int>& states, bool replace)
{
    FacetsAnnotation& annotation = annotation_for_mode(mv, mode);

    TriangleSelector selector(mv.mesh());
    if (!replace) {
        // needs_reset = false: the TriangleSelector constructor already reset
        // (TriangleSelector.cpp:1326-1330), exactly as the gizmo relies on
        // (GLGizmoMmuSegmentation.cpp:768-770).
        selector.deserialize(annotation.get_data(), false);
    }

    // set_facet only accepts original triangles (TriangleSelector.hpp:360-361, and its assert at
    // TriangleSelector.cpp:1009), so never index past the mesh even if a caller-side bug hands
    // over a longer vector.
    const std::size_t facet_count = std::min(states.size(), mv.mesh().its.indices.size());
    for (std::size_t i = 0; i < facet_count; ++i)
        if (states[i] >= 0)
            selector.set_facet(int(i), EnforcerBlockerType(states[i]));

    return annotation.set(selector);
}

std::vector<PaintedStateInfo> read_volume_paint(const ModelVolume& mv, PaintMode mode)
{
    const FacetsAnnotation& annotation = annotation_for_mode(mv, mode);

    TriangleSelector selector(mv.mesh());
    selector.deserialize(annotation.get_data(), false);

    const double total_area = its_surface_area(mv.mesh().its);

    std::vector<PaintedStateInfo> painted;
    for (int state = int(EnforcerBlockerType::NONE); state <= max_paint_state(); ++state) {
        const int count = selector.num_facets(EnforcerBlockerType(state));
        if (count == 0)
            continue;
        PaintedStateInfo info;
        info.state       = state;
        info.facet_count = count;
        info.area_ratio  = total_area > 0.0
                               ? its_surface_area(selector.get_facets(EnforcerBlockerType(state))) / total_area
                               : 0.0;
        painted.push_back(info);
    }
    return painted;
}

bool clear_volume_paint(ModelVolume& mv, PaintMode mode)
{
    FacetsAnnotation& annotation = annotation_for_mode(mv, mode);
    if (annotation.empty())
        return false;
    annotation.reset();
    return true;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[paint]"
```
Expected: PASS — 26 test cases, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp \
        tests/slic3rutils/test_paint_geometry.cpp
git commit -m "$(cat <<'EOF'
feat: wrote facet paint through the same path the MMU gizmo commits with

The write drives a TriangleSelector with set_facet and hands it to
FacetsAnnotation::set, which is character for character what
GLGizmoMmuSegmentation::update_model_object does (GLGizmoMmuSegmentation.cpp:711-723).
Writing the serialised bitstream directly would have been shorter and would have
produced data the gizmo and the 3MF writer disagreed with; going through the selector is
what makes MCP-painted and gizmo-painted volumes indistinguishable.

The read-back reports an area ratio alongside the facet count because facet counts alone
lie: a triangle an earlier gizmo stroke subdivided counts the same as one covering a
whole face. State 0 is included in the report so a caller can see how much of the volume
is still bare, which is the difference between "the selection worked" and "the selection
missed".

clear returns false on an already-empty annotation rather than reporting success, and
resets only its own member -- the four annotations are siblings on the same volume and
clearing colour must not take supports with it. The test asserts that directly.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: The tools file and `get_object_paint`

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp:67` (declare the registrar)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:4332-4333` (call it)
- Modify: `src/slic3r/CMakeLists.txt` (add the new .cpp)

**Interfaces:**
- Consumes: `PaintMode`, `parse_paint_mode`, `paint_mode_name`, `paint_state_label`,
  `read_volume_paint`, `volume_to_plate` (Tasks 6-7); `run_on_main_thread`,
  `get_active_warnings_json` (`OrcaMCPCommon.hpp`); `OrcaMCPServer::register_tool`.
- Produces: `void OrcaMCPServer::register_paint_tools()`; the file-local helper
  `bool resolve_paint_target(const nlohmann::json& params, PaintTarget& out, std::string& error)`
  with `struct PaintTarget { ModelObject* object; std::vector<ModelVolume*> volumes;
  std::vector<int> volume_ids; std::size_t instance_idx; int object_id; }` (reused by Tasks
  9-11), `Slic3r::BoundingBoxf3 target_plate_bbox(const PaintTarget&)` and
  `nlohmann::json painted_json(const ModelVolume&, PaintMode)` (reused by Task 9),
  `nlohmann::json brim_ears_json(const ModelObject&, std::size_t)` (reused by Task 11),
  `nlohmann::json bbox_json(const Slic3r::BoundingBoxf3&)`; and the MCP tool `get_object_paint`.

- [ ] **Step 1: Declare the registrar and call it**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp`, after the `register_printer_tools()` declaration
(line 67, immediately before the closing `};`):

```cpp
    // Facet painting and brim ears (OrcaMCPPaintTools.cpp)
    static void register_paint_tools();
```

In `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp`, after `register_printer_tools();` (line 4333):

```cpp
    register_paint_tools();
```

- [ ] **Step 2: Write the tools file with the shared target resolution and `get_object_paint`**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPaintGeometry.hpp"
#include "OrcaMCPPaintModel.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/Model.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// The volumes one call addresses, and the instance whose transform defines plate coordinates.
// Paint lives on the ModelVolume, so it applies to every instance of the object; `instance_idx`
// only decides which instance's frame the caller's coordinates are read in.
struct PaintTarget
{
    Slic3r::ModelObject*              object   = nullptr;
    std::vector<Slic3r::ModelVolume*> volumes;
    std::vector<int>                  volume_ids;
    std::size_t                       instance_idx = 0;
    int                               object_id    = -1;
};

// Main thread only. Reads object_id (required), volume_id (optional, -1 = every model part)
// and instance_id (optional, default 0).
bool resolve_paint_target(const nlohmann::json& params, PaintTarget& out, std::string& error)
{
    Plater*        plater = wxGetApp().plater();
    Slic3r::Model& model  = plater->model();

    const int object_id = params.value("object_id", -1);
    if (object_id < 0 || object_id >= int(model.objects.size())) {
        error = "Invalid object_id " + std::to_string(object_id) + ": the scene has " +
                std::to_string(model.objects.size()) + " objects";
        return false;
    }
    out.object    = model.objects[std::size_t(object_id)];
    out.object_id = object_id;

    const int instance_id = params.value("instance_id", 0);
    if (instance_id < 0 || instance_id >= int(out.object->instances.size())) {
        error = "Invalid instance_id " + std::to_string(instance_id) + ": the object has " +
                std::to_string(out.object->instances.size()) + " instances";
        return false;
    }
    out.instance_idx = std::size_t(instance_id);

    const int volume_id = params.value("volume_id", -1);
    if (volume_id < -1) {
        error = "Invalid volume_id " + std::to_string(volume_id) +
                ": use a 0-based part index, or omit it (or pass -1) for every part of the object";
        return false;
    }
    if (volume_id >= 0) {
        if (volume_id >= int(out.object->volumes.size())) {
            error = "Invalid volume_id " + std::to_string(volume_id) + ": the object has " +
                    std::to_string(out.object->volumes.size()) + " volumes";
            return false;
        }
        Slic3r::ModelVolume* mv = out.object->volumes[std::size_t(volume_id)];
        // Only a model part has a printed surface. A modifier, a support blocker or a negative
        // volume carries the same annotation members but nothing reads them, so painting one
        // would report success and change nothing the caller can see.
        if (!mv->is_model_part()) {
            error = "volume_id " + std::to_string(volume_id) +
                    " is not a model part, so it has no surface to paint";
            return false;
        }
        out.volumes.push_back(mv);
        out.volume_ids.push_back(volume_id);
    } else {
        for (int i = 0; i < int(out.object->volumes.size()); ++i)
            if (out.object->volumes[std::size_t(i)]->is_model_part()) {
                out.volumes.push_back(out.object->volumes[std::size_t(i)]);
                out.volume_ids.push_back(i);
            }
        if (out.volumes.empty()) {
            error = "Object " + std::to_string(object_id) + " has no model parts to paint";
            return false;
        }
    }
    return true;
}

// The plate-frame bounding box of exactly the volumes a call paints, so a band range defaulted
// from it covers what is actually being painted rather than the whole object.
Slic3r::BoundingBoxf3 target_plate_bbox(const PaintTarget& target)
{
    Slic3r::BoundingBoxf3 bbox;
    for (Slic3r::ModelVolume* mv : target.volumes)
        bbox.merge(mv->mesh().transformed_bounding_box(
            volume_to_plate(*target.object, *mv, target.instance_idx)));
    return bbox;
}

nlohmann::json bbox_json(const Slic3r::BoundingBoxf3& bbox)
{
    return {{"min", {{"x", bbox.min.x()}, {"y", bbox.min.y()}, {"z", bbox.min.z()}}},
            {"max", {{"x", bbox.max.x()}, {"y", bbox.max.y()}, {"z", bbox.max.z()}}}};
}

// One mode's paint on one volume, as the response reports it.
nlohmann::json painted_json(const Slic3r::ModelVolume& mv, PaintMode mode)
{
    nlohmann::json states = nlohmann::json::array();
    for (const PaintedStateInfo& info : read_volume_paint(mv, mode)) {
        nlohmann::json entry = {{"state", info.state},
                                {"label", paint_state_label(mode, info.state)},
                                {"facet_count", info.facet_count},
                                {"coverage_percent", info.area_ratio * 100.0}};
        entry["filament"] = mode == PaintMode::Color && info.state > 0
                                ? nlohmann::json(info.state)
                                : nlohmann::json(nullptr);
        states.push_back(entry);
    }
    return states;
}

// The brim ears on an object, converted back into the plate coordinates the API speaks.
// brim_points are stored object-local (Model.hpp:390; Brim.cpp:373 transforms them by the
// instance matrix to get a world position), so the read-back has to transform them forward.
nlohmann::json brim_ears_json(const Slic3r::ModelObject& obj, std::size_t instance_idx)
{
    nlohmann::json ears = nlohmann::json::array();
    const Slic3r::Transform3d to_plate =
        obj.instances.empty() ? Slic3r::Transform3d::Identity()
                              : obj.instances[std::min(instance_idx, obj.instances.size() - 1)]->get_matrix();
    for (const Slic3r::BrimPoint& point : obj.brim_points) {
        const Slic3r::Vec3d plate_pos = to_plate * point.pos.cast<double>();
        ears.push_back({{"x", plate_pos.x()},
                        {"y", plate_pos.y()},
                        {"z", plate_pos.z()},
                        {"radius", double(point.head_front_radius)}});
    }
    return ears;
}

} // namespace

void OrcaMCPServer::register_paint_tools()
{
    register_tool({
        "get_object_paint",
        "Read what is currently painted on an object: per-volume facet counts and surface "
        "coverage for each of the four paint modes (color, support, seam, fuzzy_skin), plus its "
        "brim ears. Coordinates are PLATE millimetres, the same frame get_object_info reports "
        "its bounding_box in. Use it to verify a paint_object call did what you asked.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
                {"volume_id", {
                    {"type", "integer"},
                    {"minimum", -1},
                    {"description", "Part index within the object (0-based); omit, or pass -1, for every part"}
                }},
                {"instance_id", {
                    {"type", "integer"},
                    {"description", "Which instance's transform defines plate coordinates (default 0). "
                                    "Paint is shared by every instance."}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", {"color", "support", "seam", "fuzzy_skin"}},
                    {"description", "Omit to report all four modes"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([params]() -> nlohmann::json {
                PaintTarget target;
                std::string error;
                if (!resolve_paint_target(params, target, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                std::vector<PaintMode> modes = {PaintMode::Color, PaintMode::Support,
                                                PaintMode::Seam, PaintMode::FuzzySkin};
                if (params.contains("mode")) {
                    PaintMode one = PaintMode::Color;
                    if (!parse_paint_mode(params["mode"].get<std::string>(), one))
                        return nlohmann::json{{"status", "error"},
                                              {"message", "Unknown mode; expected color, support, seam or fuzzy_skin"}};
                    modes = {one};
                }

                nlohmann::json volumes = nlohmann::json::array();
                for (std::size_t i = 0; i < target.volumes.size(); ++i) {
                    Slic3r::ModelVolume* mv = target.volumes[i];
                    nlohmann::json by_mode = nlohmann::json::object();
                    for (PaintMode mode : modes)
                        by_mode[paint_mode_name(mode)] = painted_json(*mv, mode);
                    volumes.push_back({
                        {"volume_id", target.volume_ids[i]},
                        {"name", mv->name},
                        {"facets_total", int(mv->mesh().its.indices.size())},
                        {"bounding_box", bbox_json(mv->mesh().transformed_bounding_box(
                                             volume_to_plate(*target.object, *mv, target.instance_idx)))},
                        {"modes", by_mode}
                    });
                }

                return nlohmann::json{
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"coordinate_frame", "plate"},
                    {"instance_id", int(target.instance_idx)},
                    {"bounding_box", bbox_json(target_plate_bbox(target))},
                    {"volumes", volumes},
                    {"brim_ears", brim_ears_json(*target.object, target.instance_idx)}
                };
            });
        }
    });
}
```

Add the file to `src/slic3r/CMakeLists.txt` after the `OrcaMCPPaintModel.*` lines:

```cmake
    GUI/OrcaMCP/OrcaMCPPaintTools.cpp
```

- [ ] **Step 3: Build**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8`
Expected: build succeeds, no warnings from the new file.

- [ ] **Step 4: Confirm the tool is registered**

Run: `grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'`
Expected: `71`

- [ ] **Step 5: Run the test suite to confirm no regression**

Run: `build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests`
Expected: 0 failures. Case count is the 174 baseline plus the 26 `[paint]` cases added so far.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp \
        src/slic3r/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: an agent can now read back what is painted on an object

Read-back lands before the write tool on purpose: without it, the only way to check a
paint call was to open the gizmo, which is exactly the mouse-reaching step this feature
exists to remove. get_object_paint reports every mode's facet counts and surface
coverage, plus the object's brim ears, so an agent can verify its own work in the same
loop it did it in.

Coordinates are plate millimetres and the response says so in a coordinate_frame field.
Brim ears are stored object-local (Brim.cpp:373 transforms them by the instance matrix
before use), so the read-back transforms them forward rather than reporting the raw
stored values, which would have looked like plate coordinates and been wrong by the
instance offset.

resolve_paint_target rejects a volume that is not a model part rather than painting it.
A modifier or a support blocker carries the same four annotation members, so painting
one would have reported success and changed nothing the caller could observe.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: `paint_object`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp` (add to the anonymous namespace, then
  register the tool at the top of `register_paint_tools()`, before `get_object_paint`)

**Interfaces:**
- Consumes: `PaintTarget`, `resolve_paint_target`, `target_plate_bbox`, `bbox_json`,
  `painted_json` (Task 8); every geometry and model function from Tasks 1-7.
- Produces: the MCP tool `paint_object`; the file-local helpers
  `struct PaintRequest`, `bool parse_paint_request(const nlohmann::json&, PaintMode, const PaintTarget&, PaintRequest&, std::string&)`,
  `std::vector<std::string> paint_prerequisite_messages(const Slic3r::ModelObject&, PaintMode)`,
  used again by Task 11.

- [ ] **Step 1: Add the request parsing helper**

Add to the anonymous namespace of `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp`, after
`brim_ears_json`, and add `#include "libslic3r/PresetBundle.hpp"` and
`#include "libslic3r/PrintConfig.hpp"` at the top of the file:

```cpp
// A filament slot a caller asked to paint with. 0 means "unpainted" -- back to whatever filament
// the volume itself is assigned -- which is the colour-mode equivalent of state NONE.
bool validate_color_slot(int slot, std::string& error)
{
    if (slot == 0)
        return true;
    const int filament_count = int(wxGetApp().preset_bundle->filament_presets.size());
    if (slot < 0 || slot > filament_count) {
        error = "filament " + std::to_string(slot) + " out of range 1.." + std::to_string(filament_count) +
                " (0 means unpainted)";
        return false;
    }
    if (slot > max_paint_state()) {
        error = "filament " + std::to_string(slot) + " cannot be painted: a facet state stops at " +
                std::to_string(max_paint_state()) + " (EnforcerBlockerType::ExtruderMax), so slots above "
                "that can only be assigned to a whole object or part with set_object_filament";
        return false;
    }
    return true;
}

// One parsed paint_object call.
struct PaintRequest
{
    PaintMode              mode      = PaintMode::Color;
    std::string            selection;                 // "bands" | "box" | "sphere" | "all"
    PaintAxis              axis      = PaintAxis::Z;
    std::vector<PaintBand> bands;                     // resolved, even split already expanded
    double                 range_from = 0.0;
    double                 range_to   = 0.0;
    PaintBox               box;
    PaintSphere            sphere;
    int                    state     = 0;             // box / sphere / all
    bool                   replace   = true;
};

// Reads the one state a non-band selection paints with: `filament` in colour mode, `state` in
// the other three.
bool parse_single_state(const nlohmann::json& params, PaintMode mode, int& out, std::string& error)
{
    if (mode == PaintMode::Color) {
        if (!params.contains("filament")) {
            error = "mode 'color' needs a `filament` (1-based slot, or 0 to unpaint)";
            return false;
        }
        out = params["filament"].get<int>();
        return validate_color_slot(out, error);
    }
    if (!params.contains("state")) {
        error = std::string("mode '") + paint_mode_name(mode) +
                "' needs a `state`: none, enforcer" + (mode == PaintMode::FuzzySkin ? "" : " or blocker");
        return false;
    }
    if (!parse_paint_state(mode, params["state"].get<std::string>(), out)) {
        error = std::string("Unknown state for mode '") + paint_mode_name(mode) + "': expected none, enforcer" +
                (mode == PaintMode::FuzzySkin
                     ? " (fuzzy skin has no blocker: it is painted or it is not)"
                     : " or blocker");
        return false;
    }
    return true;
}

bool read_vec3(const nlohmann::json& value, Slic3r::Vec3d& out, const char* what, std::string& error)
{
    if (!value.is_array() || value.size() != 3) {
        error = std::string(what) + " must be an array of three numbers [x, y, z] in plate millimetres";
        return false;
    }
    out = Slic3r::Vec3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
    return true;
}

bool parse_paint_request(const nlohmann::json& params,
                        PaintMode             mode,
                        const PaintTarget&    target,
                        PaintRequest&         out,
                        std::string&          error)
{
    out.mode    = mode;
    out.replace = params.value("replace", true);

    out.selection = params.value("selection", std::string());
    if (out.selection.empty()) {
        error = "selection is required: bands, box, sphere or all";
        return false;
    }

    if (out.selection == "bands") {
        std::string axis_name = params.value("axis", std::string());
        if (!parse_paint_axis(axis_name, out.axis)) {
            error = "selection 'bands' needs an axis: x, y or z (plate axes)";
            return false;
        }

        const Slic3r::BoundingBoxf3 bbox = target_plate_bbox(target);
        const int                   row  = int(out.axis);
        out.range_from = params.contains("from") ? params["from"].get<double>() : bbox.min[row];
        out.range_to   = params.contains("to") ? params["to"].get<double>() : bbox.max[row];

        if (params.contains("bands")) {
            const nlohmann::json& raw = params["bands"];
            if (!raw.is_array() || raw.empty()) {
                error = "bands must be a non-empty array of {from, to, filament|state}";
                return false;
            }
            for (const nlohmann::json& entry : raw) {
                PaintBand band;
                if (!entry.contains("from") || !entry.contains("to")) {
                    error = "every band needs `from` and `to` in plate millimetres";
                    return false;
                }
                band.from = entry["from"].get<double>();
                band.to   = entry["to"].get<double>();
                if (!(band.to > band.from)) {
                    error = "band from " + std::to_string(band.from) + " to " + std::to_string(band.to) +
                            " is empty: `to` must exceed `from`";
                    return false;
                }
                if (!parse_single_state(entry, mode, band.state, error))
                    return false;
                out.bands.push_back(band);
            }
            // An explicit set defines its own range; report the span it actually covers.
            out.range_from = out.bands.front().from;
            out.range_to   = out.bands.front().to;
            for (const PaintBand& band : out.bands) {
                out.range_from = std::min(out.range_from, band.from);
                out.range_to   = std::max(out.range_to, band.to);
            }
            return true;
        }

        if (mode != PaintMode::Color) {
            error = std::string("mode '") + paint_mode_name(mode) +
                    "' has no even-split form: pass explicit `bands` with a `state` each. An even "
                    "split alternates filaments, which only means something in colour mode";
            return false;
        }
        if (!params.contains("filaments") || !params["filaments"].is_array() || params["filaments"].empty()) {
            error = "selection 'bands' needs either `filaments` (one 1-based slot per band, split "
                    "evenly) or explicit `bands`";
            return false;
        }
        std::vector<int> slots;
        for (const nlohmann::json& slot : params["filaments"]) {
            const int value = slot.get<int>();
            if (!validate_color_slot(value, error))
                return false;
            slots.push_back(value);
        }
        out.bands = make_even_bands(slots, out.range_from, out.range_to);
        if (out.bands.empty()) {
            error = "cannot split " + std::to_string(out.range_from) + ".." + std::to_string(out.range_to) +
                    " into " + std::to_string(slots.size()) +
                    " bands: `to` must exceed `from`. With no from/to the object's own extent along "
                    "the axis is used, so a zero span means the object is flat on that axis";
            return false;
        }
        return true;
    }

    if (out.selection == "box") {
        if (!params.contains("box") || !params["box"].contains("min") || !params["box"].contains("max")) {
            error = "selection 'box' needs box: {min: [x,y,z], max: [x,y,z]} in plate millimetres";
            return false;
        }
        if (!read_vec3(params["box"]["min"], out.box.min, "box.min", error) ||
            !read_vec3(params["box"]["max"], out.box.max, "box.max", error))
            return false;
        if (!paint_box_is_valid(out.box)) {
            error = "box.min must not exceed box.max on any axis";
            return false;
        }
        return parse_single_state(params, mode, out.state, error);
    }

    if (out.selection == "sphere") {
        if (!params.contains("sphere") || !params["sphere"].contains("center") ||
            !params["sphere"].contains("radius")) {
            error = "selection 'sphere' needs sphere: {center: [x,y,z], radius: n} in plate millimetres";
            return false;
        }
        if (!read_vec3(params["sphere"]["center"], out.sphere.center, "sphere.center", error))
            return false;
        out.sphere.radius = params["sphere"]["radius"].get<double>();
        if (!(out.sphere.radius > 0.0)) {
            error = "sphere.radius must be greater than zero";
            return false;
        }
        return parse_single_state(params, mode, out.state, error);
    }

    if (out.selection == "all")
        return parse_single_state(params, mode, out.state, error);

    error = "Unknown selection '" + out.selection + "': expected bands, box, sphere or all";
    return false;
}

// Painted supports and painted fuzzy skin do nothing unless the corresponding setting is on.
// The gizmos say so on screen (GLGizmoFdmSupports.cpp:543, GLGizmoFuzzySkin.cpp:326-331); over
// MCP the equivalent is an info message, or the caller paints, slices, and sees no difference.
std::vector<std::string> paint_prerequisite_messages(const Slic3r::ModelObject& obj, PaintMode mode)
{
    std::vector<std::string> messages;
    const Slic3r::DynamicPrintConfig& global = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const Slic3r::DynamicPrintConfig& object_cfg = obj.config.get();

    if (mode == PaintMode::Support) {
        const bool enabled = object_cfg.option("enable_support") ? object_cfg.opt_bool("enable_support")
                                                                 : global.opt_bool("enable_support");
        if (!enabled)
            messages.push_back("Painted support enforcers and blockers have no effect while "
                               "enable_support is false. Set it with apply_config or set_object_config.");
    }
    if (mode == PaintMode::FuzzySkin) {
        const Slic3r::FuzzySkinType effective =
            object_cfg.option("fuzzy_skin") ? object_cfg.opt_enum<Slic3r::FuzzySkinType>("fuzzy_skin")
                                            : global.opt_enum<Slic3r::FuzzySkinType>("fuzzy_skin");
        if (effective == Slic3r::FuzzySkinType::Disabled_fuzzy)
            messages.push_back("Painted fuzzy skin has no effect while fuzzy_skin is 'disabled_fuzzy' "
                               "(the default). Set fuzzy_skin to 'none' to use painted regions only.");
    }
    return messages;
}
```

- [ ] **Step 2: Register the tool**

Add at the top of `register_paint_tools()`, before the `get_object_paint` registration:

```cpp
    register_tool({
        "paint_object",
        "Paint per-triangle annotations on an object, the same data the GUI paint gizmos write. "
        "mode selects which: color (multi-material / MMU segmentation), support, seam or "
        "fuzzy_skin. selection selects where: bands along a plate axis (an even split across a "
        "list of filaments, or explicit ranges), a box, a sphere, or the whole volume. "
        "ALL COORDINATES ARE PLATE MILLIMETRES -- the same frame get_object_info reports its "
        "bounding_box and position in, not object-local coordinates. A facet belongs to the band "
        "or region containing its centroid. Paint lives on the volume, so it applies to every "
        "instance; instance_id only says whose transform reads your coordinates. Verify with "
        "get_object_paint, undo with undo, reset with clear_object_paint.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
                {"volume_id", {
                    {"type", "integer"},
                    {"minimum", -1},
                    {"description", "Part index within the object (0-based); omit, or pass -1, for every part"}
                }},
                {"instance_id", {
                    {"type", "integer"},
                    {"description", "Which instance's transform reads your coordinates (default 0)"}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", {"color", "support", "seam", "fuzzy_skin"}},
                    {"description", "Which annotation to write (default: color)"}
                }},
                {"selection", {
                    {"type", "string"},
                    {"enum", {"bands", "box", "sphere", "all"}},
                    {"description", "Where to paint"}
                }},
                {"axis", {
                    {"type", "string"},
                    {"enum", {"x", "y", "z"}},
                    {"description", "Plate axis the bands run along (selection=bands)"}
                }},
                {"filaments", {
                    {"type", "array"},
                    {"items", {{"type", "integer"}}},
                    {"description", "One 1-based filament slot per band, split evenly along the axis "
                                    "(selection=bands, mode=color). 0 means unpainted."}
                }},
                {"bands", {
                    {"type", "array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"from", {{"type", "number"}}},
                            {"to", {{"type", "number"}}},
                            {"filament", {{"type", "integer"}}},
                            {"state", {{"type", "string"}, {"enum", {"none", "enforcer", "blocker"}}}}
                        }}
                    }},
                    {"description", "Explicit ranges in plate mm, each with a filament (mode=color) or "
                                    "a state. Ranges need not tile the object; facets outside them all "
                                    "are left alone."}
                }},
                {"from", {{"type", "number"}, {"description", "Start of the even split in plate mm "
                                                              "(default: the object's own extent)"}}},
                {"to", {{"type", "number"}, {"description", "End of the even split in plate mm "
                                                            "(default: the object's own extent)"}}},
                {"box", {
                    {"type", "object"},
                    {"properties", {
                        {"min", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                        {"max", {{"type", "array"}, {"items", {{"type", "number"}}}}}
                    }},
                    {"description", "Axis-aligned box in plate mm (selection=box)"}
                }},
                {"sphere", {
                    {"type", "object"},
                    {"properties", {
                        {"center", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                        {"radius", {{"type", "number"}}}
                    }},
                    {"description", "Sphere in plate mm (selection=sphere)"}
                }},
                {"filament", {
                    {"type", "integer"},
                    {"description", "1-based filament slot for selection=box/sphere/all (mode=color). "
                                    "0 means unpainted."}
                }},
                {"state", {
                    {"type", "string"},
                    {"enum", {"none", "enforcer", "blocker"}},
                    {"description", "State for selection=box/sphere/all when mode is not color. "
                                    "fuzzy_skin accepts none and enforcer only."}
                }},
                {"replace", {
                    {"type", "boolean"},
                    {"description", "true (default) discards this mode's existing paint first; "
                                    "false paints on top of it"}
                }}
            }},
            {"required", {"object_id", "selection"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([params]() -> nlohmann::json {
                Plater*     plater = wxGetApp().plater();
                PaintTarget target;
                std::string error;
                if (!resolve_paint_target(params, target, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                PaintMode mode = PaintMode::Color;
                if (params.contains("mode") && !parse_paint_mode(params["mode"].get<std::string>(), mode))
                    return nlohmann::json{{"status", "error"},
                                          {"message", "Unknown mode; expected color, support, seam or fuzzy_skin"}};

                PaintRequest request;
                if (!parse_paint_request(params, mode, target, request, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                // Everything is validated -- only now touch the undo stack, the way
                // set_object_filament does (OrcaMCPFilamentUtils.cpp:150-151).
                plater->take_snapshot(_u8L("Paint Object"));

                int            facets_painted    = 0;
                int            facets_total      = 0;
                int            facets_unassigned = 0;
                std::vector<int> band_counts(request.bands.size(), 0);
                bool           changed = false;

                for (Slic3r::ModelVolume* mv : target.volumes) {
                    const Slic3r::Transform3d to_plate =
                        volume_to_plate(*target.object, *mv, target.instance_idx);
                    const std::vector<Slic3r::Vec3d> centroids = facet_centroids(mv->mesh().its, to_plate);
                    facets_total += int(centroids.size());

                    FacetAssignment assignment;
                    if (request.selection == "bands")
                        assignment = assign_bands(centroids, request.axis, request.bands);
                    else if (request.selection == "box")
                        assignment = assign_box(centroids, request.box, request.state);
                    else if (request.selection == "sphere")
                        assignment = assign_sphere(centroids, request.sphere, request.state);
                    else
                        assignment = assign_all(centroids.size(), request.state);

                    facets_unassigned += assignment.unassigned;
                    for (std::size_t i = 0; i < assignment.band_counts.size() && i < band_counts.size(); ++i)
                        band_counts[i] += assignment.band_counts[i];
                    facets_painted += int(assignment.states.size()) - assignment.unassigned;

                    changed |= apply_facet_states(*mv, mode, assignment.states, request.replace);
                }

                wxGetApp().obj_list()->update_info_items(std::size_t(target.object_id));
                plater->get_partplate_list().notify_instance_update(target.object_id, int(target.instance_idx));
                plater->update();

                nlohmann::json volumes = nlohmann::json::array();
                for (std::size_t i = 0; i < target.volumes.size(); ++i)
                    volumes.push_back({{"volume_id", target.volume_ids[i]},
                                       {"painted", painted_json(*target.volumes[i], mode)}});

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"mode", paint_mode_name(mode)},
                    {"selection", request.selection},
                    {"coordinate_frame", "plate"},
                    {"instance_id", int(target.instance_idx)},
                    {"replace", request.replace},
                    {"annotation_changed", changed},
                    {"facets_total", facets_total},
                    {"facets_painted", facets_painted},
                    {"facets_unassigned", facets_unassigned},
                    {"volumes", volumes},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                if (request.selection == "bands") {
                    result["axis"] = paint_axis_name(request.axis);
                    result["axis_range"] = {{"from", request.range_from}, {"to", request.range_to}};
                    nlohmann::json bands = nlohmann::json::array();
                    for (std::size_t i = 0; i < request.bands.size(); ++i) {
                        nlohmann::json band = {{"from", request.bands[i].from},
                                               {"to", request.bands[i].to},
                                               {"state", request.bands[i].state},
                                               {"label", paint_state_label(mode, request.bands[i].state)},
                                               {"facet_count", band_counts[i]}};
                        band["filament"] = mode == PaintMode::Color && request.bands[i].state > 0
                                               ? nlohmann::json(request.bands[i].state)
                                               : nlohmann::json(nullptr);
                        bands.push_back(band);
                    }
                    result["bands"] = bands;
                }

                std::vector<std::string> messages = paint_prerequisite_messages(*target.object, mode);
                if (facets_unassigned > 0 && request.selection != "bands")
                    messages.push_back(std::to_string(facets_unassigned) +
                                       " facets fell outside the selection and kept their previous state.");
                if (facets_painted == 0)
                    messages.push_back("Nothing was painted: no facet centroid fell inside the selection. "
                                       "Check the coordinates against get_object_info's bounding_box, which "
                                       "is in the same plate frame.");
                if (!messages.empty())
                    result["info_messages"] = messages;

                return result;
            });
        }
    });
```

- [ ] **Step 3: Build**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8`
Expected: build succeeds.

- [ ] **Step 4: Confirm the registration count**

Run: `grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'`
Expected: `72`

- [ ] **Step 5: Run the test suite**

Run: `build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests`
Expected: 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
git commit -m "$(cat <<'EOF'
feat: an agent can now paint a model, which is the half of the colour feature that was missing

suggest_color_mix and get_color_palette could propose a palette and nothing could apply
it, so the user had to pick up the mouse for the one step the feature exists to automate
(T5). paint_object closes that, and with it four of the missing gizmos at once:
MmSegmentation, FdmSupports, Seam and FuzzySkin all write FacetsAnnotation members of
identical type, so they are one tool with a mode parameter.

Coordinates are plate millimetres, stated in the tool description rather than left to be
inferred. get_object_info already reports a plate-frame bounding_box, so a caller can
read min.y and max.y and hand them straight back; object-local would have required the
caller to undo the instance transform first.

The response reports facets_unassigned and per-band facet counts. Without them "the call
succeeded" and "your coordinates missed the object" look identical, which is the failure
mode a geometric selection actually has.

Painted supports and painted fuzzy skin are inert unless enable_support / fuzzy_skin are
set, which the gizmos warn about on screen; over MCP that becomes an info_messages line,
or the caller paints, slices, and sees no difference with nothing to explain it.

Related occurrences checked: set_object_filament (whole object or part) and
set_object_layer_range (Z ranges) were both left alone -- they address different data and
neither was wrong, they simply could not reach a single volume's surface.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: `clear_object_paint`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp` (register after `paint_object`)

**Interfaces:**
- Consumes: `PaintTarget`, `resolve_paint_target` (Task 8); `clear_volume_paint`,
  `parse_paint_mode`, `paint_mode_name` (Tasks 6-7).
- Produces: the MCP tool `clear_object_paint`.

- [ ] **Step 1: Register the tool**

Add to `register_paint_tools()` in `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp`, after the
`paint_object` registration:

```cpp
    register_tool({
        "clear_object_paint",
        "Reset a paint annotation on an object back to unpainted -- the equivalent of the paint "
        "gizmo's 'Remove all' button. mode picks which annotation; omit it to clear all four. "
        "This resets the annotation outright, which is not the same as painting every facet with "
        "state none: the reset leaves no data at all for the 3MF to carry.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
                {"volume_id", {
                    {"type", "integer"},
                    {"minimum", -1},
                    {"description", "Part index within the object (0-based); omit, or pass -1, for every part"}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", {"color", "support", "seam", "fuzzy_skin"}},
                    {"description", "Which annotation to clear; omit to clear all four"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([params]() -> nlohmann::json {
                Plater*     plater = wxGetApp().plater();
                PaintTarget target;
                std::string error;
                if (!resolve_paint_target(params, target, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                std::vector<PaintMode> modes = {PaintMode::Color, PaintMode::Support,
                                                PaintMode::Seam, PaintMode::FuzzySkin};
                if (params.contains("mode")) {
                    PaintMode one = PaintMode::Color;
                    if (!parse_paint_mode(params["mode"].get<std::string>(), one))
                        return nlohmann::json{{"status", "error"},
                                              {"message", "Unknown mode; expected color, support, seam or fuzzy_skin"}};
                    modes = {one};
                }

                // Snapshot before the first write, so one undo restores every mode this call cleared.
                plater->take_snapshot(_u8L("Clear Object Paint"));

                nlohmann::json cleared = nlohmann::json::array();
                bool           changed = false;
                for (PaintMode mode : modes) {
                    int volumes_cleared = 0;
                    for (Slic3r::ModelVolume* mv : target.volumes)
                        if (clear_volume_paint(*mv, mode))
                            ++volumes_cleared;
                    changed |= volumes_cleared > 0;
                    cleared.push_back({{"mode", paint_mode_name(mode)}, {"volumes_cleared", volumes_cleared}});
                }

                wxGetApp().obj_list()->update_info_items(std::size_t(target.object_id));
                plater->get_partplate_list().notify_instance_update(target.object_id, int(target.instance_idx));
                plater->update();

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"cleared", cleared},
                    {"annotation_changed", changed},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
                if (!changed)
                    result["info_messages"] = std::vector<std::string>{
                        "Nothing was cleared: the requested annotations were already empty."};
                return result;
            });
        }
    });
```

- [ ] **Step 2: Build**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8`
Expected: build succeeds.

- [ ] **Step 3: Confirm the registration count**

Run: `grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'`
Expected: `73`

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
git commit -m "$(cat <<'EOF'
feat: an agent can now undo its own painting without undo

paint_object with replace:true already discards a mode's paint, but only as a side
effect of painting something else -- there was no way to get back to an unpainted volume.
clear_object_paint calls FacetsAnnotation::reset, which is what the gizmo's "Remove all"
button does (GLGizmoMmuSegmentation.cpp:681-694).

Resetting is deliberately not the same as painting every facet with state none. A reset
leaves no annotation data for the 3MF writer to carry; painting state none leaves a full
bitstream of zeroes. The tool description says so, because an agent reaching for one and
getting the other would only find out on the next project load.

The snapshot is taken once, before the first mode is cleared, so a single undo restores
every mode a clear-all call touched rather than leaving three of four still cleared.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 11: `set_brim_ears`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp` (register after `clear_object_paint`)

**Interfaces:**
- Consumes: `PaintTarget`, `resolve_paint_target`, `brim_ears_json` (Task 8).
- Produces: the MCP tool `set_brim_ears`.

- [ ] **Step 1: Register the tool**

Add to `register_paint_tools()` in `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp`, after the
`clear_object_paint` registration:

```cpp
    register_tool({
        "set_brim_ears",
        "Place brim ears on an object -- the small tabs the brim adds at chosen points. Brim ears "
        "are NOT facet paint: they are points on the object (ModelObject::brim_points), so they "
        "have their own tool. Positions are PLATE millimetres, the same frame get_object_info's "
        "bounding_box uses; only x and y matter, because an ear always sits on the bottom of the "
        "object. Pass an empty points array to remove them all. They only produce brim unless "
        "brim_type is 'painted'.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
                {"instance_id", {
                    {"type", "integer"},
                    {"description", "Which instance's transform reads your coordinates (default 0)"}
                }},
                {"points", {
                    {"type", "array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"x", {{"type", "number"}}},
                            {"y", {{"type", "number"}}},
                            {"radius", {{"type", "number"}, {"description", "Ear radius in mm, 0.1 to 100"}}}
                        }},
                        {"required", {"x", "y"}}
                    }},
                    {"description", "Ear positions in plate mm. An empty array removes every ear."}
                }},
                {"radius", {
                    {"type", "number"},
                    {"description", "Default radius for points that do not carry one (default 5.0 mm)"}
                }},
                {"append", {
                    {"type", "boolean"},
                    {"description", "false (default) replaces the object's ears; true adds to them"}
                }}
            }},
            {"required", {"object_id", "points"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([params]() -> nlohmann::json {
                Plater*     plater = wxGetApp().plater();
                PaintTarget target;
                std::string error;
                if (!resolve_paint_target(params, target, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                if (!params["points"].is_array())
                    return nlohmann::json{{"status", "error"},
                                          {"message", "points must be an array of {x, y, radius?}"}};

                // The gizmo's own bounds (GLGizmoBrimEars.cpp:18-19). Its default radius is derived
                // from the initial layer line width; 5.0 mm is a printable stand-in for a tool that
                // has no nozzle context of its own to lean on and states its default in the schema.
                constexpr double k_radius_min = 0.1;
                constexpr double k_radius_max = 100.0;
                const double default_radius = params.value("radius", 5.0);

                const Slic3r::Transform3d instance_matrix =
                    target.object->instances.empty()
                        ? Slic3r::Transform3d::Identity()
                        : target.object->instances[target.instance_idx]->get_matrix();
                const Slic3r::Transform3d to_object = instance_matrix.inverse();

                Slic3r::BrimPoints points;
                for (const nlohmann::json& entry : params["points"]) {
                    if (!entry.contains("x") || !entry.contains("y"))
                        return nlohmann::json{{"status", "error"},
                                              {"message", "every point needs x and y in plate millimetres"}};
                    const double radius = entry.value("radius", default_radius);
                    if (radius < k_radius_min || radius > k_radius_max)
                        return nlohmann::json{{"status", "error"},
                                              {"message", "radius " + std::to_string(radius) + " out of range " +
                                                          std::to_string(k_radius_min) + ".." +
                                                          std::to_string(k_radius_max)}};
                    // An ear always sits on the underside: the gizmo pins world z to -0.0001 and
                    // converts to object-local (GLGizmoBrimEars.cpp:395-397), and Brim.cpp:373-374
                    // skips any stored point whose world z is above 0.
                    const Slic3r::Vec3d world(entry["x"].get<double>(), entry["y"].get<double>(), -0.0001);
                    const Slic3r::Vec3d local = to_object * world;
                    points.emplace_back(local.cast<float>(), float(radius));
                }

                const bool append = params.value("append", false);
                plater->take_snapshot(_u8L("Set Brim Ears"));
                if (!append)
                    target.object->brim_points.clear();
                target.object->brim_points.insert(target.object->brim_points.end(), points.begin(), points.end());

                plater->set_plater_dirty(true);
                wxGetApp().obj_list()->update_info_items(std::size_t(target.object_id));
                plater->update();

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", target.object_id},
                    {"object_name", target.object->name},
                    {"coordinate_frame", "plate"},
                    {"instance_id", int(target.instance_idx)},
                    {"brim_ear_count", int(target.object->brim_points.size())},
                    {"brim_ears", brim_ears_json(*target.object, target.instance_idx)},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Painted ears are only produced when brim_type is btPainted (Brim.cpp:449 and
                // Brim.cpp:530), so say so rather than let the caller slice and find no brim.
                const Slic3r::DynamicPrintConfig& global =
                    wxGetApp().preset_bundle->prints.get_edited_preset().config;
                const Slic3r::DynamicPrintConfig& object_cfg = target.object->config.get();
                const Slic3r::BrimType brim_type =
                    object_cfg.option("brim_type") ? object_cfg.opt_enum<Slic3r::BrimType>("brim_type")
                                                   : global.opt_enum<Slic3r::BrimType>("brim_type");
                if (brim_type != Slic3r::BrimType::btPainted && !target.object->brim_points.empty())
                    result["info_messages"] = std::vector<std::string>{
                        "Brim ears are placed but will not be printed while brim_type is not 'painted'. "
                        "Set brim_type to 'painted' with apply_config or set_object_config."};

                return result;
            });
        }
    });
```

- [ ] **Step 2: Build**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8`
Expected: build succeeds.

- [ ] **Step 3: Confirm the registration count**

Run: `grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'`
Expected: `74`

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
git commit -m "$(cat <<'EOF'
feat: an agent can now place brim ears, the fifth gizmo the audit grouped with painting

The toolbar audit listed BrimEars beside the four painting gizmos, and it is the one that
does not share their mechanism: brim ears are BrimPoints on the ModelObject
(Model.hpp:390), not per-triangle annotations, which is why they get their own tool
rather than a fifth paint mode.

They are also the one place this feature stores a converted coordinate. brim_points are
object-local -- Brim.cpp:373 transforms each by the instance matrix and discards any whose
world z is above 0 -- so the tool takes plate x/y like everything else and stores
instance.inverse() * (x, y, -0.0001), which is what GLGizmoBrimEars.cpp:395-397 does when
a user clicks. Storing the caller's plate coordinates raw would have put every ear at the
wrong place on any object not sitting at the origin, and would have looked correct in the
response.

Ears only produce brim when brim_type is 'painted' (Brim.cpp:449), so the response says
so instead of letting the caller slice and find no brim.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 12: Documentation and the in-app tool catalog

**Files:**
- Modify: `docs/tools/reference.md:3` (the stale tool count), `:5-25` (Quick Reference Table),
  and insert a new section after the `clear_adaptive_layer_height` entry (currently ending at
  line 979, immediately before `## Printer Tools`)
- Modify: `CLAUDE.md` (the `### MCP Tools (70 registered, 71 reachable)` heading, the sentence
  under it, and the category table)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` (`get_server_info`'s `tools_by_category`,
  after the `per_object_settings` entry)

**Interfaces:**
- Consumes: the four tools registered in Tasks 8-11.
- Produces: no code interface. This is the Global Constraints' docs requirement discharged.

- [ ] **Step 1: Add the reference.md section**

In `docs/tools/reference.md`, replace line 3:

```markdown
Complete reference for all 50 MCP tools available in OrcaMCP.
```

with:

```markdown
Complete reference for the MCP tools available in OrcaMCP. The authoritative tool count and
the command that regenerates it live in `CLAUDE.md`; this file's Quick Reference Table is not
yet exhaustive (the filament and printer-control families are documented in
`docs/printers/` instead).
```

Add a row to the Quick Reference Table, after the `**Adaptive**` row:

```markdown
| **Painting** | `paint_object`, `get_object_paint`, `clear_object_paint`, `set_brim_ears` |
```

Insert this section immediately before `## Printer Tools`:

```markdown
## Painting Tools

All four tools take and report **plate millimetres** — the same coordinate frame
`get_object_info` reports its `bounding_box` and `position` in. They are never object-local.
A facet belongs to the band or region containing its **centroid**, so a triangle is painted
whole or not at all.

Paint is stored on the *volume*, so it applies to every instance of an object. `instance_id`
(default `0`) only decides which instance's transform your coordinates are read through; it
does not restrict which instances are painted.

### paint_object
Write per-triangle paint — the same data the GUI paint gizmos write.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `selection` | string | Yes | `bands`, `box`, `sphere` or `all` |
| `mode` | string | No | `color` (default), `support`, `seam`, `fuzzy_skin` |
| `volume_id` | integer | No | Part index (0-based); omit or `-1` for every model part |
| `instance_id` | integer | No | Whose transform reads your coordinates (default 0) |
| `axis` | string | `bands` | `x`, `y` or `z` — the plate axis the bands run along |
| `filaments` | array | `bands` + `color` | One 1-based slot per band, split evenly. `0` = unpainted |
| `bands` | array | `bands` | Explicit `[{from, to, filament\|state}]` in plate mm |
| `from` / `to` | number | No | Even-split range; defaults to the painted volumes' own extent |
| `box` | object | `box` | `{min: [x,y,z], max: [x,y,z]}` in plate mm |
| `sphere` | object | `sphere` | `{center: [x,y,z], radius: n}` in plate mm |
| `filament` | integer | `box`/`sphere`/`all` + `color` | 1-based slot; `0` = unpainted |
| `state` | string | `box`/`sphere`/`all`, non-color | `none`, `enforcer`, `blocker` |
| `replace` | boolean | No | `true` (default) discards this mode's existing paint first |

**Notes:**
- An even split (`filaments`) is colour-only. The other three modes take explicit `bands`
  with a `state`, because alternating enforcer and blocker along an axis means nothing.
- `fuzzy_skin` has no `blocker`: `EnforcerBlockerType::FUZZY_SKIN` is an alias of `ENFORCER`.
- Explicit `bands` need not tile the object and may overlap; the first match wins, and
  facets outside every band keep their previous state.
- Painted supports need `enable_support: true`; painted fuzzy skin needs `fuzzy_skin` set to
  something other than `disabled_fuzzy` (the default). The response says so in
  `info_messages` when they are not.
- Filament slots above 32 cannot be painted — a facet state stops at
  `EnforcerBlockerType::ExtruderMax`. Use `set_object_filament` for those.

**Example — 14 even bands along Y across mixed slots 5-18:**
```json
{"object_id": 0, "selection": "bands", "axis": "y",
 "filaments": [5,6,7,8,9,10,11,12,13,14,15,16,17,18]}
```

**Example — support enforcers under a Z height:**
```json
{"object_id": 0, "mode": "support", "selection": "bands", "axis": "z",
 "bands": [{"from": 0, "to": 12, "state": "enforcer"}]}
```

**Response includes:** `mode`, `selection`, `coordinate_frame` (always `"plate"`),
`instance_id`, `axis_range`, per-band `facet_count`, `facets_painted`,
`facets_unassigned`, per-volume `painted` totals, `info_messages`, `active_warnings`.

---

### get_object_paint
Read back what is painted, plus the object's brim ears.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `volume_id` | integer | No | Part index (0-based); omit or `-1` for every part |
| `instance_id` | integer | No | Whose transform reports plate coordinates (default 0) |
| `mode` | string | No | Report one mode; omit for all four |

**Response includes:** per volume, `facets_total`, a plate-frame `bounding_box`, and per mode
a list of `{state, label, filament, facet_count, coverage_percent}`. `coverage_percent` is
area-weighted, not facet-count-weighted, because a facet an earlier gizmo stroke subdivided
would otherwise count the same as a whole face. State `0` (unpainted) is included.

---

### clear_object_paint
Reset an annotation, the equivalent of the gizmo's "Remove all".

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `volume_id` | integer | No | Part index (0-based); omit or `-1` for every part |
| `mode` | string | No | Which annotation; omit to clear all four |

This resets the annotation, which is **not** the same as painting every facet with state
`none`: a reset leaves no data for the 3MF to carry, while painting `none` leaves a full
bitstream of zeroes.

---

### set_brim_ears
Place the small brim tabs at chosen points. Brim ears are **not** facet paint — they are
`BrimPoints` on the `ModelObject`, which is why they are a separate tool.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `points` | array | Yes | `[{x, y, radius?}]` in plate mm. An empty array removes every ear |
| `radius` | number | No | Default ear radius for points without one (default 5.0 mm, range 0.1-100) |
| `append` | boolean | No | `false` (default) replaces the object's ears; `true` adds to them |
| `instance_id` | integer | No | Whose transform reads your coordinates (default 0) |

Only `x` and `y` matter: an ear always sits on the underside of the object. Ears produce
brim only when `brim_type` is `painted`; the response says so in `info_messages` when it
is not.

---
```

- [ ] **Step 2: Update CLAUDE.md**

Change the heading and the sentence under it:

```markdown
### MCP Tools (74 registered, 75 reachable)

The server registers 74; the bridge adds `start_orca`, which launches OrcaSlicer and
so cannot live inside it. To regenerate this count after adding a tool:
```

Add a row to the category table, immediately after the `**Filaments & colour**` row:

```markdown
| **Painting** | `paint_object`, `get_object_paint`, `clear_object_paint`, `set_brim_ears` |
```

- [ ] **Step 3: Add the tools to the in-app catalog**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp`, inside `get_server_info`'s `tools_by_category`
object, add a new entry after the `per_object_settings` entry:

```cpp
                    {"painting", {
                        {"paint_object", "Paint per-triangle annotations: mode=color (multi-material), "
                                         "support, seam or fuzzy_skin. selection=bands along a plate axis "
                                         "(even split over `filaments`, or explicit `bands`), box, sphere, "
                                         "or all. ALL COORDINATES ARE PLATE MM, same frame as "
                                         "get_object_info's bounding_box."},
                        {"get_object_paint", "Read back what is painted per volume and per mode, plus brim ears"},
                        {"clear_object_paint", "Reset one paint annotation, or all four, back to unpainted"},
                        {"set_brim_ears", "Place brim ears at plate x/y points. Not facet paint - these are "
                                          "points on the object, and need brim_type='painted' to print."}
                    }},
```

- [ ] **Step 4: Verify the count command agrees with the docs**

Run: `grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'`
Expected: `74`, matching the number now written in `CLAUDE.md`.

- [ ] **Step 5: Build and run the suite**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: build succeeds; 0 failures.

- [ ] **Step 6: Commit**

```bash
git add docs/tools/reference.md CLAUDE.md src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp
git commit -m "$(cat <<'EOF'
docs: documented the painting tools, and stopped reference.md claiming a count it never had

The four new tools are now in reference.md, in CLAUDE.md's table, and in the catalog
get_server_info serves to an agent that has no other documentation to read. Each of the
three says the same thing about coordinates -- plate millimetres, the frame
get_object_info already reports -- because an agent that reads only one of them still has
to get that right.

reference.md's opening line claimed "all 50 MCP tools" while the server registered 70.
Correcting the number to 74 would have been worse, not better: the file is missing the
filament and printer-control families entirely, so a precise count above an incomplete
list reads as a promise the page does not keep. The line now points at CLAUDE.md's count
command, which is the one that regenerates.

Left alone deliberately: the per-tool sections for the filament and printer families that
reference.md still does not carry. Adding them is a documentation sweep of its own and
belongs with whoever next audits that file, not smuggled into a painting change.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 13: Acceptance — the scraper in 14 bands, end to end

This is the **only** task that starts the application. It drives the running slicer, so run it
with the user present. It must not send anything to a printer: `send_to_printer`,
`printer_control` and `print_printer_file` are out of scope here.

Per `CLAUDE.md`'s testing guidelines, every check below uses the `mcp__orca-slicer__*` tools,
never `curl` — direct HTTP bypasses the bridge and does not exercise the integration.

**Files:**
- No source changes. If a step fails, fix the cause in the owning task's file and re-run
  this task from Step 1.

**Preconditions:**
- `/Users/hanan/Downloads/printbed_scraper_hanging_hole.stl` exists (40 x 122 x 6 mm, flat,
  length along Y).
- A multi-filament printer profile is selected — the Flashforge Creator 5 Pro (`C5P`) profile
  the 2026-09-10 session used. `set_mixed_filament` needs one.

**Interfaces:**
- Consumes: `paint_object`, `get_object_paint`, `clear_object_paint`, `set_brim_ears` from
  Tasks 8-11.
- Produces: the evidence that T5's original request now works.

- [ ] **Step 1: Put the new build where the bridge looks for it, and restart the app**

The bridge searches `build/arm64/src/Release/OrcaSlicer.app` (`scripts/orcamcp-bridge.py:88`),
but the batch's build command produces `RelWithDebInfo`. Copy it across, and make sure no old
instance is running:

```bash
cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8
pkill -x OrcaSlicer || true
rm -rf build/arm64/src/Release/OrcaSlicer.app
cp -R build/arm64/src/RelWithDebInfo/OrcaSlicer.app build/arm64/src/Release/OrcaSlicer.app
```

Then start it with the MCP tool: `mcp__orca-slicer__start_orca` with `{}`.

Expected: the tool reports the app started. Follow with `mcp__orca-slicer__get_server_info`
`{}` and confirm `tools_by_category` now contains a `painting` entry — that is proof the
running binary is the one just built, not a stale instance.

- [ ] **Step 2: Load the scraper and confirm its plate extents**

```
mcp__orca-slicer__new_project   {}
mcp__orca-slicer__load_model    {"file_path": "/Users/hanan/Downloads/printbed_scraper_hanging_hole.stl"}
mcp__orca-slicer__get_object_info {"object_id": 0}
```

Expected: `bounding_box.size_x` ≈ 40, `size_y` ≈ 122, `size_z` ≈ 6 — the length is along Y,
which is the whole reason `cut_object` and `set_object_layer_range` could not do this job.
**Write down `bounding_box.min.y` and `max.y`**; every later check compares against them.

- [ ] **Step 3: Make sure there are 18 filament slots**

```
mcp__orca-slicer__get_filaments {}
```

If the response lists fewer than 18 slots, create mixed slots from physical slots 1 and 2
until there are 18, one call each, using these ratios in order so the bands render as a
visible gradient rather than 14 identical colours:

```
95/5, 88/12, 81/19, 74/26, 67/33, 60/40, 53/47, 46/54, 39/61, 32/68, 25/75, 18/82, 11/89, 4/96
```

Each call is:

```
mcp__orca-slicer__set_mixed_filament {"components": [1, 2], "ratios": [95, 5]}
```

Expected: `get_filaments` finally reports at least 18 slots, with slots 5-18 marked as mixed.

- [ ] **Step 4: Paint the 14 bands — the request that had no tool**

```
mcp__orca-slicer__paint_object {"object_id": 0, "selection": "bands", "axis": "y",
                                "filaments": [5,6,7,8,9,10,11,12,13,14,15,16,17,18]}
```

Expected, all four in the same response:
- `"status": "success"` and `"coordinate_frame": "plate"`.
- `axis_range.from` and `axis_range.to` equal the `min.y` / `max.y` recorded in Step 2.
- `facets_unassigned` is `0`. Anything else means facets fell through a band boundary.
- `bands` has 14 entries, filaments 5 through 18 in Y order, each with a non-zero
  `facet_count`.

- [ ] **Step 5: Read the paint back**

```
mcp__orca-slicer__get_object_paint {"object_id": 0, "mode": "color"}
```

Expected: one volume, and its `modes.color` lists 14 entries with `filament` 5 through 18.
`coverage_percent` is roughly 100/14 ≈ 7.1 for each; the two end bands may differ slightly
because the scraper is not a uniform prism. State `0` should be absent or near zero.

- [ ] **Step 6: Look at it**

```
mcp__orca-slicer__render_plate_view {"plate_index": 0, "save_to_file": true,
                                     "views": [{"camera_position": [155, -150, 220], "target": [155, 155, 3]}]}
```

Read the returned image path with the Read tool. Expected: 14 distinct stripes running across
the scraper's length, ordered from the scraping edge to the hanging hole. This is the picture
the user asked for on 2026-09-10 and could not get.

- [ ] **Step 7: Prove the paint reaches the slicer**

```
mcp__orca-slicer__slice_all         {}
mcp__orca-slicer__get_slicing_status {}          # poll every 2-3 s until idle
mcp__orca-slicer__get_print_estimate {}
```

Expected: slicing completes, and `get_print_estimate` reports per-filament usage for the
painted slots — not a single filament. If only one filament appears, the annotation is not
reaching `MultiMaterialSegmentation` and the write path in Task 7 is wrong.

- [ ] **Step 8: Prove the paint survives a 3MF round trip**

```
mcp__orca-slicer__export_3mf  {"output_path": "/tmp/scraper_painted.3mf"}
mcp__orca-slicer__new_project {}
mcp__orca-slicer__load_project {"file_path": "/tmp/scraper_painted.3mf"}
mcp__orca-slicer__get_object_paint {"object_id": 0, "mode": "color"}
```

Expected: the same 14 states with the same filament numbers as Step 5. This is what proves
the MCP write produced gizmo-shaped data rather than something only this code can read.

- [ ] **Step 9: Check undo, clear and brim ears**

```
mcp__orca-slicer__paint_object       {"object_id": 0, "selection": "box", "filament": 3,
                                      "box": {"min": [0, 0, 0], "max": [400, 400, 400]}}
mcp__orca-slicer__undo               {}
mcp__orca-slicer__get_object_paint   {"object_id": 0, "mode": "color"}
```
Expected: after the undo, the 14 bands are back — the snapshot in `paint_object` works.

```
mcp__orca-slicer__clear_object_paint {"object_id": 0, "mode": "color"}
mcp__orca-slicer__get_object_paint   {"object_id": 0, "mode": "color"}
```
Expected: `modes.color` reports a single entry at state `0` covering 100%.

```
mcp__orca-slicer__set_brim_ears {"object_id": 0, "points": [{"x": <min.x + 5>, "y": <min.y + 5>},
                                                            {"x": <max.x - 5>, "y": <max.y - 5>}]}
mcp__orca-slicer__get_object_paint {"object_id": 0}
```
(substituting the bounding-box numbers from Step 2). Expected: `brim_ear_count` is 2, and the
`brim_ears` read back from `get_object_paint` report the same plate x/y that were sent —
within a rounding tolerance, since they are stored as object-local floats. An
`info_messages` line about `brim_type` is expected unless the profile already uses `painted`.

- [ ] **Step 10: Report, and do not commit**

Nothing in this task changes a tracked file. Report to the user:
- the 14-band render from Step 6,
- the per-filament usage from Step 7,
- whether the 3MF round trip in Step 8 matched,
- any step that did not match its expectation, with the response that did not match.

---

## Notes for whoever executes this

**Two things this plan deliberately does not do.**

1. **No seed fill, no angle-based selection, no brush.** The gizmos also select by surface
   feature (`seed_fill_select_triangles`, `bucket_fill_select_triangles`,
   `TriangleSelector.hpp:331-345`) and subdivide triangles under a circular cursor. Both need
   a hit point on the mesh, which is a mouse concept; bands and regions are specifiable in
   numbers an agent already has from `get_object_info`. Adding them would also mean writing
   split triangles, which is a much larger correctness surface.
2. **No per-band colour suggestion.** `suggest_color_mix` and `get_color_palette` already
   produce slots; `paint_object` consumes slot numbers. Wiring them together belongs in a
   workflow doc, not in the tool.

**One thing that could not be turned into a task.** `docs/tools/workflows.md` would be the
natural home for a "propose a palette, create the mixed slots, paint the bands, verify"
recipe, but the Global Constraints only require `reference.md` and `CLAUDE.md`, and the
acceptance run in Task 13 is the sequence that recipe would describe. If the user wants it
written up, it is a follow-up of about one commit.
