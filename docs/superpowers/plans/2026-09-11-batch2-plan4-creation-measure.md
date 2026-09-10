# Batch 2 Plan 4 — Creation and measurement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four MCP tools — `measure_object`, `get_fonts`, `emboss_text`, `emboss_svg` — so an agent can read a model's real dimensions and features, discover the fonts this machine actually has, and put text or an SVG onto an object without touching the mouse.

**Architecture:** `measure_object` drives `Measure::Measuring` (`src/libslic3r/Measure.hpp:110`) directly on the volume's `indexed_triangle_set`, enumerating planes and their edges/circles the same way `GLGizmoMeasure` does, and reporting them with stable ids so a second call can measure between two of them. The emboss tools bypass the gizmo's job/worker/raycast machinery entirely: they build an `EmbossShape` (glyph outlines from a font, or paths from nanosvg), turn it into a `TriangleMesh` with the same `polygons2model` call `EmbossJob.cpp` uses, and attach it as a `ModelVolume` with a transform computed from one of the six faces of the object's bounding box. Everything that can be computed without wxWidgets lives in `OrcaMCPMeasureUtils` / `OrcaMCPEmbossShape` and is covered by Catch2; only the font lookup needs wx.

**Tech Stack:** C++17, libslic3r (`Measure`, `Emboss`, `NSVGUtils`, `Model`), wxWidgets (`wxFontEnumerator`, `WxFontUtils`), nlohmann/json, Catch2 v3.

**Spec:** docs/superpowers/plans/2026-09-10-batch-2-spec.md

## Global Constraints

Copied from the spec's Global Constraints section. Every task inherits these.

- **Branch:** `sync-upstream-2.5`. Do not push. Do not create branches.
- **Commit style:** `fix:` / `feat:` + what changed, past tense. The body names the **root cause**, not the symptom, and lists related occurrences checked — *including the ones deliberately left alone, and why*. Read `git log -5` for the bar. Every commit ends with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
- **Threading:** every tool handler body runs inside `run_on_main_thread()` (`OrcaMCPCommon.hpp`). Anything touching `Model`, `Plater` or the preset bundle must be inside it. A modal opened inside it hangs the GUI forever.
- **Dialogs:** never call `set_mcp_dialog_suppression()` directly. `McpDialogSuppressionGuard` (RAII, nest-safe) is the only sanctioned way.
- **Pure logic goes in a free function with Catch2 coverage** under `tests/slic3rutils/`. This project tests logic without a printer wherever possible; geometry maths must be testable without a GUI.
- **Undo/redo:** any tool that mutates the model must take a snapshot so `undo` works, the way the existing transform tools do. Check how `move_object` does it and follow it.
- **Registration:** tools are registered with `register_tool({...})` in `register_builtin_tools()` (`OrcaMCPServer.cpp`) or the per-area registrars (`OrcaMCPFilamentTools.cpp`, `OrcaMCPPrinterTools.cpp`). New tool groups get their own file following that pattern, added to `src/slic3r/CMakeLists.txt`.
- **Docs:** every new or changed tool updates `docs/tools/reference.md`, and the tool table plus the count in `CLAUDE.md`. The count command is in CLAUDE.md.
- **Response shape:** `{"status": "success"|"error"|"partial", ...}` plus `active_warnings` on anything that mutates the scene, matching existing tools.
- **Verify before reporting:**
  ```
  cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
  build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
  ```
  Baseline at the time of writing: **174 cases / 1195 assertions / 0 failures**. The skipped count varies run to run (bundled-Python and numpy cases) — that is expected, not a regression.
- **Do not start the app or contact the printer** unless the plan says to.

### Plan-specific note on `move_object` and snapshots

The spec says to follow `move_object` for undo. Read it before you copy it: `move_object` (`src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:3079`) does **not** take a snapshot — that is a separate defect, not the pattern to imitate. The tool in this codebase that does it correctly is `set_object_printable` (`OrcaMCPServer.cpp:4076-4080`):

```cpp
std::string snapshot_text = (boost::format("%1% \"%2%\"") % "Set Object Printable" % object->name).str();
plater->take_snapshot(snapshot_text);
```

`emboss_text` and `emboss_svg` follow **that**. `measure_object` is read-only and takes no snapshot. Do not fix `move_object` here; it belongs to a different plan.

---

## Scope

### `measure_object` — in scope

- Address an object by `object_id` (0-based index into `Model::objects`, the convention every other tool uses), optionally narrowed to one `volume_id`.
- **Dimensions:** the snug world-space bounding box from `ModelObject::instance_bounding_box(0)` (`Model.hpp:473`) and the untransformed mesh box from `ModelObject::raw_mesh_bounding_box()` (`Model.hpp:477`).
- **Features:** every plane the mesh has (`Measuring::get_num_of_planes()`, `Measure.hpp:122`) and the edges / circles / plane record inside each (`Measuring::get_plane_features()`, `Measure.hpp:128`), each with a stable id.
- **Measurement:** distance and angle between any two of those ids, via `Measure::get_measurement` (`Measure.hpp:174`).
- A cap on how many features come back, with the true total reported — the same lesson as T2.

### `measure_object` — out of scope, and why

- **Feature picking by cursor.** `Measuring::get_feature(face_idx, point, world_tran, only_select_plane)` (`Measure.hpp:119`) is the gizmo's hover path; it needs a screen ray. Plane enumeration gives an agent the same features without one.
- **Assembly actions.** `Measure::get_assembly_action` (`Measure.hpp:191`) belongs to the Assembly gizmo, which the spec puts out of scope for this batch.

### `emboss_text` — in scope

- **The text:** a `text` string, `\n` for extra lines.
- **The font:** an exact installed face name in `font`, or omitted to use the OS default face. `bold` and `italic` flags. A name that is not installed is a **hard error naming the parameter** — never a silent substitution (see "Fonts are the risk" below).
- **Size and depth:** `size_mm` → `FontProp::size_in_mm` (`TextConfiguration.hpp:61`); `depth_mm` → `EmbossProjection::depth` (`EmbossShape.hpp:19`).
- **Spacing and alignment:** `char_gap`, `line_gap` (`TextConfiguration.hpp:23,27`), and `align_h` / `align_v` (`TextConfiguration.hpp:52`).
- **Volume type:** `part` → `MODEL_PART` (raised), `negative` → `NEGATIVE_VOLUME` (engraved), `modifier` → `PARAMETER_MODIFIER` (`Model.hpp:343-350`).
- **Placement:** **axis-aligned only.** One of the six faces of the object's raw-mesh bounding box (`top`, `bottom`, `front`, `back`, `left`, `right`), plus `offset_u` / `offset_v` in mm from that face's centre and `rotation_deg` in the plane of the face.
- The created volume carries `text_configuration` and `emboss_shape` (`Model.hpp:900,904`), so the GUI's Emboss gizmo can select and edit it afterwards.

### `emboss_text` — out of scope, stated plainly

Each of these is a deliberate cut. An honest narrow tool that works beats a broad one that does not.

| Cut | Why |
|---|---|
| **Arbitrary placement on a non-axis-aligned face** | The gizmo gets its position from a screen ray through `RaycastManager` and `Emboss::create_transformation_onto_surface` (`Emboss.hpp:330`). There is no cursor in an MCP call, and inventing one ("nearest point to a 3-D coordinate") is a second raycasting problem. Six bbox faces cover a name plate, a label, a logo on a lid. |
| **`use_surface`** (shape cut from the parent's surface, `EmbossShape.hpp:23`) | Needs `CreateSurfaceVolumeJob` / `cut_surface` and a full copy of the parent's sliced sources. Large, and it only matters on curved surfaces, which axis-aligned placement does not target anyway. `use_surface` is left `false`. |
| **`per_glyph` / text on a curve** (`FontProp` per-glyph flag, `Emboss::TextLines`) | Needs `TextLinesModel::init` against the live `Selection` and a slice of the parent per line. |
| **Editing an existing text volume** | `GLGizmoEmboss::re_emboss` (`GLGizmoEmboss.hpp:67`) exists but drives `start_update_volume` through the plater worker. `emboss_text` creates; to change text, delete the volume and call again. |
| **A standalone text object with no parent** | `CreateObjectJob` also handles bed placement and arrangement. `emboss_text` requires `object_id`. |
| **Font by file path, font collections, synthetic boldness/skew** | `EmbossStyle::Type::file_path` (`TextConfiguration.hpp:135`), `collection_number`, `boldness` and `skew` are style-manager territory. wx weight/style flags cover bold and italic. |
| **Saved emboss styles** | `StyleManager` persists styles in AppConfig. Every MCP call is self-contained. |

### `emboss_svg` — in scope, and what it shares

`emboss_svg` is `emboss_text` with a different front end. Shared, and therefore **not re-implemented** in its tasks:

- `EmbossShape` → `TriangleMesh` (Task 8, `emboss_shape_to_mesh`).
- Face placement maths (Task 7, `emboss_face_transform`).
- Volume type mapping, snapshot, attach, response shape (Task 9, `attach_emboss_volume`).

Its own part is one function: `build_svg_shape` (Task 8), which parses the file with `nsvgParse` and converts paths to `ExPolygonsWithIds` with `create_shape_with_ids` (`NSVGUtils.hpp:58`) — mirroring `select_shape` in `GLGizmoSVG.cpp:2206`, but **without** its `show_error` calls, which open modals.

### `emboss_svg` — out of scope

- **One volume per SVG path.** The gizmo can split an SVG so each path takes its own filament. One volume for the whole file here.
- **`use_surface`**, same reason as text.
- **Storing the SVG into the 3MF's private area** (`EmbossShape::SvgFile::path_in_3mf`, `EmbossShape.hpp:91`). We set `path` and `file_data`, which is enough to reload and to serialise; the "delete private data on save" dialog path is not driven.

### Beyond the spec's three tools: `get_fonts`

The spec lists three tools. This plan adds a fourth, `get_fonts`, because the spec's own font requirement cannot be met without it: if a missing font must be "a clear error rather than a crash or a silent substitution", the agent needs a way to find out what *is* installed. Stuffing several hundred face names into every error response is not that way. `get_fonts` is one `wxFontEnumerator::GetFacenames` call with a `name_contains` filter and a limit. Tool count goes 70 → 74.

### Fonts are the risk — what the code actually does

Read before writing Task 6.

- `Emboss::get_font_list()` (`Emboss.hpp:30`) is **Windows-only**. On macOS and Linux it is the stub at `Emboss.cpp:1026` returning `{}`. So is `Emboss::get_font_path` (`Emboss.cpp:1031`). **Do not use either.**
- The cross-platform enumeration the gizmo actually uses is `wxFontEnumerator::GetFacenames(wxFONTENCODING_SYSTEM)` (`GLGizmoEmboss.cpp:989`, `:3552`), and the cross-platform validity check is `wxFontEnumerator::IsValidFacename(facename)` (`GLGizmoEmboss.cpp:1681`).
- Turning a `wxFont` into glyph outlines is `WxFontUtils::create_font_file(const wxFont&)` (`WxFontUtils.hpp:26`), which branches per platform (`WxFontUtils.cpp:84-105`): Windows via `HFONT`, macOS via `CTFontDescriptorCopyAttribute(kCTFontURLAttribute)` → a file path → `Emboss::create_font_file(path)`, Linux via fontconfig (`FontConfigHelp.hpp`). It returns `nullptr` on failure.
- **The silent-substitution trap.** `wxFont::SetFaceName` / `wxFontInfo().FaceName(...)` on a name the OS does not have still yields an `IsOk()` font — the system substitutes. The text then renders in a font the caller never asked for. So the order in `resolve_font` is: `IsValidFacename` first (missing → error), only then construct the `wxFont`.
- **The second trap, macOS-specific.** A face can be installed and still unusable: `WxFontUtils.cpp:19-42`'s `is_valid_ttf` rejects `.dfont` files (Courier, Geneva, Monaco), so `create_font_file` returns `nullptr` for them. That is a different error from "not installed" and must say so.

---

## File Structure

**Create:**

| File | Responsibility |
|---|---|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp` / `.cpp` | Everything `measure_object` computes: feature ids, feature/measurement JSON, plane enumeration, and the `world_plane_features` fix-up that keeps `Measure::get_measurement` from dereferencing null. No wx, no `Plater`. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp` / `.cpp` | Everything emboss can do without wx: face parsing, the placement transform, `EmbossShape` → `TriangleMesh`, SVG → `EmbossShape`, text → `EmbossShape` given a loaded font, and font-name filtering. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossFont.hpp` / `.cpp` | The only wx-dependent part: enumerating installed faces and resolving a face name to a loaded `FontFile`. Thin on purpose. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp` | `OrcaMCPServer::register_creation_tools()` — the four `register_tool({...})` blocks — plus `attach_emboss_volume`, which is the only function here that mutates the `Model`. |
| `tests/slic3rutils/test_measure_utils.cpp` | Catch2 for `OrcaMCPMeasureUtils`. |
| `tests/slic3rutils/test_emboss_shape.cpp` | Catch2 for `OrcaMCPEmbossShape`. |

**Modify:**

| File | Change |
|---|---|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp:66` | Declare `static void register_creation_tools();` next to `register_filament_tools` / `register_printer_tools`. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:4332-4333` | Call `register_creation_tools();` after `register_printer_tools();`. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:648-810` | Add the four tools to `get_server_info`'s `tools_by_category`. |
| `src/slic3r/CMakeLists.txt:426` | Add the six new source/header files to the `libslic3r_gui` sources. |
| `tests/slic3rutils/CMakeLists.txt:27` | Add the two new test files. |
| `docs/tools/reference.md` | New sections; extend the quick-reference table. |
| `CLAUDE.md` | Tool table rows and the count. |

---

## Task 1: Feature ids and feature JSON

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp`
- Create: `tests/slic3rutils/test_measure_utils.cpp`
- Modify: `src/slic3r/CMakeLists.txt:426`, `tests/slic3rutils/CMakeLists.txt:27`

**Interfaces:**
- Consumes: nothing from earlier tasks. Uses `Slic3r::Measure::SurfaceFeature` and `SurfaceFeatureType` from `libslic3r/Measure.hpp:16,21`.
- Produces:
  - `std::string Slic3r::GUI::OrcaMCP::make_feature_id(int plane, int index)`
  - `bool Slic3r::GUI::OrcaMCP::parse_feature_id(const std::string& id, int& plane, int& index)`
  - `nlohmann::json Slic3r::GUI::OrcaMCP::feature_to_json(const Measure::SurfaceFeature& feature)`

- [ ] **Step 1: Write the failing test**

Create `tests/slic3rutils/test_measure_utils.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp"

// measure_object's reporting layer. Nothing here needs a GUI: a SurfaceFeature is plain geometry,
// and the ids are the handle an agent uses to ask for a measurement in a second call.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

TEST_CASE("Feature ids round-trip", "[orcamcp][measure]")
{
    CHECK(make_feature_id(0, 0) == "p0.0");
    CHECK(make_feature_id(3, 7) == "p3.7");

    int plane = -1, index = -1;
    REQUIRE(parse_feature_id("p3.7", plane, index));
    CHECK(plane == 3);
    CHECK(index == 7);
}

TEST_CASE("Feature ids reject what they did not produce", "[orcamcp][measure]")
{
    int plane = -1, index = -1;
    CHECK_FALSE(parse_feature_id("", plane, index));
    CHECK_FALSE(parse_feature_id("3.7", plane, index));       // no 'p'
    CHECK_FALSE(parse_feature_id("p3", plane, index));        // no index
    CHECK_FALSE(parse_feature_id("p3.", plane, index));       // empty index
    CHECK_FALSE(parse_feature_id("p-1.7", plane, index));     // negative plane
    CHECK_FALSE(parse_feature_id("p3.7x", plane, index));     // trailing junk
}

TEST_CASE("An edge feature serialises its two endpoints", "[orcamcp][measure]")
{
    const Measure::SurfaceFeature edge(Measure::SurfaceFeatureType::Edge,
                                       Vec3d(0., 0., 0.), Vec3d(10., 0., 0.));
    const nlohmann::json j = feature_to_json(edge);

    CHECK(j.at("type") == "edge");
    CHECK_THAT(j.at("start").at("x").get<double>(), WithinAbs(0., 1e-9));
    CHECK_THAT(j.at("end").at("x").get<double>(), WithinAbs(10., 1e-9));
    CHECK_THAT(j.at("length").get<double>(), WithinAbs(10., 1e-9));
}

TEST_CASE("A circle feature serialises centre, radius and normal", "[orcamcp][measure]")
{
    const Measure::SurfaceFeature circle(Measure::SurfaceFeatureType::Circle,
                                         Vec3d(1., 2., 3.), Vec3d(0., 0., 1.), std::nullopt, 4.);
    const nlohmann::json j = feature_to_json(circle);

    CHECK(j.at("type") == "circle");
    CHECK_THAT(j.at("center").at("y").get<double>(), WithinAbs(2., 1e-9));
    CHECK_THAT(j.at("radius").get<double>(), WithinAbs(4., 1e-9));
    CHECK_THAT(j.at("diameter").get<double>(), WithinAbs(8., 1e-9));
    CHECK_THAT(j.at("normal").at("z").get<double>(), WithinAbs(1., 1e-9));
}

TEST_CASE("A plane feature serialises its normal and centroid", "[orcamcp][measure]")
{
    // The plane index lives in `value`, offset by 0.0001 the way Measure.cpp writes it.
    const Measure::SurfaceFeature plane(Measure::SurfaceFeatureType::Plane,
                                        Vec3d(0., 0., 1.), Vec3d(5., 5., 6.), std::nullopt, 2.0001);
    const nlohmann::json j = feature_to_json(plane);

    CHECK(j.at("type") == "plane");
    CHECK(j.at("plane_index") == 2);
    CHECK_THAT(j.at("normal").at("z").get<double>(), WithinAbs(1., 1e-9));
    CHECK_THAT(j.at("center").at("z").get<double>(), WithinAbs(6., 1e-9));
}
```

- [ ] **Step 2: Run test to verify it fails**

Add the file to `tests/slic3rutils/CMakeLists.txt` after the `test_material_mapping.cpp` line:

```cmake
    test_measure_utils.cpp
```

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `'slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp' file not found`.

- [ ] **Step 3: Write the header**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "libslic3r/Measure.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A feature id is "p<plane>.<index>": which plane Measuring found it on, and where it sits in that
// plane's feature list. The pair is stable for a given mesh, which is what lets an agent read the
// features in one call and ask for a distance in the next.
std::string make_feature_id(int plane, int index);
bool        parse_feature_id(const std::string& id, int& plane, int& index);

// One detected feature as JSON. Point/Edge/Circle/Plane each get the fields that mean something for
// their type -- an edge has no radius, a circle has no length.
nlohmann::json feature_to_json(const Measure::SurfaceFeature& feature);

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 4: Write the implementation**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp
#include "OrcaMCPMeasureUtils.hpp"

#include <cctype>
#include <cstdlib>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

nlohmann::json point_to_json(const Vec3d& p)
{
    return {{"x", p.x()}, {"y", p.y()}, {"z", p.z()}};
}

// Non-negative decimal only, whole field, no sign and no trailing characters. strtol would accept
// "3x" and a leading '+', which would let two different strings name the same feature.
bool parse_non_negative(const std::string& text, int& out)
{
    if (text.empty())
        return false;
    for (char c : text)
        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
            return false;
    try {
        out = std::stoi(text);
    } catch (const std::exception&) {
        return false;
    }
    return out >= 0;
}

} // namespace

std::string make_feature_id(int plane, int index)
{
    return "p" + std::to_string(plane) + "." + std::to_string(index);
}

bool parse_feature_id(const std::string& id, int& plane, int& index)
{
    if (id.size() < 4 || id[0] != 'p')
        return false;
    const size_t dot = id.find('.');
    if (dot == std::string::npos)
        return false;
    int parsed_plane = 0;
    int parsed_index = 0;
    if (!parse_non_negative(id.substr(1, dot - 1), parsed_plane))
        return false;
    if (!parse_non_negative(id.substr(dot + 1), parsed_index))
        return false;
    plane = parsed_plane;
    index = parsed_index;
    return true;
}

nlohmann::json feature_to_json(const Measure::SurfaceFeature& feature)
{
    switch (feature.get_type()) {
    case Measure::SurfaceFeatureType::Point:
        return {{"type", "point"}, {"position", point_to_json(feature.get_point())}};
    case Measure::SurfaceFeatureType::Edge: {
        const auto [start, end] = feature.get_edge();
        nlohmann::json j = {
            {"type", "edge"},
            {"start", point_to_json(start)},
            {"end", point_to_json(end)},
            {"length", (end - start).norm()}
        };
        if (feature.get_extra_point().has_value())
            j["center"] = point_to_json(*feature.get_extra_point());
        return j;
    }
    case Measure::SurfaceFeatureType::Circle: {
        const auto [center, radius, normal] = feature.get_circle();
        return {
            {"type", "circle"},
            {"center", point_to_json(center)},
            {"radius", radius},
            {"diameter", radius * 2.},
            {"normal", point_to_json(normal)}
        };
    }
    case Measure::SurfaceFeatureType::Plane: {
        // Measure.cpp writes the plane index into m_value as `plane_idx + 0.0001`; get_plane()
        // truncates it back to an int.
        const auto [plane_index, normal, center] = feature.get_plane();
        return {
            {"type", "plane"},
            {"plane_index", plane_index},
            {"normal", point_to_json(normal)},
            {"center", point_to_json(center)}
        };
    }
    default:
        return {{"type", "undefined"}};
    }
}

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 5: Add the sources to the GUI library**

In `src/slic3r/CMakeLists.txt`, after the line `GUI/OrcaMCP/MCPClientConfig.cpp` (line 426), add:

```cmake
    GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp
    GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp
```

- [ ] **Step 6: Run the tests**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][measure]"
```
Expected: PASS, 5 test cases.

- [ ] **Step 7: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp \
        tests/slic3rutils/test_measure_utils.cpp tests/slic3rutils/CMakeLists.txt src/slic3r/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: measure_object had no way to name a feature across two calls

A measurement needs two features, and an MCP call cannot carry a mouse. Added the
"p<plane>.<index>" id that Measuring's plane enumeration can mint and a later call can
resolve, plus the per-type JSON for a point, edge, circle and plane.

parse_feature_id rejects a leading '+', a sign and trailing characters, so two spellings
can never name the same feature; strtol would have accepted all three.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Measurement result JSON

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp`
- Test: `tests/slic3rutils/test_measure_utils.cpp`

**Interfaces:**
- Consumes: `feature_to_json` from Task 1 (not called here, but lives in the same header).
- Produces: `nlohmann::json Slic3r::GUI::OrcaMCP::measurement_to_json(const Measure::MeasurementResult& result)`

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_measure_utils.cpp`:

```cpp
TEST_CASE("A distance-only measurement reports both distances and the xyz delta", "[orcamcp][measure]")
{
    Measure::MeasurementResult result;
    result.distance_infinite = Measure::DistAndPoints(6.0, Vec3d(0., 0., 0.), Vec3d(0., 0., 6.));
    result.distance_strict   = Measure::DistAndPoints(6.5, Vec3d(0., 0., 0.), Vec3d(1., 1., 6.));
    result.distance_xyz      = Vec3d(1., 1., 6.);

    const nlohmann::json j = measurement_to_json(result);

    CHECK_THAT(j.at("distance").get<double>(), WithinAbs(6.0, 1e-9));
    CHECK_THAT(j.at("distance_strict").get<double>(), WithinAbs(6.5, 1e-9));
    CHECK_THAT(j.at("distance_xyz").at("z").get<double>(), WithinAbs(6.0, 1e-9));
    CHECK_THAT(j.at("from").at("z").get<double>(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(j.at("to").at("z").get<double>(), WithinAbs(6.0, 1e-9));
    CHECK_FALSE(j.contains("angle_degrees"));
}

TEST_CASE("An angle measurement reports degrees", "[orcamcp][measure]")
{
    Measure::MeasurementResult result;
    result.angle = Measure::AngleAndEdges(M_PI / 2., Vec3d::Zero(),
                                          {Vec3d(0., 0., 0.), Vec3d(1., 0., 0.)},
                                          {Vec3d(0., 0., 0.), Vec3d(0., 1., 0.)},
                                          1.0, true);

    const nlohmann::json j = measurement_to_json(result);

    CHECK_THAT(j.at("angle_degrees").get<double>(), WithinAbs(90.0, 1e-9));
    CHECK(j.at("coplanar") == true);
}

TEST_CASE("A measurement with nothing in it says so", "[orcamcp][measure]")
{
    const nlohmann::json j = measurement_to_json(Measure::MeasurementResult());

    CHECK(j.at("has_result") == false);
    CHECK_FALSE(j.contains("distance"));
    CHECK_FALSE(j.contains("angle_degrees"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `use of undeclared identifier 'measurement_to_json'`.

- [ ] **Step 3: Declare it**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp`, after `feature_to_json`:

```cpp
// A Measure::MeasurementResult as JSON. Every field is optional in the source struct, so a field is
// present here only when it was actually computed: `distance` is the infinite-line distance the GUI
// shows first, `distance_strict` the segment-to-segment one, and they differ whenever the closest
// approach falls outside one of the two features.
nlohmann::json measurement_to_json(const Measure::MeasurementResult& result);
```

- [ ] **Step 4: Implement it**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp`, and add `#include "libslic3r/Geometry.hpp"` at the top for `rad2deg`:

```cpp
nlohmann::json measurement_to_json(const Measure::MeasurementResult& result)
{
    nlohmann::json j = {{"has_result", result.has_any_data()}};

    if (result.distance_infinite.has_value()) {
        j["distance"] = result.distance_infinite->dist;
        j["from"]     = point_to_json(result.distance_infinite->from);
        j["to"]       = point_to_json(result.distance_infinite->to);
    }
    if (result.distance_strict.has_value()) {
        j["distance_strict"] = result.distance_strict->dist;
        if (!j.contains("from")) {
            j["from"] = point_to_json(result.distance_strict->from);
            j["to"]   = point_to_json(result.distance_strict->to);
        }
    }
    if (result.distance_xyz.has_value())
        j["distance_xyz"] = point_to_json(*result.distance_xyz);
    if (result.angle.has_value()) {
        j["angle_degrees"] = Geometry::rad2deg(result.angle->angle);
        j["coplanar"]      = result.angle->coplanar;
    }
    return j;
}
```

- [ ] **Step 5: Run the tests**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][measure]"
```
Expected: PASS, 8 test cases.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp \
        tests/slic3rutils/test_measure_utils.cpp
git commit -m "$(cat <<'EOF'
feat: a MeasurementResult had no JSON shape an agent could read

Every field of Measure::MeasurementResult is optional, so serialising it unconditionally
would report a distance of zero for a pair that has no distance. measurement_to_json emits
only the fields that were computed and states has_result outright.

Both distances are reported, not just one: distance_infinite is what the GUI shows first,
distance_strict is the segment-to-segment answer, and for a point past the end of an edge
they are different numbers. Hiding either would make the tool disagree with the gizmo.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Enumerate a mesh's features

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp`
- Test: `tests/slic3rutils/test_measure_utils.cpp`

**Interfaces:**
- Consumes: `make_feature_id` (Task 1).
- Produces:
  - `struct Slic3r::GUI::OrcaMCP::MeasuredFeature { std::string id; int plane; int index; Measure::SurfaceFeature feature; }`
  - `struct Slic3r::GUI::OrcaMCP::MeasuredFeatures { std::vector<MeasuredFeature> features; int plane_count; int total; bool truncated; }`
  - `MeasuredFeatures Slic3r::GUI::OrcaMCP::collect_features(Measure::Measuring& measuring, const Transform3d& to_report_space, int max_features)`
  - `const MeasuredFeature* Slic3r::GUI::OrcaMCP::find_feature(const MeasuredFeatures& features, const std::string& id)`

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_measure_utils.cpp`, and add `#include "libslic3r/TriangleMesh.hpp"` at the top:

```cpp
namespace {

// The print-bed scraper from the testing session: 40 x 122 x 6 mm, lying flat, length along Y.
// its_make_cube puts it in [0,40] x [0,122] x [0,6].
indexed_triangle_set scraper_box() { return its_make_cube(40., 122., 6.); }

const MeasuredFeature* plane_with_normal(const MeasuredFeatures& found, const Vec3d& normal)
{
    for (const MeasuredFeature& f : found.features) {
        if (f.feature.get_type() != Measure::SurfaceFeatureType::Plane)
            continue;
        if (std::get<1>(f.feature.get_plane()).isApprox(normal, 1e-6))
            return &f;
    }
    return nullptr;
}

} // namespace

TEST_CASE("A box yields six planes, each with four edges", "[orcamcp][measure]")
{
    Measure::Measuring measuring(scraper_box());
    const MeasuredFeatures found = collect_features(measuring, Transform3d::Identity(), 1000);

    CHECK(found.plane_count == 6);
    CHECK_FALSE(found.truncated);
    CHECK(found.total == 30); // 6 planes x (4 edges + the plane itself)

    int planes = 0, edges = 0;
    for (const MeasuredFeature& f : found.features) {
        if (f.feature.get_type() == Measure::SurfaceFeatureType::Plane) ++planes;
        if (f.feature.get_type() == Measure::SurfaceFeatureType::Edge)  ++edges;
    }
    CHECK(planes == 6);
    CHECK(edges == 24);
}

TEST_CASE("Every collected feature is findable by its own id", "[orcamcp][measure]")
{
    Measure::Measuring measuring(scraper_box());
    const MeasuredFeatures found = collect_features(measuring, Transform3d::Identity(), 1000);

    REQUIRE_FALSE(found.features.empty());
    for (const MeasuredFeature& f : found.features) {
        const MeasuredFeature* looked_up = find_feature(found, f.id);
        REQUIRE(looked_up != nullptr);
        CHECK(looked_up->plane == f.plane);
        CHECK(looked_up->index == f.index);
    }
    CHECK(find_feature(found, "p99.0") == nullptr);
}

TEST_CASE("The report space transform is applied to every feature", "[orcamcp][measure]")
{
    Measure::Measuring measuring(scraper_box());
    const Transform3d shifted = Transform3d(Eigen::Translation3d(100., 0., 0.));
    const MeasuredFeatures found = collect_features(measuring, shifted, 1000);

    const MeasuredFeature* top = plane_with_normal(found, Vec3d(0., 0., 1.));
    REQUIRE(top != nullptr);
    // Centroid of the top face moves with the object; the normal does not.
    CHECK_THAT(std::get<2>(top->feature.get_plane()).x(), WithinAbs(120., 1e-6));
    CHECK_THAT(std::get<1>(top->feature.get_plane()).z(), WithinAbs(1., 1e-6));
}

TEST_CASE("A feature cap reports the true total", "[orcamcp][measure]")
{
    Measure::Measuring measuring(scraper_box());
    const MeasuredFeatures found = collect_features(measuring, Transform3d::Identity(), 5);

    CHECK(found.features.size() == 5);
    CHECK(found.total == 30);
    CHECK(found.truncated);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `unknown type name 'MeasuredFeatures'`.

- [ ] **Step 3: Declare the types**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp`, before `feature_to_json`:

```cpp
// One feature, already transformed into the space the response reports in.
struct MeasuredFeature
{
    std::string             id;          // make_feature_id(plane, index)
    int                     plane = 0;
    int                     index = 0;
    Measure::SurfaceFeature feature{Measure::SurfaceFeatureType::Undef, Vec3d::Zero(), Vec3d::Zero()};
};

struct MeasuredFeatures
{
    std::vector<MeasuredFeature> features;
    int                          plane_count = 0;
    int                          total       = 0;     // before the cap
    bool                         truncated   = false;
};

// Walk every plane Measuring found and every feature inside it, applying `to_report_space` to each.
// Stops adding after `max_features` but keeps counting, so the caller can say how much it withheld
// -- the same lesson as get_presets (T2): a capped list that lies about the total is worse than a
// long one.
MeasuredFeatures collect_features(Measure::Measuring& measuring,
                                  const Transform3d&  to_report_space,
                                  int                 max_features);

// nullptr when no collected feature carries that id.
const MeasuredFeature* find_feature(const MeasuredFeatures& features, const std::string& id);
```

- [ ] **Step 4: Implement it**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp`:

```cpp
MeasuredFeatures collect_features(Measure::Measuring& measuring,
                                  const Transform3d&  to_report_space,
                                  int                 max_features)
{
    MeasuredFeatures result;
    result.plane_count = measuring.get_num_of_planes();

    for (int plane = 0; plane < result.plane_count; ++plane) {
        // get_plane_features extracts on first use; the cost is per plane, not per call.
        const std::vector<Measure::SurfaceFeature>& plane_features =
            measuring.get_plane_features(static_cast<unsigned int>(plane));

        for (size_t index = 0; index < plane_features.size(); ++index) {
            ++result.total;
            if (static_cast<int>(result.features.size()) >= max_features) {
                result.truncated = true;
                continue;
            }
            MeasuredFeature entry;
            entry.plane   = plane;
            entry.index   = static_cast<int>(index);
            entry.id      = make_feature_id(entry.plane, entry.index);
            entry.feature = plane_features[index];
            entry.feature.translate(to_report_space);
            result.features.push_back(std::move(entry));
        }
    }
    return result;
}

const MeasuredFeature* find_feature(const MeasuredFeatures& features, const std::string& id)
{
    for (const MeasuredFeature& f : features.features)
        if (f.id == id)
            return &f;
    return nullptr;
}
```

- [ ] **Step 5: Run the tests**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][measure]"
```
Expected: PASS, 12 test cases.

If `found.total == 30` fails, print the actual per-plane feature counts and adjust the expectation — `extract_features` merges collinear adjacent edges (`Measure.cpp:478-490`), which for a plain box should leave four per face. Do not weaken `plane_count == 6` or the edge/plane split; those are the load-bearing assertions.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp \
        tests/slic3rutils/test_measure_utils.cpp
git commit -m "$(cat <<'EOF'
feat: nothing could list a mesh's features without a mouse

GLGizmoMeasure discovers a feature by raycasting the cursor onto a facet
(Measuring::get_feature). An MCP call has no cursor, so collect_features walks the plane
list instead -- get_num_of_planes plus get_plane_features -- which reaches the same
SurfaceFeature objects the gizmo hands out, minus the picking.

The cap reports the true total rather than silently shortening the list, because a truncated
list that claims to be complete is the get_presets failure (T2) in a new place.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Measure between two features, without dereferencing null

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp`
- Test: `tests/slic3rutils/test_measure_utils.cpp`

**Interfaces:**
- Consumes: `MeasuredFeature`, `collect_features` (Task 3).
- Produces: `Measure::MeasurementResult Slic3r::GUI::OrcaMCP::measure_between(Measure::Measuring& measuring, const Transform3d& to_report_space, Measure::SurfaceFeature a, Measure::SurfaceFeature b)`

**Why this task exists separately.** `Measure::get_measurement` dereferences `SurfaceFeature::world_plane_features` (`Measure.hpp:89`) in two branches — Edge↔Plane (`Measure.cpp:979`) and Circle↔Plane (`Measure.cpp:1237`) — and that member is a `shared_ptr` initialised to `nullptr`. It is only ever filled by `GLGizmoMeasure::update_world_plane_features` (`GLGizmoMeasure.cpp:2398`). Feed it a Plane straight out of `get_plane_features` and those branches dereference null. `measure_between` does what the gizmo does first.

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_measure_utils.cpp`:

```cpp
TEST_CASE("Two parallel faces of the box measure its thickness", "[orcamcp][measure]")
{
    Measure::Measuring measuring(scraper_box());
    const MeasuredFeatures found = collect_features(measuring, Transform3d::Identity(), 1000);

    const MeasuredFeature* top    = plane_with_normal(found, Vec3d(0., 0., 1.));
    const MeasuredFeature* bottom = plane_with_normal(found, Vec3d(0., 0., -1.));
    REQUIRE(top != nullptr);
    REQUIRE(bottom != nullptr);

    const Measure::MeasurementResult result =
        measure_between(measuring, Transform3d::Identity(), top->feature, bottom->feature);

    REQUIRE(result.distance_infinite.has_value());
    CHECK_THAT(result.distance_infinite->dist, WithinAbs(6.0, 1e-6));
}

TEST_CASE("Two perpendicular faces of the box measure a right angle", "[orcamcp][measure]")
{
    Measure::Measuring measuring(scraper_box());
    const MeasuredFeatures found = collect_features(measuring, Transform3d::Identity(), 1000);

    const MeasuredFeature* top  = plane_with_normal(found, Vec3d(0., 0., 1.));
    const MeasuredFeature* side = plane_with_normal(found, Vec3d(1., 0., 0.));
    REQUIRE(top != nullptr);
    REQUIRE(side != nullptr);

    const Measure::MeasurementResult result =
        measure_between(measuring, Transform3d::Identity(), top->feature, side->feature);

    REQUIRE(result.angle.has_value());
    CHECK_THAT(Slic3r::Geometry::rad2deg(result.angle->angle), WithinAbs(90.0, 1e-6));
}

TEST_CASE("An edge measured against a plane does not dereference a null plane feature list",
          "[orcamcp][measure]")
{
    // Regression: Measure::get_measurement reads f2.world_plane_features for Edge-Plane pairs and
    // that shared_ptr is null on anything Measuring did not hand out through get_feature().
    Measure::Measuring measuring(scraper_box());
    const MeasuredFeatures found = collect_features(measuring, Transform3d::Identity(), 1000);

    const MeasuredFeature* side = plane_with_normal(found, Vec3d(1., 0., 0.));
    REQUIRE(side != nullptr);

    const MeasuredFeature* edge = nullptr;
    for (const MeasuredFeature& f : found.features) {
        if (f.feature.get_type() != Measure::SurfaceFeatureType::Edge)
            continue;
        const auto [start, end] = f.feature.get_edge();
        // An edge that is neither parallel nor perpendicular to the +X plane's normal does not
        // exist on a box, so take any edge of the opposite face: the branch under test is the one
        // that runs when are_perpendicular() is false for at least one endpoint pair.
        if (f.plane != side->plane) { edge = &f; break; }
    }
    REQUIRE(edge != nullptr);

    const Measure::MeasurementResult result =
        measure_between(measuring, Transform3d::Identity(), edge->feature, side->feature);
    CHECK(result.has_any_data());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `use of undeclared identifier 'measure_between'`.

- [ ] **Step 3: Declare it**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp`, after `find_feature`:

```cpp
// Distance and angle between two features, taken by value because a Plane needs fields filled in
// before it is measurable.
//
// Measure::get_measurement dereferences SurfaceFeature::world_plane_features for Edge-Plane and
// Circle-Plane pairs (Measure.cpp:979 and :1237). That member is null on any feature that did not
// come out of Measuring::get_feature(), which is every feature collect_features produces. This
// fills it the way GLGizmoMeasure::update_world_plane_features does before measuring.
Measure::MeasurementResult measure_between(Measure::Measuring&     measuring,
                                           const Transform3d&      to_report_space,
                                           Measure::SurfaceFeature a,
                                           Measure::SurfaceFeature b);
```

- [ ] **Step 4: Implement it**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp`, inside the anonymous namespace:

```cpp
// Mirror of GLGizmoMeasure::update_world_plane_features (GLGizmoMeasure.cpp:2398).
void prepare_plane_feature(Measure::Measuring&      measuring,
                           const Transform3d&       to_report_space,
                           Measure::SurfaceFeature& feature)
{
    if (feature.get_type() != Measure::SurfaceFeatureType::Plane)
        return;

    const int plane_index = std::get<0>(feature.get_plane());
    if (plane_index < 0 || plane_index >= measuring.get_num_of_planes())
        return;

    feature.world_tran = to_report_space;
    feature.plane_indices = const_cast<std::vector<int>*>(
        &measuring.get_plane_triangle_indices(plane_index));

    const std::vector<Measure::SurfaceFeature>& local_features =
        measuring.get_plane_features(static_cast<unsigned int>(plane_index));

    feature.world_plane_features = std::make_shared<std::vector<Measure::SurfaceFeature>>();
    feature.world_plane_features->reserve(local_features.size());
    for (const Measure::SurfaceFeature& local : local_features) {
        Measure::SurfaceFeature transformed(local);
        transformed.translate(to_report_space);
        feature.world_plane_features->push_back(std::move(transformed));
    }
}
```

and, outside it:

```cpp
Measure::MeasurementResult measure_between(Measure::Measuring&     measuring,
                                           const Transform3d&      to_report_space,
                                           Measure::SurfaceFeature a,
                                           Measure::SurfaceFeature b)
{
    prepare_plane_feature(measuring, to_report_space, a);
    prepare_plane_feature(measuring, to_report_space, b);
    return Measure::get_measurement(a, b);
}
```

- [ ] **Step 5: Run the tests**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][measure]"
```
Expected: PASS, 15 test cases.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp \
        tests/slic3rutils/test_measure_utils.cpp
git commit -m "$(cat <<'EOF'
feat: measuring against a plane would have dereferenced a null pointer

Measure::get_measurement reads f2.world_plane_features for Edge-Plane and Circle-Plane
pairs (Measure.cpp:979, :1237), and that shared_ptr is null on every feature that did not
come out of Measuring::get_feature(). GLGizmoMeasure fills it in
update_world_plane_features before it ever measures; collect_features does not, so
measure_between now does the same fix-up on both operands.

Checked the sibling branches: Point-Plane and Plane-Plane read only the plane's normal and
origin, so they were never at risk and are left alone. Circle-Circle reads
world_plane_features only through the Circle-Plane path already covered.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Register `measure_object`

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp:66`, `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:4333`, `src/slic3r/CMakeLists.txt:426`

**Interfaces:**
- Consumes: `collect_features`, `find_feature`, `measure_between`, `feature_to_json`, `measurement_to_json`, `MeasuredFeatures` (Tasks 1–4); `run_on_main_thread` (`OrcaMCPCommon.hpp:19`).
- Produces: `static void OrcaMCPServer::register_creation_tools();` — the registrar Tasks 6, 10 and 11 also add to.

- [ ] **Step 1: Declare the registrar**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp`, after line 66 (`static void register_printer_tools();`), add:

```cpp
    // Measurement and creation tools (OrcaMCPCreationTools.cpp)
    static void register_creation_tools();
```

In `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp`, after line 4333 (`register_printer_tools();`), add:

```cpp
    register_creation_tools();
```

- [ ] **Step 2: Write the registrar with `measure_object`**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPMeasureUtils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// Default cap on the feature list. A machined part with many small chamfers can produce hundreds of
// planes; the response has to stay inside an MCP client's per-result limit.
constexpr int k_default_max_features = 200;

nlohmann::json bbox_to_json(const BoundingBoxf3& bbox)
{
    const Vec3d size = bbox.size();
    return {
        {"size_x", size.x()}, {"size_y", size.y()}, {"size_z", size.z()},
        {"min", {{"x", bbox.min.x()}, {"y", bbox.min.y()}, {"z", bbox.min.z()}}},
        {"max", {{"x", bbox.max.x()}, {"y", bbox.max.y()}, {"z", bbox.max.z()}}}
    };
}

// The volume this call measures, and the transform from its mesh coordinates into the reported
// space. `world` is plate coordinates -- what get_scene_info and move_object talk in. `local` is the
// volume's own mesh frame, which is what emboss_text's offsets are measured in.
struct MeasureTarget
{
    const ModelVolume* volume = nullptr;
    Transform3d        to_report_space = Transform3d::Identity();
    int                volume_id = 0;
};

bool resolve_measure_target(const ModelObject& object, int volume_id, bool world_space,
                            MeasureTarget& out, std::string& error)
{
    if (object.volumes.empty()) {
        error = "Object has no volumes to measure";
        return false;
    }
    if (volume_id < 0 || volume_id >= static_cast<int>(object.volumes.size())) {
        error = "Invalid volume_id: " + std::to_string(volume_id) + " (object has " +
                std::to_string(object.volumes.size()) + " volumes)";
        return false;
    }
    const ModelVolume* volume = object.volumes[volume_id];
    if (volume->mesh_ptr() == nullptr || volume->mesh_ptr()->its.indices.empty()) {
        error = "Volume " + std::to_string(volume_id) + " has no mesh";
        return false;
    }
    out.volume    = volume;
    out.volume_id = volume_id;
    out.to_report_space = world_space && !object.instances.empty()
        ? object.instances[0]->get_transformation().get_matrix() * volume->get_matrix()
        : Transform3d::Identity();
    return true;
}

} // namespace

void OrcaMCPServer::register_creation_tools()
{
    register_tool({
        "measure_object",
        "Measure an object: exact dimensions, and the planes, edges and circles its mesh contains. "
        "Read-only. Pass measure_between with two feature ids from a previous call to get the "
        "distance and angle between them, the same numbers the Measure gizmo shows.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"volume_id", {
                    {"type", "integer"},
                    {"description", "Volume/part index within the object (0-based, default 0)"}
                }},
                {"include_features", {
                    {"type", "boolean"},
                    {"description", "List detected features (default true). Set false for dimensions only."}
                }},
                {"coordinates", {
                    {"type", "string"},
                    {"enum", {"world", "local"}},
                    {"description", "world (default) = plate coordinates, as get_scene_info reports. "
                                    "local = the volume's own mesh frame, as emboss_text offsets use."}
                }},
                {"max_features", {
                    {"type", "integer"},
                    {"description", "Cap on returned features (default 200). The true total is always reported."}
                }},
                {"measure_between", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Exactly two feature ids, e.g. [\"p0.4\", \"p3.0\"]"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = 0;
            if (!parse_integer_param(params.value("object_id", nlohmann::json(nullptr)), object_id))
                return {{"status", "error"}, {"message", "object_id is required and must be an integer"}};

            int volume_id = 0;
            if (params.contains("volume_id") && !parse_integer_param(params["volume_id"], volume_id))
                return {{"status", "error"}, {"message", "volume_id must be an integer"}};

            bool include_features = true;
            if (params.contains("include_features") &&
                !parse_boolean_param(params["include_features"], include_features))
                return {{"status", "error"}, {"message", "include_features must be a boolean"}};

            int max_features = k_default_max_features;
            if (params.contains("max_features") && !parse_integer_param(params["max_features"], max_features))
                return {{"status", "error"}, {"message", "max_features must be an integer"}};
            if (max_features < 1)
                return {{"status", "error"}, {"message", "max_features must be at least 1"}};

            const std::string coordinates = params.value("coordinates", std::string("world"));
            if (coordinates != "world" && coordinates != "local")
                return {{"status", "error"},
                        {"message", "coordinates must be \"world\" or \"local\", got \"" + coordinates + "\""}};

            std::vector<std::string> between;
            if (params.contains("measure_between")) {
                if (!params["measure_between"].is_array())
                    return {{"status", "error"}, {"message", "measure_between must be an array of two feature ids"}};
                for (const auto& entry : params["measure_between"]) {
                    if (!entry.is_string())
                        return {{"status", "error"}, {"message", "measure_between entries must be strings"}};
                    between.push_back(entry.get<std::string>());
                }
                if (between.size() != 2)
                    return {{"status", "error"},
                            {"message", "measure_between needs exactly two feature ids, got " +
                                        std::to_string(between.size())}};
            }

            return run_on_main_thread([object_id, volume_id, include_features, max_features,
                                       coordinates, between]() -> nlohmann::json {
                Plater* plater = wxGetApp().plater();
                Model&  model  = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size()))
                    return {{"status", "error"},
                            {"message", "Invalid object_id: " + std::to_string(object_id)}};

                ModelObject* object = model.objects[object_id];
                MeasureTarget target;
                std::string   error;
                if (!resolve_measure_target(*object, volume_id, coordinates == "world", target, error))
                    return {{"status", "error"}, {"message", error}};

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", object_id},
                    {"object_name", object->name},
                    {"volume_id", target.volume_id},
                    {"volume_name", target.volume->name},
                    {"coordinates", coordinates},
                    // instance_bounding_box is snug. get_object_info reports bounding_box_approx,
                    // which transforms the raw box instead of the mesh and is therefore larger for a
                    // rotated object -- if the two disagree, this one is the measurement.
                    {"bounding_box", bbox_to_json(object->instances.empty()
                                                      ? object->raw_mesh_bounding_box()
                                                      : object->instance_bounding_box(0))},
                    {"raw_bounding_box", bbox_to_json(object->raw_mesh_bounding_box())},
                    {"triangle_count", static_cast<int>(target.volume->mesh_ptr()->its.indices.size())}
                };

                if (!include_features && between.empty())
                    return result;

                Measure::Measuring measuring(target.volume->mesh_ptr()->its);
                const MeasuredFeatures found =
                    collect_features(measuring, target.to_report_space, max_features);

                result["plane_count"]   = found.plane_count;
                result["feature_total"] = found.total;

                if (include_features) {
                    nlohmann::json features = nlohmann::json::array();
                    for (const MeasuredFeature& f : found.features) {
                        nlohmann::json entry = feature_to_json(f.feature);
                        entry["id"] = f.id;
                        features.push_back(std::move(entry));
                    }
                    result["features"] = std::move(features);
                    if (found.truncated) {
                        result["features_truncated"] = true;
                        result["hint"] = "Showing " + std::to_string(found.features.size()) + " of " +
                                         std::to_string(found.total) +
                                         " features. Raise max_features to see more.";
                    }
                }

                if (!between.empty()) {
                    const MeasuredFeature* a = find_feature(found, between[0]);
                    const MeasuredFeature* b = find_feature(found, between[1]);
                    if (a == nullptr || b == nullptr) {
                        result["status"] = "partial";
                        result["measurement_error"] =
                            std::string("Unknown feature id: ") + (a == nullptr ? between[0] : between[1]) +
                            ". Feature ids come from this tool's own features array and change when the "
                            "mesh changes.";
                    } else {
                        result["measurement"] = measurement_to_json(
                            measure_between(measuring, target.to_report_space, a->feature, b->feature));
                        result["measurement"]["between"] = {between[0], between[1]};
                    }
                }
                return result;
            });
        }
    });
}
```

- [ ] **Step 3: Add the source to CMake**

In `src/slic3r/CMakeLists.txt`, after the `GUI/OrcaMCP/OrcaMCPMeasureUtils.cpp` line added in Task 1, add:

```cmake
    GUI/OrcaMCP/OrcaMCPCreationTools.cpp
```

- [ ] **Step 4: Build and check the tool registers**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'
```
Expected: build succeeds; the count prints `71`.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp src/slic3r/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: an agent could not check its own work against the model's real dimensions

The Measure gizmo had no MCP equivalent, so an agent that scaled or cut a part had no way to
read back what it had actually made. measure_object reports the snug bounding box and the
mesh's planes, edges and circles, and measures between any two of them.

It deliberately reports instance_bounding_box, not the bounding_box_approx that
get_object_info uses: approx transforms the raw box rather than the mesh (Model.cpp:1512),
so for a rotated object it is larger than the part. get_object_info is left alone -- changing
its numbers would move a value callers already depend on -- and measure_object's description
says which one is the measurement.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Font discovery and resolution, plus `get_fonts`

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossFont.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossFont.cpp`
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp`
- Create: `tests/slic3rutils/test_emboss_shape.cpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp`, `src/slic3r/CMakeLists.txt`, `tests/slic3rutils/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `std::vector<std::string> Slic3r::GUI::OrcaMCP::filter_font_names(const std::vector<std::string>& names, const std::string& name_contains, int limit, int& total)` (in `OrcaMCPEmbossShape.hpp` — pure, no wx)
  - `struct Slic3r::GUI::OrcaMCP::ResolvedFont { Slic3r::Emboss::FontFileWithCache font; Slic3r::EmbossStyle style; }`
  - `std::vector<std::string> Slic3r::GUI::OrcaMCP::installed_font_faces()`
  - `bool Slic3r::GUI::OrcaMCP::resolve_font(const std::string& face_name, bool bold, bool italic, float size_mm, ResolvedFont& out, std::string& error)`

- [ ] **Step 1: Write the failing test for the pure half**

Create `tests/slic3rutils/test_emboss_shape.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp"

#include <string>
#include <vector>

// emboss_text / emboss_svg's geometry and filtering. Nothing here opens a font or a window: the
// font name filter is string work, the placement transform is Eigen, and the shape-to-mesh step
// takes an ExPolygon this file builds by hand.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

namespace {

const std::vector<std::string> k_faces = {
    "Arial", "Arial Black", "Courier New", "Helvetica", "Helvetica Neue", "Times New Roman"
};

} // namespace

TEST_CASE("An unfiltered font list is capped but reports the true total", "[orcamcp][emboss]")
{
    int total = 0;
    const std::vector<std::string> got = filter_font_names(k_faces, "", 3, total);

    CHECK(got.size() == 3);
    CHECK(total == 6);
    CHECK(got[0] == "Arial");
}

TEST_CASE("Font name filtering is case-insensitive on a substring", "[orcamcp][emboss]")
{
    int total = 0;
    const std::vector<std::string> got = filter_font_names(k_faces, "helvet", 100, total);

    CHECK(total == 2);
    REQUIRE(got.size() == 2);
    CHECK(got[0] == "Helvetica");
    CHECK(got[1] == "Helvetica Neue");
}

TEST_CASE("A font filter that matches nothing returns nothing, not everything", "[orcamcp][emboss]")
{
    int total = 0;
    const std::vector<std::string> got = filter_font_names(k_faces, "Comic Sans", 100, total);

    CHECK(total == 0);
    CHECK(got.empty());
}
```

- [ ] **Step 2: Run test to verify it fails**

Add to `tests/slic3rutils/CMakeLists.txt` after `test_measure_utils.cpp`:

```cmake
    test_emboss_shape.cpp
```

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `'slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp' file not found`.

- [ ] **Step 3: Create the pure header and the filter**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp
#pragma once
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Case-insensitive substring filter over installed face names, capped at `limit`. `total` receives
// the number that matched before the cap -- a capped list that hides its total is the get_presets
// failure (T2), and this machine has several hundred faces.
std::vector<std::string> filter_font_names(const std::vector<std::string>& names,
                                           const std::string&             name_contains,
                                           int                            limit,
                                           int&                           total);

}}} // namespace Slic3r::GUI::OrcaMCP
```

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp
#include "OrcaMCPEmbossShape.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

std::string to_lower(const std::string& text)
{
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

} // namespace

std::vector<std::string> filter_font_names(const std::vector<std::string>& names,
                                           const std::string&             name_contains,
                                           int                            limit,
                                           int&                           total)
{
    const std::string needle = to_lower(name_contains);
    std::vector<std::string> matched;
    total = 0;
    for (const std::string& name : names) {
        if (!needle.empty() && to_lower(name).find(needle) == std::string::npos)
            continue;
        ++total;
        if (static_cast<int>(matched.size()) < limit)
            matched.push_back(name);
    }
    return matched;
}

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 4: Run the tests**

Add to `src/slic3r/CMakeLists.txt` next to the other OrcaMCP entries:

```cmake
    GUI/OrcaMCP/OrcaMCPEmbossShape.hpp
    GUI/OrcaMCP/OrcaMCPEmbossShape.cpp
```

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][emboss]"
```
Expected: PASS, 3 test cases.

- [ ] **Step 5: Write the wx-dependent font resolver**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossFont.hpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossFont.hpp
#pragma once
#include <string>
#include <vector>
#include "libslic3r/Emboss.hpp"
#include "libslic3r/TextConfiguration.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A font the caller named, opened and ready to turn into glyph outlines.
struct ResolvedFont
{
    Slic3r::Emboss::FontFileWithCache font;
    // style.path holds the wx font descriptor, so the GUI's Emboss gizmo can reopen the same font
    // when the user selects the volume this font produced.
    Slic3r::EmbossStyle               style;
};

// Every face name the OS reports. Sorted, de-duplicated.
//
// NOT Emboss::get_font_list(): that is implemented for Windows only (Emboss.cpp:901 vs the stub at
// Emboss.cpp:1026 that returns {} everywhere else). wxFontEnumerator is what GLGizmoEmboss actually
// uses on all three platforms (GLGizmoEmboss.cpp:989).
std::vector<std::string> installed_font_faces();

// Resolve a face name to an opened font. `face_name` empty means "the OS default face".
//
// Returns false with `error` set, and never substitutes silently: wxFont::SetFaceName on a name the
// OS does not have still yields an IsOk() font backed by whatever the system picked, which would
// emboss text in a font the caller never asked for. So IsValidFacename is checked first, and a face
// that is installed but not backed by a usable TrueType file (macOS .dfont -- Courier, Geneva,
// Monaco; see is_valid_ttf at WxFontUtils.cpp:19) gets its own distinct message.
bool resolve_font(const std::string& face_name,
                  bool               bold,
                  bool               italic,
                  float              size_mm,
                  ResolvedFont&      out,
                  std::string&       error);

}}} // namespace Slic3r::GUI::OrcaMCP
```

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossFont.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossFont.cpp
#include "OrcaMCPEmbossFont.hpp"

#include "slic3r/Utils/WxFontUtils.hpp"

#include <wx/font.h>
#include <wx/fontenum.h>

#include <algorithm>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// GLGizmoEmboss enumerates with this encoding (Facenames::encoding, GLGizmoEmboss.cpp:215).
constexpr wxFontEncoding k_encoding = wxFontEncoding::wxFONTENCODING_SYSTEM;

} // namespace

std::vector<std::string> installed_font_faces()
{
    wxFontEnumerator::InvalidateCache();
    const wxArrayString facenames = wxFontEnumerator::GetFacenames(k_encoding);

    std::vector<std::string> names;
    names.reserve(facenames.GetCount());
    for (const wxString& facename : facenames)
        names.emplace_back(facename.ToUTF8().data());

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

bool resolve_font(const std::string& face_name,
                  bool               bold,
                  bool               italic,
                  float              size_mm,
                  ResolvedFont&      out,
                  std::string&       error)
{
    wxFontInfo info(static_cast<double>(size_mm));
    info.Encoding(k_encoding);
    if (bold)
        info.Weight(wxFONTWEIGHT_BOLD);
    if (italic)
        info.Style(wxFONTSTYLE_ITALIC);

    if (!face_name.empty()) {
        const wxString wx_face = wxString::FromUTF8(face_name.c_str());
        wxFontEnumerator::InvalidateCache();
        if (!wxFontEnumerator::IsValidFacename(wx_face)) {
            error = "Font \"" + face_name + "\" is not installed on this machine. "
                    "Call get_fonts to list the fonts that are.";
            return false;
        }
        info.FaceName(wx_face);
    }

    wxFont wx_font(info);
    if (!wx_font.IsOk()) {
        error = face_name.empty()
                    ? std::string("Could not open the system default font")
                    : "Font \"" + face_name + "\" could not be opened";
        return false;
    }

    std::unique_ptr<Slic3r::Emboss::FontFile> font_file = WxFontUtils::create_font_file(wx_font);
    if (font_file == nullptr) {
        error = "Font \"" + (face_name.empty() ? WxFontUtils::get_human_readable_name(wx_font)
                                               : face_name) +
                "\" is installed but is not backed by a usable TrueType file, so it cannot be "
                "embossed. Call get_fonts and pick another.";
        return false;
    }

    out.font  = Slic3r::Emboss::FontFileWithCache(std::move(font_file));
    out.style = WxFontUtils::create_emboss_style(wx_font);
    out.style.prop.size_in_mm = size_mm;
    return true;
}

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 6: Register `get_fonts`**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp` — the include list gains:

```cpp
#include "OrcaMCPEmbossFont.hpp"
#include "OrcaMCPEmbossShape.hpp"
```

and, inside `register_creation_tools()` after the `measure_object` block:

```cpp
    register_tool({
        "get_fonts",
        "List the font face names installed on this machine, for use as emboss_text's `font` "
        "parameter. Narrow with name_contains; the true match count is always reported.",
        {
            {"type", "object"},
            {"properties", {
                {"name_contains", {
                    {"type", "string"},
                    {"description", "Case-insensitive substring filter"}
                }},
                {"limit", {
                    {"type", "integer"},
                    {"description", "Maximum names to return (default 50)"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const std::string name_contains = params.value("name_contains", std::string());
            int limit = 50;
            if (params.contains("limit") && !parse_integer_param(params["limit"], limit))
                return {{"status", "error"}, {"message", "limit must be an integer"}};
            if (limit < 1)
                return {{"status", "error"}, {"message", "limit must be at least 1"}};

            // wxFontEnumerator touches the platform font manager, which is main-thread-only on macOS.
            return run_on_main_thread([name_contains, limit]() -> nlohmann::json {
                int total = 0;
                const std::vector<std::string> installed = installed_font_faces();
                const std::vector<std::string> names =
                    filter_font_names(installed, name_contains, limit, total);

                nlohmann::json result = {
                    {"status", "success"},
                    {"fonts", names},
                    {"returned", static_cast<int>(names.size())},
                    {"matched", total},
                    {"installed_total", static_cast<int>(installed.size())}
                };
                if (static_cast<int>(names.size()) < total)
                    result["hint"] = "Showing " + std::to_string(names.size()) + " of " +
                                     std::to_string(total) +
                                     " matches. Narrow with name_contains or raise limit.";
                return result;
            });
        }
    });
```

- [ ] **Step 7: Build and verify the count**

Add the two new sources to `src/slic3r/CMakeLists.txt`:

```cmake
    GUI/OrcaMCP/OrcaMCPEmbossFont.hpp
    GUI/OrcaMCP/OrcaMCPEmbossFont.cpp
```

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][emboss]"
grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'
```
Expected: build succeeds, 3 emboss tests pass, count prints `72`.

- [ ] **Step 8: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossFont.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossFont.cpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp \
        src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp tests/slic3rutils/test_emboss_shape.cpp \
        src/slic3r/CMakeLists.txt tests/slic3rutils/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: naming a font that is not installed would have embossed a different font

wxFont::SetFaceName on a name the OS does not have still yields an IsOk() font, backed by
whatever the system substituted -- so a typo in `font` would have produced a real, wrong
result rather than an error. resolve_font checks wxFontEnumerator::IsValidFacename first and
refuses.

The second failure is distinct and gets its own message: a face can be installed and still
have no usable TrueType file behind it. On macOS is_valid_ttf (WxFontUtils.cpp:19) rejects
.dfont, which is Courier, Geneva and Monaco, and create_font_file returns nullptr for them.

Emboss::get_font_list was deliberately not used: it is implemented for Windows only
(Emboss.cpp:901) and is a stub returning {} on macOS and Linux (Emboss.cpp:1026), which
would have made get_fonts report zero fonts on two of three platforms. The gizmo does not
use it either.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: The face placement transform

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp`
- Test: `tests/slic3rutils/test_emboss_shape.cpp`

**Interfaces:**
- Consumes: `filter_font_names` lives in the same header; nothing else.
- Produces:
  - `enum class Slic3r::GUI::OrcaMCP::EmbossFace { Top, Bottom, Front, Back, Left, Right }`
  - `bool Slic3r::GUI::OrcaMCP::parse_emboss_face(const std::string& name, EmbossFace& out)`
  - `struct Slic3r::GUI::OrcaMCP::EmbossPlacement { EmbossFace face; double offset_u; double offset_v; double rotation_deg; }`
  - `Slic3r::Transform3d Slic3r::GUI::OrcaMCP::emboss_face_transform(const BoundingBoxf3& bbox, const EmbossPlacement& placement)`

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_emboss_shape.cpp`, and add `#include "libslic3r/BoundingBox.hpp"` at the top:

```cpp
namespace {

// The scraper's raw mesh box: 40 x 122 x 6, corner at the origin, as its_make_cube produces.
BoundingBoxf3 scraper_bbox()
{
    return BoundingBoxf3(Vec3d(0., 0., 0.), Vec3d(40., 122., 6.));
}

} // namespace

TEST_CASE("Face names parse, and nothing else does", "[orcamcp][emboss]")
{
    EmbossFace face = EmbossFace::Bottom;
    REQUIRE(parse_emboss_face("top", face));
    CHECK(face == EmbossFace::Top);
    REQUIRE(parse_emboss_face("right", face));
    CHECK(face == EmbossFace::Right);
    CHECK_FALSE(parse_emboss_face("Top", face));      // exact, lower-case, as the schema enum says
    CHECK_FALSE(parse_emboss_face("", face));
    CHECK_FALSE(parse_emboss_face("sideways", face));
}

TEST_CASE("The top face places the origin on the top, pointing up", "[orcamcp][emboss]")
{
    const Transform3d trafo = emboss_face_transform(scraper_bbox(), {EmbossFace::Top, 0., 0., 0.});

    const Vec3d origin = trafo * Vec3d::Zero();
    CHECK_THAT(origin.x(), WithinAbs(20., 1e-9));   // centre of X
    CHECK_THAT(origin.y(), WithinAbs(61., 1e-9));   // centre of Y
    CHECK_THAT(origin.z(), WithinAbs(6., 1e-9));    // the top surface

    // Local +Z is the outward normal of the face.
    const Vec3d normal = trafo.linear() * Vec3d::UnitZ();
    CHECK_THAT(normal.z(), WithinAbs(1., 1e-9));
}

TEST_CASE("The front face points along -Y and keeps Z as its up axis", "[orcamcp][emboss]")
{
    const Transform3d trafo = emboss_face_transform(scraper_bbox(), {EmbossFace::Front, 0., 0., 0.});

    const Vec3d origin = trafo * Vec3d::Zero();
    CHECK_THAT(origin.y(), WithinAbs(0., 1e-9));    // the y-min face
    CHECK_THAT(origin.x(), WithinAbs(20., 1e-9));
    CHECK_THAT(origin.z(), WithinAbs(3., 1e-9));

    const Vec3d normal = trafo.linear() * Vec3d::UnitZ();
    CHECK_THAT(normal.y(), WithinAbs(-1., 1e-9));

    // Text must read the right way up: the shape's local +Y goes to world +Z on a side face.
    const Vec3d up = trafo.linear() * Vec3d::UnitY();
    CHECK_THAT(up.z(), WithinAbs(1., 1e-9));
}

TEST_CASE("Offsets move within the face, in millimetres", "[orcamcp][emboss]")
{
    const Transform3d trafo = emboss_face_transform(scraper_bbox(), {EmbossFace::Top, 5., -10., 0.});

    const Vec3d origin = trafo * Vec3d::Zero();
    CHECK_THAT(origin.x(), WithinAbs(25., 1e-9));   // 20 + 5 along the face's right axis
    CHECK_THAT(origin.y(), WithinAbs(51., 1e-9));   // 61 - 10 along the face's up axis
    CHECK_THAT(origin.z(), WithinAbs(6., 1e-9));
}

TEST_CASE("Rotation turns the shape in the plane of the face", "[orcamcp][emboss]")
{
    // The scraper request: text running along the 122 mm length instead of across the 40 mm width.
    const Transform3d trafo = emboss_face_transform(scraper_bbox(), {EmbossFace::Top, 0., 0., 90.});

    const Vec3d right = trafo.linear() * Vec3d::UnitX();
    CHECK_THAT(right.x(), WithinAbs(0., 1e-9));
    CHECK_THAT(right.y(), WithinAbs(1., 1e-9));

    // Rotating in the plane must not tip the shape off the face.
    const Vec3d normal = trafo.linear() * Vec3d::UnitZ();
    CHECK_THAT(normal.z(), WithinAbs(1., 1e-9));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `unknown type name 'EmbossFace'`.

- [ ] **Step 3: Declare the types**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp`, above `filter_font_names`, and add `#include "libslic3r/BoundingBox.hpp"` and `#include "libslic3r/Point.hpp"`:

```cpp
// The six faces of an object's raw-mesh bounding box. Arbitrary placement on a curved or angled
// surface is out of scope: the gizmo gets that from a screen ray, and an MCP call has none.
enum class EmbossFace { Top, Bottom, Front, Back, Left, Right };

// Exact lower-case match on the tool schema's enum. Anything else is a caller error, not something
// to guess at.
bool parse_emboss_face(const std::string& name, EmbossFace& out);

struct EmbossPlacement
{
    EmbossFace face         = EmbossFace::Top;
    double     offset_u     = 0.0;  // mm from the face centre, along the face's right axis
    double     offset_v     = 0.0;  // mm from the face centre, along the face's up axis
    double     rotation_deg = 0.0;  // in the plane of the face, CCW seen from outside
};

// The ModelVolume transform that puts an emboss shape on `face` of `bbox`.
//
// `bbox` is the object's raw-mesh bounding box (ModelObject::raw_mesh_bounding_box, Model.hpp:477),
// so the result is in the object's own frame and survives moving or rotating the object.
//
// The shape's local frame is the one polygons2model produces: the outline lies in local XY and
// extrudes along local +Z. The transform maps local +Z onto the face's outward normal, so the mesh
// grows out of the object for a MODEL_PART and into it for a NEGATIVE_VOLUME. The 0.015 mm of
// overlap that makes the union watertight is already baked into the mesh by emboss_shape_to_mesh.
Transform3d emboss_face_transform(const BoundingBoxf3& bbox, const EmbossPlacement& placement);
```

- [ ] **Step 4: Implement it**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp`, with `#include "libslic3r/Geometry.hpp"` added at the top:

```cpp
bool parse_emboss_face(const std::string& name, EmbossFace& out)
{
    if (name == "top")    { out = EmbossFace::Top;    return true; }
    if (name == "bottom") { out = EmbossFace::Bottom; return true; }
    if (name == "front")  { out = EmbossFace::Front;  return true; }
    if (name == "back")   { out = EmbossFace::Back;   return true; }
    if (name == "left")   { out = EmbossFace::Left;   return true; }
    if (name == "right")  { out = EmbossFace::Right;  return true; }
    return false;
}

Transform3d emboss_face_transform(const BoundingBoxf3& bbox, const EmbossPlacement& placement)
{
    const Vec3d center = bbox.center();

    // For each face: its outward normal, the face's "right" axis (local +X) and its "up" axis
    // (local +Y). Up is world +Z on the four side faces, so text reads the right way up; on top and
    // bottom, where +Z is the normal, up is world +Y.
    Vec3d normal, right, up, face_center;
    switch (placement.face) {
    case EmbossFace::Top:
        normal = Vec3d::UnitZ();  right = Vec3d::UnitX();   up = Vec3d::UnitY();
        face_center = Vec3d(center.x(), center.y(), bbox.max.z());
        break;
    case EmbossFace::Bottom:
        normal = -Vec3d::UnitZ(); right = -Vec3d::UnitX();  up = Vec3d::UnitY();
        face_center = Vec3d(center.x(), center.y(), bbox.min.z());
        break;
    case EmbossFace::Front:
        normal = -Vec3d::UnitY(); right = Vec3d::UnitX();   up = Vec3d::UnitZ();
        face_center = Vec3d(center.x(), bbox.min.y(), center.z());
        break;
    case EmbossFace::Back:
        normal = Vec3d::UnitY();  right = -Vec3d::UnitX();  up = Vec3d::UnitZ();
        face_center = Vec3d(center.x(), bbox.max.y(), center.z());
        break;
    case EmbossFace::Left:
        normal = -Vec3d::UnitX(); right = -Vec3d::UnitY();  up = Vec3d::UnitZ();
        face_center = Vec3d(bbox.min.x(), center.y(), center.z());
        break;
    case EmbossFace::Right:
    default:
        normal = Vec3d::UnitX();  right = Vec3d::UnitY();   up = Vec3d::UnitZ();
        face_center = Vec3d(bbox.max.x(), center.y(), center.z());
        break;
    }

    const Vec3d origin = face_center + right * placement.offset_u + up * placement.offset_v;

    Matrix3d basis;
    basis.col(0) = right;
    basis.col(1) = up;
    basis.col(2) = normal;

    Transform3d trafo = Transform3d::Identity();
    trafo.translate(origin);
    trafo.linear() = basis;
    // In-plane rotation, applied in the shape's own frame so it cannot tip the shape off the face.
    trafo.rotate(Eigen::AngleAxisd(Geometry::deg2rad(placement.rotation_deg), Vec3d::UnitZ()));
    return trafo;
}
```

- [ ] **Step 5: Run the tests**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][emboss]"
```
Expected: PASS, 8 test cases.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp \
        tests/slic3rutils/test_emboss_shape.cpp
git commit -m "$(cat <<'EOF'
feat: emboss had no way to say where on an object the shape goes

The gizmo takes its position from a screen ray, which an MCP call does not have.
emboss_face_transform names the six faces of the object's raw-mesh bounding box instead,
with an in-plane offset and rotation.

Two details that would have shipped wrong: the up axis is world +Z on the four side faces
and world +Y on top and bottom, so text reads the right way up rather than lying on its side;
and the rotation is applied in the shape's own frame, after the basis, so it turns the text
in the plane instead of tipping it off the face.

It works from raw_mesh_bounding_box, not the instance box, so the placement is in the
object's own frame and survives a later move_object or rotate_object.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Shape → mesh, and the two shape builders

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp`
- Test: `tests/slic3rutils/test_emboss_shape.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `Slic3r::TriangleMesh Slic3r::GUI::OrcaMCP::emboss_shape_to_mesh(Slic3r::EmbossShape& shape, bool is_outside)`
  - `bool Slic3r::GUI::OrcaMCP::build_svg_shape(const std::string& svg_path, double depth_mm, Slic3r::EmbossShape& out, std::string& error)`
  - `bool Slic3r::GUI::OrcaMCP::build_text_shape(Slic3r::Emboss::FontFileWithCache& font, const std::string& text, const Slic3r::FontProp& prop, double depth_mm, Slic3r::EmbossShape& out, std::string& error)`

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_emboss_shape.cpp`, adding `#include "libslic3r/EmbossShape.hpp"`, `#include "libslic3r/TriangleMesh.hpp"`, `#include <boost/filesystem.hpp>` and `#include <fstream>`:

```cpp
namespace {

// A 10 x 4 mm rectangle as an EmbossShape, in the integer scale EmbossShape::scale converts to mm.
EmbossShape rectangle_shape(double depth_mm)
{
    EmbossShape shape;
    shape.scale = SCALING_FACTOR;
    shape.projection.depth = depth_mm;
    shape.projection.use_surface = false;

    Polygon outline;
    outline.points = {
        Point(scale_(0.),  scale_(0.)),
        Point(scale_(10.), scale_(0.)),
        Point(scale_(10.), scale_(4.)),
        Point(scale_(0.),  scale_(4.))
    };
    ExPolygonsWithId entry;
    entry.id = 0;
    entry.expoly = {ExPolygon(outline)};
    shape.shapes_with_ids = {entry};
    return shape;
}

std::string write_temp_svg(const std::string& body)
{
    const boost::filesystem::path path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orcamcp-%%%%.svg");
    std::ofstream out(path.string());
    out << body;
    out.close();
    return path.string();
}

} // namespace

TEST_CASE("A raised shape becomes a mesh of the requested depth", "[orcamcp][emboss]")
{
    EmbossShape shape = rectangle_shape(2.0);
    const TriangleMesh mesh = emboss_shape_to_mesh(shape, /*is_outside=*/true);

    REQUIRE_FALSE(mesh.its.indices.empty());
    const BoundingBoxf3 bbox = mesh.bounding_box();
    CHECK_THAT(bbox.size().x(), WithinAbs(10., 1e-3));
    CHECK_THAT(bbox.size().y(), WithinAbs(4., 1e-3));
    CHECK_THAT(bbox.size().z(), WithinAbs(2., 1e-3));

    // A raised shape starts 0.015 mm below the surface, so the union with the parent is watertight.
    CHECK_THAT(bbox.min.z(), WithinAbs(-0.015, 1e-4));
}

TEST_CASE("An engraved shape sits above the surface and cuts down", "[orcamcp][emboss]")
{
    EmbossShape shape = rectangle_shape(2.0);
    const TriangleMesh mesh = emboss_shape_to_mesh(shape, /*is_outside=*/false);

    const BoundingBoxf3 bbox = mesh.bounding_box();
    CHECK_THAT(bbox.size().z(), WithinAbs(2., 1e-3));
    CHECK_THAT(bbox.max.z(), WithinAbs(0.015, 1e-4));
}

TEST_CASE("An SVG file becomes an emboss shape", "[orcamcp][emboss]")
{
    const std::string path = write_temp_svg(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="20mm" height="10mm" viewBox="0 0 20 10">)"
        R"(<rect x="2" y="2" width="16" height="6" fill="black"/></svg>)");

    EmbossShape shape;
    std::string error;
    REQUIRE(build_svg_shape(path, 3.0, shape, error));
    CHECK(error.empty());
    CHECK_FALSE(shape.shapes_with_ids.empty());
    CHECK_THAT(shape.projection.depth, WithinAbs(3.0, 1e-9));
    REQUIRE(shape.svg_file.has_value());
    CHECK(shape.svg_file->path == path);

    boost::filesystem::remove(path);
}

TEST_CASE("A missing SVG is an error, not an empty shape", "[orcamcp][emboss]")
{
    EmbossShape shape;
    std::string error;
    CHECK_FALSE(build_svg_shape("/nowhere/does/this/exist.svg", 3.0, shape, error));
    CHECK(error.find("does not exist") != std::string::npos);
}

TEST_CASE("An SVG with no path is an error naming the file", "[orcamcp][emboss]")
{
    const std::string path = write_temp_svg(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="20mm" height="10mm"></svg>)");

    EmbossShape shape;
    std::string error;
    CHECK_FALSE(build_svg_shape(path, 3.0, shape, error));
    CHECK(error.find(path) != std::string::npos);

    boost::filesystem::remove(path);
}

TEST_CASE("A file that is not named .svg is refused before it is parsed", "[orcamcp][emboss]")
{
    EmbossShape shape;
    std::string error;
    CHECK_FALSE(build_svg_shape("/tmp/logo.png", 3.0, shape, error));
    CHECK(error.find(".svg") != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8`
Expected: FAIL — `use of undeclared identifier 'emboss_shape_to_mesh'`.

- [ ] **Step 3: Declare the three functions**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp`, and add `#include "libslic3r/Emboss.hpp"`, `#include "libslic3r/EmbossShape.hpp"`, `#include "libslic3r/TextConfiguration.hpp"`, `#include "libslic3r/TriangleMesh.hpp"`:

```cpp
// EmbossShape -> mesh, in the shape's own frame: the outline in local XY, extruded along local +Z.
//
// This is the non-per-glyph half of try_create_mesh (EmbossJob.cpp:927), lifted because that one is
// in an anonymous namespace and only reachable from a plater Job. Same constants, same projection,
// same 0.015 mm SAFE_SURFACE_OFFSET overlap (EmbossJob.cpp:63), so the mesh matches what the gizmo
// would have made.
//
// Returns an empty mesh when the shape has no printable outline -- text made only of spaces, an SVG
// of zero-width strokes. The caller must treat that as an error rather than adding an empty volume.
TriangleMesh emboss_shape_to_mesh(EmbossShape& shape, bool is_outside);

// Glyph outlines for `text` in `font`, ready for emboss_shape_to_mesh.
bool build_text_shape(Slic3r::Emboss::FontFileWithCache& font,
                      const std::string&                 text,
                      const FontProp&                    prop,
                      double                             depth_mm,
                      EmbossShape&                       out,
                      std::string&                       error);

// Paths from an .svg file, ready for emboss_shape_to_mesh.
//
// Mirrors select_shape (GLGizmoSVG.cpp:2206) with one deliberate difference: every failure returns
// false with a message, where the gizmo calls show_error(). A modal opened inside
// run_on_main_thread hangs the GUI forever, and McpDialogSuppressionGuard only covers MsgDialog
// subclasses -- so the right fix is not to open one.
bool build_svg_shape(const std::string& svg_path,
                     double             depth_mm,
                     EmbossShape&       out,
                     std::string&       error);
```

- [ ] **Step 4: Implement them**

Add to `src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp`, with these includes at the top:

```cpp
#include "libslic3r/NSVGUtils.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
```

and this body:

```cpp
namespace {

// EmbossJob.cpp:63. Redefined here because it is file-local there; keep the two in step.
constexpr float k_safe_surface_offset = 0.015f;

// GLGizmoSVG.cpp:87 get_tesselation_tolerance(1.), expanded. Curve-to-line tolerance of 0.1 mm,
// expressed in the integer image scale.
double svg_tesselation_tolerance()
{
    const double tolerance_mm = 0.1;
    return (tolerance_mm * tolerance_mm) / SCALING_FACTOR / SCALING_FACTOR;
}

} // namespace

TriangleMesh emboss_shape_to_mesh(EmbossShape& shape, bool is_outside)
{
    const ExPolygons shapes = Slic3r::union_with_delta(shape, Slic3r::Emboss::UNION_DELTA,
                                                       Slic3r::Emboss::UNION_MAX_ITERATIN);
    if (shapes.empty())
        return {};

    // SHAPE_SCALE is applied by the projection, exactly as try_create_mesh does it.
    const double scale = shape.scale;
    const double depth = shape.projection.depth / scale;
    auto project_z = std::make_unique<Slic3r::Emboss::ProjectZ>(depth);

    const float offset = is_outside
                             ? -k_safe_surface_offset
                             : static_cast<float>(k_safe_surface_offset - shape.projection.depth);
    const Transform3d tr =
        Eigen::Translation<double, 3>(0., 0., static_cast<double>(offset)) * Eigen::Scaling(scale);
    Slic3r::Emboss::ProjectTransform project(std::move(project_z), tr);

    return TriangleMesh(Slic3r::Emboss::polygons2model(shapes, project));
}

bool build_text_shape(Slic3r::Emboss::FontFileWithCache& font,
                      const std::string&                 text,
                      const FontProp&                    prop,
                      double                             depth_mm,
                      EmbossShape&                       out,
                      std::string&                       error)
{
    if (!font.has_value()) {
        error = "No font is loaded";
        return false;
    }
    if (text.empty()) {
        error = "text is empty";
        return false;
    }

    out = EmbossShape();
    out.projection.depth       = depth_mm;
    out.projection.use_surface = false;
    out.scale = Slic3r::Emboss::get_text_shape_scale(prop, *font.font_file);
    out.shapes_with_ids = Slic3r::Emboss::text2vshapes(font, boost::nowide::widen(text), prop);

    if (out.shapes_with_ids.empty()) {
        error = "The text \"" + text + "\" produced no printable outline in this font";
        return false;
    }
    return true;
}

bool build_svg_shape(const std::string& svg_path,
                     double             depth_mm,
                     EmbossShape&       out,
                     std::string&       error)
{
    if (svg_path.empty()) {
        error = "svg_path is required";
        return false;
    }
    if (!boost::algorithm::iends_with(svg_path, ".svg")) {
        error = "svg_path must name a .svg file, got \"" + svg_path + "\"";
        return false;
    }
    if (!boost::filesystem::exists(boost::filesystem::path(svg_path))) {
        error = "SVG file does not exist: " + svg_path;
        return false;
    }

    out = EmbossShape();
    out.projection.depth       = depth_mm;
    out.projection.use_surface = false;

    EmbossShape::SvgFile svg;
    svg.path = svg_path;
    if (Slic3r::init_image(svg) == nullptr) {
        error = "The SVG parser could not read " + svg_path;
        return false;
    }

    const NSVGLineParams params{svg_tesselation_tolerance()};
    out.shapes_with_ids = Slic3r::create_shape_with_ids(*svg.image, params);
    if (out.shapes_with_ids.empty()) {
        error = "SVG file contains no path that can be embossed: " + svg_path;
        return false;
    }
    out.svg_file = std::move(svg);
    return true;
}
```

- [ ] **Step 5: Run the tests**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][emboss]"
```
Expected: PASS, 14 test cases.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPEmbossShape.cpp \
        tests/slic3rutils/test_emboss_shape.cpp
git commit -m "$(cat <<'EOF'
feat: the only path from an emboss shape to a mesh ran inside a plater Job

try_create_mesh and select_shape are both file-local to their gizmo/job translation units, so
neither is reachable from an MCP handler, and both of select_shape's failure paths call
show_error -- a modal, which inside run_on_main_thread hangs the GUI forever.

emboss_shape_to_mesh reproduces the non-per-glyph half of try_create_mesh with the same
projection and the same 0.015 mm SAFE_SURFACE_OFFSET, so the mesh matches what the gizmo
would have made. build_svg_shape reproduces select_shape returning an error string for each
of the four cases the gizmo shows a dialog for.

The per-glyph branch of try_create_mesh was deliberately not copied: it needs TextLinesModel
against the live Selection, and text-on-a-curve is out of scope for this batch.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Attach an embossed volume to an object

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp`

**Interfaces:**
- Consumes: `emboss_face_transform` (Task 7).
- Produces (file-local to `OrcaMCPCreationTools.cpp`, used by Tasks 10 and 11):
  - `ModelVolume* attach_emboss_volume(ModelObject& object, TriangleMesh&& mesh, ModelVolumeType type, const Transform3d& trafo, const EmbossShape& shape, const std::optional<TextConfiguration>& text_configuration, const std::string& volume_name)`
  - `bool parse_volume_type(const std::string& name, ModelVolumeType& out)`
  - `nlohmann::json emboss_result_json(int object_id, const ModelObject& object, const ModelVolume& volume, int volume_id)`

- [ ] **Step 1: Add the helpers**

Add to the anonymous namespace in `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp`, with `#include "OrcaMCPEmbossShape.hpp"`, `#include "slic3r/GUI/GUI_ObjectList.hpp"` and `#include "libslic3r/EmbossShape.hpp"` added at the top:

```cpp
bool parse_volume_type(const std::string& name, ModelVolumeType& out)
{
    if (name == "part")     { out = ModelVolumeType::MODEL_PART;         return true; }
    if (name == "negative") { out = ModelVolumeType::NEGATIVE_VOLUME;    return true; }
    if (name == "modifier") { out = ModelVolumeType::PARAMETER_MODIFIER; return true; }
    return false;
}

// Add the embossed mesh to `object` as a new ModelVolume.
//
// Mirrors create_volume (EmbossJob.cpp:1061), minus the parts that only make sense from a job: no
// notification, no gizmo opening, and no selection change -- an MCP call must not move the user's
// selection out from under them.
//
// The cube-then-set_mesh dance is not cosmetic: ModelObject::add_volume re-centres the mesh it is
// given and bakes the offset into the volume transform, which would throw away the placement.
// Adding a 1 mm cube first and replacing the mesh afterwards keeps the transform we computed.
ModelVolume* attach_emboss_volume(ModelObject&                           object,
                                  TriangleMesh&&                         mesh,
                                  ModelVolumeType                        type,
                                  const Transform3d&                     trafo,
                                  const EmbossShape&                     shape,
                                  const std::optional<TextConfiguration>& text_configuration,
                                  const std::string&                     volume_name)
{
    ModelVolume* volume = object.add_volume(make_cube(1., 1., 1.), type);
    volume->set_mesh(std::move(mesh));
    volume->calculate_convex_hull();
    volume->set_transformation(trafo);

    // The user cannot pick an extruder for a volume they never selected; 0 means "inherit".
    volume->config.set_key_value("extruder", new ConfigOptionInt(0));
    // Not loaded from a file, so "reload from disk" must not offer to.
    volume->source.is_from_builtin_objects = true;
    volume->name = volume_name;

    volume->emboss_shape = shape;
    volume->emboss_shape->fix_3mf_tr.reset();
    volume->text_configuration = text_configuration;

    if (type == ModelVolumeType::MODEL_PART)
        object.ensure_on_bed();
    object.invalidate_bounding_box();
    return volume;
}

nlohmann::json emboss_result_json(int object_id, const ModelObject& object,
                                  const ModelVolume& volume, int volume_id)
{
    Plater* plater = wxGetApp().plater();
    return {
        {"status", "success"},
        {"object_id", object_id},
        {"object_name", object.name},
        {"volume_id", volume_id},
        {"volume_name", volume.name},
        {"volume_size", {
            {"x", volume.mesh().bounding_box().size().x()},
            {"y", volume.mesh().bounding_box().size().y()},
            {"z", volume.mesh().bounding_box().size().z()}
        }},
        {"object_bounding_box", bbox_to_json(object.instances.empty()
                                                 ? object.raw_mesh_bounding_box()
                                                 : object.instance_bounding_box(0))},
        {"active_warnings", get_active_warnings_json(plater)}
    };
}
```

- [ ] **Step 2: Build**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8`
Expected: build succeeds. (Nothing calls these yet; the compiler may warn about unused static functions — that is expected and disappears in Task 10. If the build treats it as an error, land this task together with Task 10.)

- [ ] **Step 3: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp
git commit -m "$(cat <<'EOF'
feat: adding an emboss volume needed the job's finalize step, which no MCP call can reach

create_volume in EmbossJob.cpp runs from a plater Job's finalize and is file-local.
attach_emboss_volume does the model half of it synchronously and drops the three parts that
only make sense from the GUI: the notification, opening the Emboss gizmo, and changing the
selection -- an MCP call must not move the user's selection.

Kept the cube-then-set_mesh sequence deliberately: ModelObject::add_volume re-centres the
mesh it is handed and folds the offset into the volume transform, which would discard the
placement emboss_face_transform computed.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Register `emboss_text`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp`

**Interfaces:**
- Consumes: `resolve_font`, `ResolvedFont` (Task 6); `parse_emboss_face`, `EmbossPlacement`, `emboss_face_transform`, `build_text_shape`, `emboss_shape_to_mesh` (Tasks 7–8); `parse_volume_type`, `attach_emboss_volume`, `emboss_result_json` (Task 9).
- Produces: the `emboss_text` tool.

- [ ] **Step 1: Add a shared placement parser**

Add to the anonymous namespace in `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp` (Task 11 uses it too):

```cpp
// The parameters emboss_text and emboss_svg share. Both tools parse them the same way, so the
// errors read the same way.
struct EmbossCommon
{
    int             object_id  = 0;
    ModelVolumeType volume_type = ModelVolumeType::MODEL_PART;
    EmbossPlacement placement;
    double          depth_mm   = 1.0;
};

bool parse_emboss_common(const nlohmann::json& params, EmbossCommon& out, std::string& error)
{
    if (!parse_integer_param(params.value("object_id", nlohmann::json(nullptr)), out.object_id)) {
        error = "object_id is required and must be an integer";
        return false;
    }

    const std::string type_name = params.value("volume_type", std::string("part"));
    if (!parse_volume_type(type_name, out.volume_type)) {
        error = "volume_type must be \"part\", \"negative\" or \"modifier\", got \"" + type_name + "\"";
        return false;
    }

    const std::string face_name = params.value("face", std::string("top"));
    if (!parse_emboss_face(face_name, out.placement.face)) {
        error = "face must be one of top, bottom, front, back, left, right (lower case), got \"" +
                face_name + "\"";
        return false;
    }

    out.placement.offset_u     = params.value("offset_u", 0.0);
    out.placement.offset_v     = params.value("offset_v", 0.0);
    out.placement.rotation_deg = params.value("rotation_deg", 0.0);
    out.depth_mm               = params.value("depth_mm", 1.0);

    if (out.depth_mm <= 0.0) {
        error = "depth_mm must be greater than 0, got " + std::to_string(out.depth_mm);
        return false;
    }
    return true;
}
```

- [ ] **Step 2: Register the tool**

Add to `register_creation_tools()` in `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp`, after `get_fonts`:

```cpp
    register_tool({
        "emboss_text",
        "Put text on an object as a new part (raised), negative volume (engraved) or modifier. "
        "Placement is on one of the six faces of the object's bounding box, with an offset and an "
        "in-plane rotation; arbitrary placement on a curved surface is not supported. The font must "
        "be an exact installed face name -- call get_fonts to list them. A font that is not "
        "installed is an error, never a substitution.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
                {"text", {{"type", "string"}, {"description", "The text. \\n starts a new line."}}},
                {"font", {
                    {"type", "string"},
                    {"description", "Exact installed face name from get_fonts. Omit for the system default."}
                }},
                {"bold", {{"type", "boolean"}, {"description", "Default false"}}},
                {"italic", {{"type", "boolean"}, {"description", "Default false"}}},
                {"size_mm", {{"type", "number"}, {"description", "Line height in mm (default 10)"}}},
                {"depth_mm", {{"type", "number"}, {"description", "Emboss depth in mm (default 1)"}}},
                {"char_gap", {{"type", "integer"}, {"description", "Extra space between letters, in font points"}}},
                {"line_gap", {{"type", "integer"}, {"description", "Extra space between lines, in font points"}}},
                {"align_h", {{"type", "string"}, {"enum", {"left", "center", "right"}}, {"description", "Default center"}}},
                {"align_v", {{"type", "string"}, {"enum", {"top", "center", "bottom"}}, {"description", "Default center"}}},
                {"volume_type", {
                    {"type", "string"}, {"enum", {"part", "negative", "modifier"}},
                    {"description", "part (default) = raised, negative = engraved"}
                }},
                {"face", {
                    {"type", "string"}, {"enum", {"top", "bottom", "front", "back", "left", "right"}},
                    {"description", "Which face of the object's bounding box (default top)"}
                }},
                {"offset_u", {{"type", "number"}, {"description", "mm from the face centre, along the face's right axis"}}},
                {"offset_v", {{"type", "number"}, {"description", "mm from the face centre, along the face's up axis"}}},
                {"rotation_deg", {{"type", "number"}, {"description", "Rotation within the face, CCW seen from outside"}}}
            }},
            {"required", {"object_id", "text"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            EmbossCommon common;
            std::string  error;
            if (!parse_emboss_common(params, common, error))
                return {{"status", "error"}, {"message", error}};

            const std::string text = params.value("text", std::string());
            if (text.empty())
                return {{"status", "error"}, {"message", "text is required and must not be empty"}};

            const std::string font_name = params.value("font", std::string());
            bool bold = false, italic = false;
            if (params.contains("bold") && !parse_boolean_param(params["bold"], bold))
                return {{"status", "error"}, {"message", "bold must be a boolean"}};
            if (params.contains("italic") && !parse_boolean_param(params["italic"], italic))
                return {{"status", "error"}, {"message", "italic must be a boolean"}};

            const double size_mm = params.value("size_mm", 10.0);
            if (size_mm <= 0.0)
                return {{"status", "error"},
                        {"message", "size_mm must be greater than 0, got " + std::to_string(size_mm)}};

            const std::string align_h = params.value("align_h", std::string("center"));
            const std::string align_v = params.value("align_v", std::string("center"));
            FontProp::HorizontalAlign h_align;
            FontProp::VerticalAlign   v_align;
            if (align_h == "left")        h_align = FontProp::HorizontalAlign::left;
            else if (align_h == "center") h_align = FontProp::HorizontalAlign::center;
            else if (align_h == "right")  h_align = FontProp::HorizontalAlign::right;
            else return {{"status", "error"},
                         {"message", "align_h must be left, center or right, got \"" + align_h + "\""}};
            if (align_v == "top")         v_align = FontProp::VerticalAlign::top;
            else if (align_v == "center") v_align = FontProp::VerticalAlign::center;
            else if (align_v == "bottom") v_align = FontProp::VerticalAlign::bottom;
            else return {{"status", "error"},
                         {"message", "align_v must be top, center or bottom, got \"" + align_v + "\""}};

            std::optional<int> char_gap;
            std::optional<int> line_gap;
            int gap = 0;
            if (params.contains("char_gap")) {
                if (!parse_integer_param(params["char_gap"], gap))
                    return {{"status", "error"}, {"message", "char_gap must be an integer"}};
                char_gap = gap;
            }
            if (params.contains("line_gap")) {
                if (!parse_integer_param(params["line_gap"], gap))
                    return {{"status", "error"}, {"message", "line_gap must be an integer"}};
                line_gap = gap;
            }

            return run_on_main_thread([common, text, font_name, bold, italic, size_mm,
                                       h_align, v_align, char_gap, line_gap]() -> nlohmann::json {
                McpDialogSuppressionGuard dialog_guard;

                Plater* plater = wxGetApp().plater();
                Model&  model  = plater->model();

                if (common.object_id < 0 || common.object_id >= static_cast<int>(model.objects.size()))
                    return {{"status", "error"},
                            {"message", "Invalid object_id: " + std::to_string(common.object_id)}};

                ModelObject* object = model.objects[common.object_id];

                ResolvedFont font;
                std::string  error;
                if (!resolve_font(font_name, bold, italic, static_cast<float>(size_mm), font, error))
                    return {{"status", "error"}, {"message", error}, {"parameter", "font"}};

                FontProp prop = font.style.prop;
                prop.size_in_mm = static_cast<float>(size_mm);
                prop.align      = FontProp::Align(h_align, v_align);
                prop.per_glyph  = false;   // text on a curve is out of scope
                prop.char_gap   = char_gap;
                prop.line_gap   = line_gap;

                EmbossShape shape;
                if (!build_text_shape(font.font, text, prop, common.depth_mm, shape, error))
                    return {{"status", "error"}, {"message", error}, {"parameter", "text"}};

                const bool is_outside = common.volume_type == ModelVolumeType::MODEL_PART;
                TriangleMesh mesh = emboss_shape_to_mesh(shape, is_outside);
                if (mesh.its.empty())
                    return {{"status", "error"},
                            {"message", "The text \"" + text +
                                        "\" produced an empty mesh. Try a larger size_mm or another font."}};

                std::string volume_name = text;
                std::replace(volume_name.begin(), volume_name.end(), '\n', ' ');

                plater->take_snapshot("Emboss text \"" + volume_name + "\"");

                font.style.prop = prop;
                TextConfiguration text_configuration{font.style, text};
                const Transform3d trafo =
                    emboss_face_transform(object->raw_mesh_bounding_box(), common.placement);

                ModelVolume* volume = attach_emboss_volume(*object, std::move(mesh),
                                                           common.volume_type, trafo, shape,
                                                           text_configuration, volume_name);
                const int volume_id = static_cast<int>(object->volumes.size()) - 1;

                wxGetApp().obj_list()->update_object_list_by_model(false);
                plater->update();

                nlohmann::json result =
                    emboss_result_json(common.object_id, *object, *volume, volume_id);
                result["font"] = font.style.prop.face_name.value_or(font.style.name);
                result["face"] = params_face_name(common.placement.face);

                const std::vector<std::string> messages = dialog_guard.messages();
                if (!messages.empty())
                    result["info_messages"] = messages;
                return result;
            });
        }
    });
```

Add the small reverse lookup next to `parse_emboss_face`'s use, in the same anonymous namespace:

```cpp
const char* params_face_name(EmbossFace face)
{
    switch (face) {
    case EmbossFace::Top:    return "top";
    case EmbossFace::Bottom: return "bottom";
    case EmbossFace::Front:  return "front";
    case EmbossFace::Back:   return "back";
    case EmbossFace::Left:   return "left";
    case EmbossFace::Right:  return "right";
    }
    return "top";
}
```

Note: the lambda captures `common` by value but references `params` in the `params_face_name` line — change that line to use `common.placement.face`, which it already does. The `params` name does not appear inside `run_on_main_thread`.

- [ ] **Step 3: Build and count**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8
grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'
```
Expected: build succeeds; count prints `73`.

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp
git commit -m "$(cat <<'EOF'
feat: the Emboss gizmo had no MCP equivalent, so text needed a mouse

emboss_text builds the glyph outlines, extrudes them and attaches the volume synchronously,
which is why it can answer inside one MCP call: the gizmo's route runs through the plater
worker and finishes after the call would have returned.

Placement is limited to the six faces of the object's bounding box and stated as such in the
tool description. Arbitrary surface placement needs a screen ray through RaycastManager,
which an MCP call does not have, and guessing one would be a second raycasting problem
rather than a smaller one.

use_surface and per_glyph are left off for the same reason: both need the live Selection and
a slice of the parent. The volume still carries text_configuration and emboss_shape, so the
GUI gizmo can select and edit anything this tool made.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Register `emboss_svg`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp`

**Interfaces:**
- Consumes: `parse_emboss_common`, `EmbossCommon`, `attach_emboss_volume`, `emboss_result_json`, `params_face_name` (Tasks 9–10); `build_svg_shape`, `emboss_shape_to_mesh`, `emboss_face_transform` (Tasks 7–8).
- Produces: the `emboss_svg` tool.

**Shared, not duplicated.** Everything after "produce an `EmbossShape`" is identical to `emboss_text` and is reached through the same functions: `emboss_shape_to_mesh`, `emboss_face_transform`, `attach_emboss_volume`, `emboss_result_json`. Only `build_svg_shape` and the `svg_path` / `scale` parameters are new here.

- [ ] **Step 1: Register the tool**

Add to `register_creation_tools()` in `src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp`, after `emboss_text`:

```cpp
    register_tool({
        "emboss_svg",
        "Put an SVG file's paths on an object as a new part (raised), negative volume (engraved) or "
        "modifier. Same placement rules as emboss_text: one of the six faces of the object's "
        "bounding box, with an offset and an in-plane rotation. The whole file becomes one volume; "
        "per-path volumes are not supported.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
                {"svg_path", {{"type", "string"}, {"description", "Absolute path to a .svg file"}}},
                {"size_mm", {
                    {"type", "number"},
                    {"description", "Width of the result in mm. Omit to keep the SVG's own size."}
                }},
                {"depth_mm", {{"type", "number"}, {"description", "Emboss depth in mm (default 1)"}}},
                {"volume_type", {
                    {"type", "string"}, {"enum", {"part", "negative", "modifier"}},
                    {"description", "part (default) = raised, negative = engraved"}
                }},
                {"face", {
                    {"type", "string"}, {"enum", {"top", "bottom", "front", "back", "left", "right"}},
                    {"description", "Which face of the object's bounding box (default top)"}
                }},
                {"offset_u", {{"type", "number"}, {"description", "mm from the face centre, along the face's right axis"}}},
                {"offset_v", {{"type", "number"}, {"description", "mm from the face centre, along the face's up axis"}}},
                {"rotation_deg", {{"type", "number"}, {"description", "Rotation within the face, CCW seen from outside"}}}
            }},
            {"required", {"object_id", "svg_path"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            EmbossCommon common;
            std::string  error;
            if (!parse_emboss_common(params, common, error))
                return {{"status", "error"}, {"message", error}};

            const std::string svg_path = params.value("svg_path", std::string());
            if (svg_path.empty())
                return {{"status", "error"}, {"message", "svg_path is required"}};

            const double size_mm = params.value("size_mm", 0.0);   // 0 = keep the SVG's own size
            if (size_mm < 0.0)
                return {{"status", "error"}, {"message", "size_mm must not be negative"}};

            return run_on_main_thread([common, svg_path, size_mm]() -> nlohmann::json {
                McpDialogSuppressionGuard dialog_guard;

                Plater* plater = wxGetApp().plater();
                Model&  model  = plater->model();

                if (common.object_id < 0 || common.object_id >= static_cast<int>(model.objects.size()))
                    return {{"status", "error"},
                            {"message", "Invalid object_id: " + std::to_string(common.object_id)}};

                ModelObject* object = model.objects[common.object_id];

                EmbossShape shape;
                std::string error;
                if (!build_svg_shape(svg_path, common.depth_mm, shape, error))
                    return {{"status", "error"}, {"message", error}, {"parameter", "svg_path"}};

                const bool is_outside = common.volume_type == ModelVolumeType::MODEL_PART;
                TriangleMesh mesh = emboss_shape_to_mesh(shape, is_outside);
                if (mesh.its.empty())
                    return {{"status", "error"},
                            {"message", "The SVG produced an empty mesh: " + svg_path}};

                // Scale in the volume transform rather than in the shape, so the emboss depth stays
                // the depth the caller asked for.
                double scale_factor = 1.0;
                if (size_mm > 0.0) {
                    const double current_width = mesh.bounding_box().size().x();
                    if (current_width <= 0.0)
                        return {{"status", "error"},
                                {"message", "The SVG has no width to scale: " + svg_path}};
                    scale_factor = size_mm / current_width;
                }

                const std::string volume_name =
                    boost::filesystem::path(svg_path).stem().string();

                plater->take_snapshot("Emboss SVG \"" + volume_name + "\"");

                Transform3d trafo =
                    emboss_face_transform(object->raw_mesh_bounding_box(), common.placement);
                if (scale_factor != 1.0)
                    trafo = trafo * Eigen::Scaling(scale_factor, scale_factor, 1.0);

                ModelVolume* volume = attach_emboss_volume(*object, std::move(mesh),
                                                           common.volume_type, trafo, shape,
                                                           std::nullopt, volume_name);
                const int volume_id = static_cast<int>(object->volumes.size()) - 1;

                wxGetApp().obj_list()->update_object_list_by_model(false);
                plater->update();

                nlohmann::json result =
                    emboss_result_json(common.object_id, *object, *volume, volume_id);
                result["svg_path"] = svg_path;
                result["face"]     = params_face_name(common.placement.face);
                result["paths"]    = static_cast<int>(shape.shapes_with_ids.size());

                const std::vector<std::string> messages = dialog_guard.messages();
                if (!messages.empty())
                    result["info_messages"] = messages;
                return result;
            });
        }
    });
```

- [ ] **Step 2: Build and count**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'
```
Expected: build succeeds; count prints `74`.

- [ ] **Step 3: Run the full test suite**

Run:
```
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: 0 failures. The case count is the 174 baseline plus the 20 added by Tasks 1–8.

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPCreationTools.cpp
git commit -m "$(cat <<'EOF'
feat: the Svg gizmo had no MCP equivalent either

emboss_svg differs from emboss_text in one function: build_svg_shape parses the file with
nanosvg where build_text_shape rasterises glyphs. Everything after that -- the extrusion,
the face placement, the volume attach, the snapshot and the response -- is the same code, not
a copy of it.

size_mm scales in the volume transform rather than in the shape, so the emboss depth stays
the depth the caller asked for; scaling the shape would have scaled Z with it.

Per-path volumes were deliberately left out: set_object_filament can already address a
volume, but splitting an SVG into one volume per path is a different feature and belongs
with split_object.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Documentation and the tool catalogue

**Files:**
- Modify: `docs/tools/reference.md`, `CLAUDE.md`, `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:648-810`

**Interfaces:**
- Consumes: the four registered tools (Tasks 5, 6, 10, 11).
- Produces: nothing code depends on.

- [ ] **Step 1: Add the tools to `get_server_info`**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp`, inside `tools_by_category`, add a new category after the `object_transforms` block (which ends at line 686 with `{"transform_objects", ...}`):

```cpp
                    {"measurement_and_creation", {
                        {"measure_object", "Exact dimensions, plus the planes/edges/circles of the mesh and the distance or angle between any two of them. Read-only."},
                        {"get_fonts", "List the font face names installed on this machine, for emboss_text's `font`."},
                        {"emboss_text", "Put text on one of the six faces of an object's bounding box, raised or engraved."},
                        {"emboss_svg", "Put an SVG file's paths on one of the six faces of an object's bounding box, raised or engraved."}
                    }},
```

- [ ] **Step 2: Update `docs/tools/reference.md`**

Add a row to the quick-reference table after the `**Per-Object**` row:

```markdown
| **Measure & Create** | `measure_object`, `get_fonts`, `emboss_text`, `emboss_svg` |
```

And add a new section before `## Slicing Tools`:

```markdown
## Measurement & Creation Tools

### measure_object
Measure an object: exact dimensions, and the planes, edges and circles its mesh contains. Read-only.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `volume_id` | integer | No | Volume/part index within the object (default 0) |
| `include_features` | boolean | No | List detected features (default true) |
| `coordinates` | string | No | `world` (default, plate coordinates) or `local` (the volume's mesh frame) |
| `max_features` | integer | No | Cap on returned features (default 200) |
| `measure_between` | array | No | Exactly two feature ids from a previous call |

**Coordinates:** `world` matches what `get_scene_info` and `move_object` report. `local` matches the frame `emboss_text`'s `offset_u` / `offset_v` are measured in.

**`bounding_box` vs `get_object_info`:** `measure_object` reports the snug `instance_bounding_box`. `get_object_info` reports `bounding_box_approx`, which transforms the raw box rather than the mesh and is therefore larger for a rotated object. When the two disagree, `measure_object` is the measurement.

**Feature ids** are `p<plane>.<index>` and change whenever the mesh changes. Read them from this tool's own `features` array; do not construct them.

**Example — the thickness of a plate:**
```json
{"name": "measure_object", "arguments": {"object_id": 0}}
{"name": "measure_object", "arguments": {"object_id": 0, "measure_between": ["p0.4", "p1.4"], "include_features": false}}
```

**Returns:**
```json
{
  "status": "success",
  "object_id": 0,
  "volume_id": 0,
  "coordinates": "world",
  "bounding_box": {"size_x": 40.0, "size_y": 122.0, "size_z": 6.0, "min": {...}, "max": {...}},
  "raw_bounding_box": {"size_x": 40.0, "size_y": 122.0, "size_z": 6.0, "min": {...}, "max": {...}},
  "triangle_count": 12,
  "plane_count": 6,
  "feature_total": 30,
  "features": [
    {"id": "p0.0", "type": "edge", "start": {...}, "end": {...}, "length": 40.0},
    {"id": "p0.4", "type": "plane", "plane_index": 0, "normal": {...}, "center": {...}}
  ],
  "measurement": {"has_result": true, "distance": 6.0, "from": {...}, "to": {...}, "between": ["p0.4", "p1.4"]}
}
```

---

### get_fonts
List the font face names installed on this machine, for use as `emboss_text`'s `font`.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name_contains` | string | No | Case-insensitive substring filter |
| `limit` | integer | No | Maximum names to return (default 50) |

**Example:**
```json
{"name": "get_fonts", "arguments": {"name_contains": "helvetica"}}
```

**Returns:** `{"status": "success", "fonts": [...], "returned": 2, "matched": 2, "installed_total": 318}`

---

### emboss_text
Put text on an object as a new part (raised), negative volume (engraved) or modifier.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `text` | string | Yes | The text; `\n` starts a new line |
| `font` | string | No | Exact installed face name from `get_fonts`. Omit for the system default. |
| `bold` / `italic` | boolean | No | Default false |
| `size_mm` | number | No | Line height in mm (default 10) |
| `depth_mm` | number | No | Emboss depth in mm (default 1) |
| `char_gap` / `line_gap` | integer | No | Extra spacing, in font points |
| `align_h` | string | No | `left` / `center` (default) / `right` |
| `align_v` | string | No | `top` / `center` (default) / `bottom` |
| `volume_type` | string | No | `part` (default, raised), `negative` (engraved), `modifier` |
| `face` | string | No | `top` (default), `bottom`, `front`, `back`, `left`, `right` |
| `offset_u` / `offset_v` | number | No | mm from the face centre, along the face's right / up axis |
| `rotation_deg` | number | No | Rotation within the face, CCW seen from outside |

**Placement is axis-aligned only.** The shape goes on one of the six faces of the object's *raw mesh* bounding box, so the placement is in the object's own frame and survives a later `move_object` or `rotate_object`. Arbitrary placement on a curved or angled surface is not supported — the GUI gizmo gets that from a screen ray, which an MCP call does not have.

**A font that is not installed is an error, never a substitution.** `wxFont` silently falls back to whatever the system picks for an unknown face name; `emboss_text` refuses instead, and its message points at `get_fonts`. A face that is installed but not backed by a usable TrueType file (macOS `.dfont` — Courier, Geneva, Monaco) gets its own distinct message.

**Not supported:** `use_surface` (shape cut from a curved parent), per-glyph placement / text on a curve, editing an existing text volume, standalone text objects with no parent, fonts by file path, font collections, synthetic boldness and skew.

**Example — a label along the length of a flat part:**
```json
{"name": "emboss_text", "arguments": {
  "object_id": 0, "text": "SCRAPER", "font": "Helvetica",
  "size_mm": 12, "depth_mm": 0.8, "face": "top", "rotation_deg": 90
}}
```

**Returns:**
```json
{
  "status": "success",
  "object_id": 0,
  "volume_id": 1,
  "volume_name": "SCRAPER",
  "font": "Helvetica",
  "face": "top",
  "volume_size": {"x": 6.2, "y": 58.0, "z": 0.8},
  "object_bounding_box": {...},
  "active_warnings": {"count": 0, "warnings": []}
}
```

---

### emboss_svg
Put an SVG file's paths on an object as a new part, negative volume or modifier.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `svg_path` | string | Yes | Absolute path to a `.svg` file |
| `size_mm` | number | No | Width of the result in mm. Omit to keep the SVG's own size. |
| `depth_mm` | number | No | Emboss depth in mm (default 1) |
| `volume_type` | string | No | `part` (default), `negative`, `modifier` |
| `face` | string | No | `top` (default), `bottom`, `front`, `back`, `left`, `right` |
| `offset_u` / `offset_v` | number | No | mm from the face centre |
| `rotation_deg` | number | No | Rotation within the face |

Placement rules are identical to `emboss_text`.

**Not supported:** one volume per SVG path (the whole file becomes one volume), `use_surface`, storing the SVG into the 3MF's private area.

**Example:**
```json
{"name": "emboss_svg", "arguments": {
  "object_id": 0, "svg_path": "/Users/me/logo.svg", "size_mm": 30, "depth_mm": 1.0, "face": "front"
}}
```

---
```

Also update the file's opening line — it still says "all 50 MCP tools" while 74 are registered:

```markdown
Complete reference for all 74 MCP tools available in OrcaMCP.
```

- [ ] **Step 3: Update `CLAUDE.md`**

Change the heading:

```markdown
### MCP Tools (74 registered, 75 reachable)

The server registers 74; the bridge adds `start_orca`, which launches OrcaSlicer and
so cannot live inside it. To regenerate this count after adding a tool:
```

Add a table row after the **Adaptive** row:

```markdown
| **Measure & Create** | `measure_object`, `get_fonts`, `emboss_text`, `emboss_svg` |
```

- [ ] **Step 4: Verify the count matches the docs**

Run:
```
grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'
grep -n "74 registered" CLAUDE.md
grep -n "all 74 MCP tools" docs/tools/reference.md
```
Expected: `74`, and both greps find their line.

- [ ] **Step 5: Full build and test**

Run:
```
cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: build succeeds, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add docs/tools/reference.md CLAUDE.md src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp
git commit -m "$(cat <<'EOF'
docs: four new tools were reachable but undiscoverable

measure_object, get_fonts, emboss_text and emboss_svg reached the tools/list response but
appeared in none of the three places an agent or a maintainer looks: get_server_info's
category catalogue, reference.md, and CLAUDE.md's table.

Also corrected reference.md's opening line, which claimed 50 tools while 70 were registered
before this batch -- a count that had been stale for several releases, not just this one.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

**1. Spec coverage.** The spec's Plan 4 section names three requirements.

| Spec requirement | Task |
|---|---|
| `measure_object` — `namespace Measure` / `class Measuring`, read-only, dimensions and distances/angles between detected features, prioritised within this plan | Tasks 1–5, first in the plan |
| `emboss_text` — deliberately scoped, with what is out of scope stated | Tasks 6–10; the Scope section states every cut and why |
| `emboss_svg` — the Svg gizmo equivalent | Task 11; the shared machinery is named in its Interfaces block and reached, not copied |

Global Constraints coverage: branch and commit style — every commit block; threading — every handler body is inside `run_on_main_thread`; dialogs — `McpDialogSuppressionGuard` in Tasks 10 and 11, and `build_svg_shape` deliberately does not call `show_error`; pure logic under `tests/slic3rutils/` — Tasks 1–4, 6–8; undo/redo — `take_snapshot` in Tasks 10 and 11, with a note correcting the spec's pointer at `move_object`; registration — a new registrar file added to `src/slic3r/CMakeLists.txt` in Task 5; docs — Task 12; response shape — `status` plus `active_warnings` on the two mutating tools; verify — the build and test commands appear in Tasks 5, 6, 10, 11 and 12.

The spec's "explicitly out of scope for this batch" (`assembly_view`, `layersediting`) is respected: neither appears.

**2. Placeholder scan.** No "TBD", no "handle errors appropriately", no "similar to Task N". Every code step carries the code. Task 3 Step 5 tells the implementer what to do if `total == 30` does not hold, and names which assertions may not be weakened — that is a contingency with a decision rule, not a placeholder.

**3. Type consistency.** Checked across tasks:

- `MeasuredFeature` / `MeasuredFeatures` — declared Task 3, used Tasks 4 and 5 with the same field names (`id`, `plane`, `index`, `feature`, `features`, `plane_count`, `total`, `truncated`).
- `collect_features(Measure::Measuring&, const Transform3d&, int)` — the Task 3 test calls it with a `Measuring&`, and the Task 3 declaration and Task 5 call site match.
- `measure_between(Measure::Measuring&, const Transform3d&, Measure::SurfaceFeature, Measure::SurfaceFeature)` — by value in Task 4's declaration, and Task 5 passes `a->feature` / `b->feature` (copies), consistent.
- `EmbossFace` / `EmbossPlacement` — declared Task 7, used Tasks 10 and 11 through `EmbossCommon::placement`; `params_face_name` covers all six enumerators.
- `emboss_shape_to_mesh(EmbossShape&, bool)` — non-const reference in Task 8 because `union_with_delta` caches into `final_shape`; Tasks 10 and 11 pass a named local, not a temporary.
- `ResolvedFont::font` is `Emboss::FontFileWithCache`; `build_text_shape` takes `Emboss::FontFileWithCache&` and Task 10 passes `font.font`. Consistent.
- `attach_emboss_volume` takes `const std::optional<TextConfiguration>&`; Task 10 passes a `TextConfiguration` (implicitly converted) and Task 11 passes `std::nullopt`. Both valid.
- `bbox_to_json` is declared in Task 5's anonymous namespace and used again in Task 9's `emboss_result_json`, in the same translation unit.
- `parse_integer_param` / `parse_boolean_param` / `get_active_warnings_json` / `McpDialogSuppressionGuard` all come from `OrcaMCPCommon.hpp`, included in Task 5.

One inconsistency found and fixed inline: Task 10's first draft referenced `params` from inside the `run_on_main_thread` lambda, which does not capture it. The step now says to use `common.placement.face`.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-09-11-batch2-plan4-creation-measure.md`. Two execution options:

**1. Subagent-Driven (recommended)** — a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
