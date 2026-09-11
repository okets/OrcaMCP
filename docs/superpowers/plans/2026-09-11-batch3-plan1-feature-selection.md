# Feature Selection for Painting — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an agent paint a *feature* — "her bag" — in one or two MCP calls, and keep the MCP server answering while it does the geometry.

**Architecture:** A new pure file `OrcaMCPPaintSelect.{hpp,cpp}` (libslic3r only, no wx) holds connected-component labelling, closest-point and ray picking over `AABBMesh`, pixel-to-ray unprojection, and a seed fill driven by a tiny `TriangleSelector` subclass that reads back which original facets the fill selected. `OrcaMCPPaintTools.cpp` gains two tools (`get_object_components`, `pick_facet`), two `paint_object` selections (`connected`, `component`), and a three-hop handler shape that captures immutable mesh pointers on the main thread, computes on the HTTP worker thread, and re-verifies identities before writing. `render_plate_view` returns its camera so a pixel can be picked.

**Tech Stack:** C++17, libslic3r (`TriangleSelector`, `AABBMesh`, `its_face_neighbors_par`, TBB), nlohmann::json, Catch2, wxWidgets only in `OrcaMCPPaintTools.cpp` / `OrcaMCPPlateUtils.cpp`.

**Spec:** `docs/superpowers/plans/2026-09-11-batch-3-spec.md` — read it first; the acceptance criteria and the Global Constraints below come from it.

## Global Constraints

- Branch `sync-upstream-2.5`. Do not push. Do not create branches.
- Commit style: `feat:` / `fix:` + what changed, past tense; body names the **root cause** and lists related occurrences checked, including ones deliberately left alone and why. Every commit ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Anything touching `Model`, `Plater` or the preset bundle runs inside `run_on_main_thread()` (`OrcaMCPCommon.hpp:17-30`). Pure geometry over a captured `std::shared_ptr<const TriangleMesh>` runs on the HTTP worker thread that called the handler. A modal inside `run_on_main_thread` hangs the GUI forever.
- Never call `set_mcp_dialog_suppression()`; `McpDialogSuppressionGuard` is the only sanctioned way.
- Pure logic is a free function with Catch2 coverage under `tests/slic3rutils/`. The test binary links `libslic3r_gui` (`tests/slic3rutils/CMakeLists.txt:40`), so anything in `src/slic3r/CMakeLists.txt` is reachable.
- A mutation takes `plater->take_snapshot(...)` **before** mutating, after all validation. `set_object_printable` is the pattern.
- `assert()` compiles out (`NDEBUG` in non-Debug). Schema `required` is not enforced server-side; guard every `params[...]` with `contains`.
- Coordinates are plate millimetres. Volumes are reported as `{volume_id, name, original_facets, bounding_box, ...}`.
- Docs: `docs/tools/reference.md` (Painting Tools section starts at `:1174`, `render_plate_view` at `:983`), `CLAUDE.md` (count at `:102,104`, Painting row at `:121`), `get_server_info` catalog (`OrcaMCPServer.cpp:707`). Count goes **74 → 76**.
- Verify before reporting:
  ```
  cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
  build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
  ```
  Baseline: 257 cases / ~1785–1827 assertions / 0 failures. Skip count varies; nine `its_convex_hull` lines are known noise.
- Do not start the app except in Task 12, with the user present.

---

## File structure

| File | Responsibility |
|---|---|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp/.cpp` (new) | Pure selection geometry: component labelling, picking, unprojection, seed fill read-back. No wx, no `Plater`. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp` | `facet_centroids` goes parallel (Task 5). |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp` | The wx-aware handlers: three-hop `paint_object`, `get_object_components`, `pick_facet`. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.hpp/.cpp` | `RenderThumbnail` exposes its camera; `RenderPlateView` reports it. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` | `get_server_info` catalog entries. |
| `tests/slic3rutils/test_paint_select.cpp` (new) | Every pure function above. |
| `docs/tools/reference.md`, `CLAUDE.md` | Documentation and counts. |

Interface facts every task may rely on (all verified in the codebase):

- `TriangleSelector(const TriangleMesh& mesh, float edge_limit = 0.6f)` (`TriangleSelector.hpp:317`); `m_triangles` (`:497`) and `m_orig_size_indices` are **protected**, so a subclass can read them; `Triangle::is_split()`, `is_selected_by_seed_fill()`, `get_state()` are public.
- `void seed_fill_select_triangles(const Vec3f& hit, int facet_start, const Transform3d& trafo_no_translate, const ClippingPlane& clp, float seed_fill_angle, float highlight_by_angle_deg = 0.f, bool force_reselection = false)` (`:332`). `ClippingPlane()` default-constructs **inactive** (`offset = FLT_MAX`, `:83,86`). `hit` is **mesh-local**. `trafo_no_translate` is only used when `highlight_by_angle_deg != 0`.
- `AABBMesh(const indexed_triangle_set&, bool calculate_epsilon = false)`; `double squared_distance(const Vec3d& p, int& face, Vec3d& closest) const` (`AABBMesh.cpp:313`); `hit_result query_ray_hit(const Vec3d& s, const Vec3d& dir) const` with `is_hit()`, `face()`, `position()`, `distance()` (`AABBMesh.hpp:84-118`). All mesh-local.
- `std::vector<Vec3i32> its_face_neighbors_par(const indexed_triangle_set&)` (`TriangleMesh.hpp:201`); a negative entry means "no neighbour across that edge" (`its_num_open_edges` counts `n < 0`).
- `std::shared_ptr<const TriangleMesh> ModelVolume::mesh_ptr() const` (`Model.hpp:861`); `ObjectID ObjectBase::id() const` (`ObjectID.hpp:68`) on both `ModelObject` and `ModelVolume`.
- `Transform3d volume_to_plate(const ModelObject&, const ModelVolume&, std::size_t instance_idx)` and `FacetAssignment {std::vector<int> states; std::vector<int> band_counts; int unassigned;}` already exist (`OrcaMCPPaintModel.hpp`, `OrcaMCPPaintGeometry.hpp`).
- `Camera::get_view_matrix()`, `get_projection_matrix()` return `const Transform3d&` (`Camera.hpp:106-107`); `get_viewport()` returns `const std::array<int,4>&` (`:105`). The saved thumbnail is row-flipped so pixel row 0 is the **top** (`OrcaMCPPlateUtils.cpp:44,63`).
- TBB idiom: `tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t>& r) { for (size_t i = r.begin(); i < r.end(); ++i) ... });` with `#include <tbb/parallel_for.h>` (`CutSurface.cpp:32,811-818`).

---

### Task 1: Connected components — label, summarise, assign

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp`
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp`
- Create: `tests/slic3rutils/test_paint_select.cpp`
- Modify: `src/slic3r/CMakeLists.txt:415` (after `GUI/OrcaMCP/OrcaMCPPaintModel.cpp`)
- Modify: `tests/slic3rutils/CMakeLists.txt:32` (after `test_scalar_params.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  std::vector<int> facet_component_ids(const indexed_triangle_set& its, int& component_count);
  struct ComponentInfo { int component; int facet_count; double area; BoundingBoxf3 bbox; };
  std::vector<ComponentInfo> summarize_components(const indexed_triangle_set& its, const std::vector<int>& ids, int component_count, const Transform3d& to_plate);
  FacetAssignment assign_component(const std::vector<int>& ids, int component, int state);
  ```

- [ ] **Step 1: Write the failing tests**

Create `tests/slic3rutils/test_paint_select.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <vector>

// The geometry behind "paint her bag": which triangles form one shell, which triangle a point or
// a ray lands on, and which surface a seed fill spreads over. Pure, so it can be right before any
// of it touches a Model.

using namespace Slic3r::GUI::OrcaMCP;
using Slic3r::Vec3d;
using Slic3r::Vec3f;
using Slic3r::Vec3i32;
using Slic3r::Transform3d;
using Slic3r::indexed_triangle_set;
using Slic3r::TriangleMesh;
using Catch::Matchers::WithinAbs;

namespace {

indexed_triangle_set translated(indexed_triangle_set its, const Vec3f& offset)
{
    for (Vec3f& v : its.vertices)
        v += offset;
    return its;
}

// Two unit-ish cubes 5 mm apart: two shells, 12 facets each, cube A first in facet order.
indexed_triangle_set two_cubes()
{
    indexed_triangle_set a = Slic3r::its_make_cube(2.0, 2.0, 2.0);
    indexed_triangle_set b = translated(Slic3r::its_make_cube(1.0, 1.0, 1.0), Vec3f(5.f, 0.f, 0.f));
    Slic3r::its_merge(a, b);
    return a;
}

} // namespace

TEST_CASE("facet_component_ids labels each shell once, in discovery order", "[orcamcp][select]")
{
    int count = -1;
    const std::vector<int> ids = facet_component_ids(two_cubes(), count);

    REQUIRE(count == 2);
    REQUIRE(ids.size() == 24);
    // Cube A's 12 facets come first in the merged mesh, so they are component 0; cube B is 1.
    for (std::size_t i = 0; i < 12; ++i)
        CHECK(ids[i] == 0);
    for (std::size_t i = 12; i < 24; ++i)
        CHECK(ids[i] == 1);
}

TEST_CASE("facet_component_ids on a single closed mesh is one component", "[orcamcp][select]")
{
    int count = -1;
    const std::vector<int> ids = facet_component_ids(Slic3r::its_make_cube(3.0, 3.0, 3.0), count);
    CHECK(count == 1);
    CHECK(std::all_of(ids.begin(), ids.end(), [](int id) { return id == 0; }));
}

TEST_CASE("facet_component_ids on an empty mesh reports no components", "[orcamcp][select]")
{
    int count = -1;
    CHECK(facet_component_ids(indexed_triangle_set{}, count).empty());
    CHECK(count == 0);
}

TEST_CASE("summarize_components reports facet counts, plate bounding boxes and area per shell",
          "[orcamcp][select]")
{
    const indexed_triangle_set its = two_cubes();
    int count = 0;
    const std::vector<int> ids = facet_component_ids(its, count);
    // Shift the whole thing by +10 on X so a summary in the identity frame would be wrong.
    Transform3d to_plate = Transform3d::Identity();
    to_plate.translation() = Vec3d(10.0, 0.0, 0.0);

    const std::vector<ComponentInfo> summary = summarize_components(its, ids, count, to_plate);
    REQUIRE(summary.size() == 2);

    CHECK(summary[0].component == 0);
    CHECK(summary[0].facet_count == 12);
    CHECK_THAT(summary[0].area, WithinAbs(6.0 * 4.0, 1e-9));          // 6 faces of 2 x 2
    CHECK_THAT(summary[0].bbox.min.x(), WithinAbs(10.0, 1e-9));       // translated into plate
    CHECK_THAT(summary[0].bbox.max.x(), WithinAbs(12.0, 1e-9));

    CHECK(summary[1].component == 1);
    CHECK(summary[1].facet_count == 12);
    CHECK_THAT(summary[1].area, WithinAbs(6.0, 1e-9));                // 6 faces of 1 x 1
    CHECK_THAT(summary[1].bbox.min.x(), WithinAbs(15.0, 1e-9));
    CHECK_THAT(summary[1].bbox.max.x(), WithinAbs(16.0, 1e-9));
}

TEST_CASE("assign_component selects exactly one shell and leaves the rest alone", "[orcamcp][select]")
{
    int count = 0;
    const std::vector<int> ids = facet_component_ids(two_cubes(), count);

    const FacetAssignment a = assign_component(ids, 1, 7);
    REQUIRE(a.states.size() == 24);
    for (std::size_t i = 0; i < 12; ++i)
        CHECK(a.states[i] == -1);       // not selected: "leave as is"
    for (std::size_t i = 12; i < 24; ++i)
        CHECK(a.states[i] == 7);
    CHECK(a.unassigned == 12);
    CHECK(a.band_counts.empty());

    // A component that does not exist selects nothing rather than everything.
    const FacetAssignment none = assign_component(ids, 5, 7);
    CHECK(std::all_of(none.states.begin(), none.states.end(), [](int s) { return s == -1; }));
    CHECK(none.unassigned == 24);
}
```

- [ ] **Step 2: Add the new files to both CMake lists, create an empty header/source, run the test to see it fail**

In `src/slic3r/CMakeLists.txt`, after line 415 (`GUI/OrcaMCP/OrcaMCPPaintModel.cpp`), add:

```
    GUI/OrcaMCP/OrcaMCPPaintSelect.hpp
    GUI/OrcaMCP/OrcaMCPPaintSelect.cpp
```

In `tests/slic3rutils/CMakeLists.txt`, after `test_scalar_params.cpp`, add:

```
    test_paint_select.cpp
```

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp` containing only the include guard and namespace, and an empty `.cpp` including it.

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 2>&1 | grep -E "error" | head`
Expected: compile errors — `facet_component_ids` not declared.

- [ ] **Step 3: Implement**

`src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp
#pragma once
#include <array>
#include <vector>

#include <Eigen/Dense>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "OrcaMCPPaintGeometry.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// ---- Connected components -------------------------------------------------------------------
//
// A facet's component id, one per facet in facet order. Two facets share a component when a path
// of shared edges joins them (its_face_neighbors_par, TriangleMesh.hpp:201; a negative neighbour
// is an open edge). Ids are assigned in discovery order over ascending facet index, so the same
// mesh always yields the same ids: an id read from get_object_components addresses the same shell
// in a later paint_object call. `component_count` receives the number of ids handed out.
std::vector<int> facet_component_ids(const indexed_triangle_set& its, int& component_count);

struct ComponentInfo
{
    int           component   = 0;
    int           facet_count = 0;
    // Surface area in the frame `to_plate` maps into -- plate mm^2 for the callers here.
    double        area        = 0.0;
    BoundingBoxf3 bbox;
};

// One entry per component, ascending by component id, each with its facet count, area and
// bounding box computed from the facets' plate-frame vertices.
std::vector<ComponentInfo> summarize_components(const indexed_triangle_set& its,
                                                const std::vector<int>&     ids,
                                                int                         component_count,
                                                const Transform3d&          to_plate);

// `state` for every facet whose id is `component`, -1 (leave alone) for every other facet. An id
// no facet carries selects nothing -- reported through `unassigned`, never widened to "everything".
FacetAssignment assign_component(const std::vector<int>& ids, int component, int state);

}}} // namespace Slic3r::GUI::OrcaMCP
```

`src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp
#include "OrcaMCPPaintSelect.hpp"

#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

std::vector<int> facet_component_ids(const indexed_triangle_set& its, int& component_count)
{
    component_count = 0;
    std::vector<int> ids(its.indices.size(), -1);
    if (its.indices.empty())
        return ids;

    const std::vector<Vec3i32> neighbors = its_face_neighbors_par(its);
    std::vector<int>           stack;
    for (std::size_t seed = 0; seed < ids.size(); ++seed) {
        if (ids[seed] != -1)
            continue;
        const int id = component_count++;
        ids[seed]    = id;
        stack.push_back(int(seed));
        // Iterative flood fill: a 4-million-facet shell would overflow a recursive one.
        while (!stack.empty()) {
            const int facet = stack.back();
            stack.pop_back();
            for (int neighbor : neighbors[facet])
                if (neighbor >= 0 && ids[neighbor] == -1) {
                    ids[neighbor] = id;
                    stack.push_back(neighbor);
                }
        }
    }
    return ids;
}

std::vector<ComponentInfo> summarize_components(const indexed_triangle_set& its,
                                                const std::vector<int>&     ids,
                                                int                         component_count,
                                                const Transform3d&          to_plate)
{
    std::vector<ComponentInfo> out(std::size_t(std::max(component_count, 0)));
    for (int c = 0; c < component_count; ++c)
        out[c].component = c;

    for (std::size_t f = 0; f < its.indices.size() && f < ids.size(); ++f) {
        const int id = ids[f];
        if (id < 0 || id >= component_count)
            continue;
        const Vec3i32& face = its.indices[f];
        const Vec3d a = to_plate * its.vertices[face[0]].cast<double>();
        const Vec3d b = to_plate * its.vertices[face[1]].cast<double>();
        const Vec3d c = to_plate * its.vertices[face[2]].cast<double>();
        ComponentInfo& info = out[id];
        info.facet_count += 1;
        info.area += 0.5 * (b - a).cross(c - a).norm();
        info.bbox.merge(a);
        info.bbox.merge(b);
        info.bbox.merge(c);
    }
    return out;
}

FacetAssignment assign_component(const std::vector<int>& ids, int component, int state)
{
    FacetAssignment out;
    out.states.assign(ids.size(), -1);
    out.unassigned = int(ids.size());
    for (std::size_t f = 0; f < ids.size(); ++f)
        if (ids[f] == component) {
            out.states[f] = state;
            --out.unassigned;
        }
    return out;
}

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][select]"`
Expected: `All tests passed (…)` — 5 cases.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/CMakeLists.txt tests/slic3rutils/CMakeLists.txt \
        src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp \
        tests/slic3rutils/test_paint_select.cpp
git commit -m "feat: a mesh's shells can be told apart, so a bag that is its own shell can be addressed by number"
```

---

### Task 2: Picking — nearest surface point and first ray hit

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp` (append)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp` (append)
- Test: `tests/slic3rutils/test_paint_select.cpp` (append)

**Interfaces:**
- Produces:
  ```cpp
  struct SurfacePick { int facet; Vec3d point_local; Vec3d point_plate; Vec3d normal_plate; double distance; };
  Vec3d facet_normal_plate(const indexed_triangle_set& its, int facet, const Transform3d& to_plate);
  bool pick_nearest_point(const TriangleMesh& mesh, const Transform3d& to_plate, const Vec3d& plate_point, SurfacePick& out);
  bool pick_ray(const TriangleMesh& mesh, const Transform3d& to_plate, const Vec3d& origin_plate, const Vec3d& dir_plate, SurfacePick& out);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/slic3rutils/test_paint_select.cpp`:

```cpp
TEST_CASE("pick_nearest_point snaps a plate point to the closest facet, in plate coordinates",
          "[orcamcp][select]")
{
    // A 10 mm cube whose mesh-local origin is at a corner, placed with its min corner at plate
    // (100, 100, 0) via the transform -- exactly how volume_to_plate positions a volume.
    const TriangleMesh mesh(Slic3r::its_make_cube(10.0, 10.0, 10.0));
    Transform3d to_plate = Transform3d::Identity();
    to_plate.translation() = Vec3d(100.0, 100.0, 0.0);

    SurfacePick pick;
    // 3 mm above the middle of the top face.
    REQUIRE(pick_nearest_point(mesh, to_plate, Vec3d(105.0, 105.0, 13.0), pick));
    CHECK(pick.facet >= 0);
    CHECK(pick.facet < int(mesh.its.indices.size()));
    CHECK_THAT(pick.point_plate.x(), WithinAbs(105.0, 1e-6));
    CHECK_THAT(pick.point_plate.y(), WithinAbs(105.0, 1e-6));
    CHECK_THAT(pick.point_plate.z(), WithinAbs(10.0, 1e-6));       // on the top face
    CHECK_THAT(pick.distance, WithinAbs(3.0, 1e-6));
    CHECK_THAT(pick.normal_plate.z(), WithinAbs(1.0, 1e-6));       // top face points +Z
    // point_local is the same point before the transform.
    CHECK_THAT(pick.point_local.x(), WithinAbs(5.0, 1e-6));
    CHECK_THAT(pick.point_local.z(), WithinAbs(10.0, 1e-6));
}

TEST_CASE("pick_nearest_point measures distance in plate millimetres under a scaled instance",
          "[orcamcp][select]")
{
    const TriangleMesh mesh(Slic3r::its_make_cube(10.0, 10.0, 10.0));
    Transform3d to_plate = Transform3d::Identity();
    to_plate.scale(2.0);                                            // the cube is 20 mm on the plate

    SurfacePick pick;
    REQUIRE(pick_nearest_point(mesh, to_plate, Vec3d(10.0, 10.0, 25.0), pick));
    CHECK_THAT(pick.point_plate.z(), WithinAbs(20.0, 1e-6));
    CHECK_THAT(pick.distance, WithinAbs(5.0, 1e-6));                // 25 - 20, in plate mm
}

TEST_CASE("pick_nearest_point refuses an empty mesh", "[orcamcp][select]")
{
    const TriangleMesh empty;
    SurfacePick        pick;
    CHECK_FALSE(pick_nearest_point(empty, Transform3d::Identity(), Vec3d::Zero(), pick));
    CHECK(pick.facet == -1);
}

TEST_CASE("pick_ray returns the first surface a plate-frame ray hits", "[orcamcp][select]")
{
    const TriangleMesh mesh(Slic3r::its_make_cube(10.0, 10.0, 10.0));
    Transform3d to_plate = Transform3d::Identity();
    to_plate.translation() = Vec3d(100.0, 100.0, 0.0);

    SurfacePick pick;
    // From high above the cube's centre, straight down: hits the top face at z = 10, not the
    // bottom face at z = 0 behind it.
    REQUIRE(pick_ray(mesh, to_plate, Vec3d(105.0, 105.0, 50.0), Vec3d(0.0, 0.0, -1.0), pick));
    CHECK_THAT(pick.point_plate.z(), WithinAbs(10.0, 1e-6));
    CHECK_THAT(pick.distance, WithinAbs(40.0, 1e-6));
    CHECK_THAT(pick.normal_plate.z(), WithinAbs(1.0, 1e-6));

    // A ray that misses.
    SurfacePick miss;
    CHECK_FALSE(pick_ray(mesh, to_plate, Vec3d(200.0, 200.0, 50.0), Vec3d(0.0, 0.0, -1.0), miss));
    CHECK(miss.facet == -1);

    // A zero direction is a caller mistake, not a hit.
    SurfacePick zero;
    CHECK_FALSE(pick_ray(mesh, to_plate, Vec3d(105.0, 105.0, 50.0), Vec3d::Zero(), zero));
}
```

- [ ] **Step 2: Run to see them fail**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 2>&1 | grep -E "error" | head -3`
Expected: `SurfacePick` / `pick_nearest_point` not declared.

- [ ] **Step 3: Implement**

Append to the header, inside the namespace:

```cpp
// ---- Picking --------------------------------------------------------------------------------

// Where a point or a ray met the surface. `facet` is the original facet index -- the index
// TriangleSelector::set_facet and seed_fill_select_triangles take -- or -1 when nothing was hit.
struct SurfacePick
{
    int    facet        = -1;
    Vec3d  point_local  = Vec3d::Zero();   // mesh coordinates, the frame TriangleSelector wants
    Vec3d  point_plate  = Vec3d::Zero();   // the same point in plate millimetres
    Vec3d  normal_plate = Vec3d::Zero();   // unit facet normal in the plate frame
    double distance     = 0.0;             // plate mm from the query point / ray origin
};

// Unit normal of `facet`, mapped into the plate frame by the inverse-transpose of `to_plate`'s
// linear part, so a non-uniformly scaled instance still reports a normal perpendicular to the
// surface it actually has on the plate.
Vec3d facet_normal_plate(const indexed_triangle_set& its, int facet, const Transform3d& to_plate);

// Closest point on the mesh to `plate_point`. Builds an AABBMesh over the mesh for this call;
// on a multi-million-facet mesh that is the dominant cost, and it is pure CPU work the caller
// may run off the GUI thread. False for an empty mesh.
bool pick_nearest_point(const TriangleMesh& mesh,
                        const Transform3d&  to_plate,
                        const Vec3d&        plate_point,
                        SurfacePick&        out);

// First hit of the ray `origin_plate + t * dir_plate`, t > 0. False for an empty mesh, a zero
// direction, or a miss.
bool pick_ray(const TriangleMesh& mesh,
              const Transform3d&  to_plate,
              const Vec3d&        origin_plate,
              const Vec3d&        dir_plate,
              SurfacePick&        out);
```

Append to the `.cpp` (add `#include "libslic3r/AABBMesh.hpp"` at the top):

```cpp
Vec3d facet_normal_plate(const indexed_triangle_set& its, int facet, const Transform3d& to_plate)
{
    if (facet < 0 || facet >= int(its.indices.size()))
        return Vec3d::Zero();
    const Vec3i32& face = its.indices[facet];
    const Vec3d a = its.vertices[face[0]].cast<double>();
    const Vec3d b = its.vertices[face[1]].cast<double>();
    const Vec3d c = its.vertices[face[2]].cast<double>();
    const Vec3d n_local = (b - a).cross(c - a);
    if (n_local.squaredNorm() == 0.0)
        return Vec3d::Zero();
    // Normals transform by the inverse transpose of the linear part, not by the matrix itself.
    const Eigen::Matrix3d normal_matrix = to_plate.linear().inverse().transpose();
    return (normal_matrix * n_local).normalized();
}

bool pick_nearest_point(const TriangleMesh& mesh,
                        const Transform3d&  to_plate,
                        const Vec3d&        plate_point,
                        SurfacePick&        out)
{
    out = SurfacePick{};
    if (mesh.its.indices.empty())
        return false;

    const AABBMesh aabb(mesh.its);
    const Vec3d    local_point = to_plate.inverse() * plate_point;
    int            face        = -1;
    Vec3d          closest     = Vec3d::Zero();
    aabb.squared_distance(local_point, face, closest);
    if (face < 0 || face >= int(mesh.its.indices.size()))
        return false;

    out.facet        = face;
    out.point_local  = closest;
    out.point_plate  = to_plate * closest;
    out.normal_plate = facet_normal_plate(mesh.its, face, to_plate);
    // Measured in the plate frame, so a scaled instance reports millimetres the caller can use.
    out.distance     = (out.point_plate - plate_point).norm();
    return true;
}

bool pick_ray(const TriangleMesh& mesh,
              const Transform3d&  to_plate,
              const Vec3d&        origin_plate,
              const Vec3d&        dir_plate,
              SurfacePick&        out)
{
    out = SurfacePick{};
    if (mesh.its.indices.empty() || dir_plate.squaredNorm() == 0.0)
        return false;

    const AABBMesh    aabb(mesh.its);
    const Transform3d to_local = to_plate.inverse();
    const Vec3d       origin   = to_local * origin_plate;
    const Vec3d       dir      = (to_local.linear() * dir_plate).normalized();
    const AABBMesh::hit_result hit = aabb.query_ray_hit(origin, dir);
    if (!hit.is_hit())
        return false;

    out.facet        = hit.face();
    out.point_local  = hit.position();
    out.point_plate  = to_plate * out.point_local;
    out.normal_plate = facet_normal_plate(mesh.its, out.facet, to_plate);
    out.distance     = (out.point_plate - origin_plate).norm();
    return true;
}
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][select]"`
Expected: all pass — 9 cases.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp tests/slic3rutils/test_paint_select.cpp
git commit -m "feat: a plate point or a ray can be turned into the facet it lands on"
```

---

### Task 3: Pixel → ray unprojection from a render's camera

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp` (append)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp` (append)
- Test: `tests/slic3rutils/test_paint_select.cpp` (append)

**Interfaces:**
- Produces:
  ```cpp
  struct CameraFrame { Eigen::Matrix4d view; Eigen::Matrix4d projection; std::array<int,4> viewport; };
  bool unproject_pixel_to_ray(const CameraFrame& camera, double px, double py, Vec3d& origin, Vec3d& dir);
  ```
- Consumed by Task 8 (`pick_facet` with `pixel` + `camera`) and Task 10 (`render_plate_view` emits the same three fields).

- [ ] **Step 1: Write the failing tests**

Append to `tests/slic3rutils/test_paint_select.cpp`:

```cpp
namespace {

// A camera at (0, 0, 100) looking down -Z with +Y up, 90-degree vertical FOV, 1:1 aspect,
// near 1, far 1000, over a 200 x 200 viewport. Built by hand so the expected rays are hand-checkable.
CameraFrame test_camera()
{
    CameraFrame cam;
    cam.view = Eigen::Matrix4d::Identity();
    cam.view(2, 3) = -100.0;                       // translate world so the eye is at the origin
    const double f = 1.0;                          // cot(45 deg)
    const double n = 1.0, fa = 1000.0;
    cam.projection = Eigen::Matrix4d::Zero();
    cam.projection(0, 0) = f;
    cam.projection(1, 1) = f;
    cam.projection(2, 2) = (fa + n) / (n - fa);
    cam.projection(2, 3) = 2.0 * fa * n / (n - fa);
    cam.projection(3, 2) = -1.0;
    cam.viewport = {0, 0, 200, 200};
    return cam;
}

} // namespace

TEST_CASE("unproject_pixel_to_ray sends the centre pixel straight down the view axis",
          "[orcamcp][select]")
{
    Vec3d origin, dir;
    REQUIRE(unproject_pixel_to_ray(test_camera(), 100.0, 100.0, origin, dir));
    CHECK_THAT(origin.x(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(origin.y(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(dir.x(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(dir.y(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(dir.z(), WithinAbs(-1.0, 1e-6));
    CHECK_THAT(dir.norm(), WithinAbs(1.0, 1e-9));
}

TEST_CASE("unproject_pixel_to_ray treats pixel row 0 as the top of the image", "[orcamcp][select]")
{
    // Top-centre pixel: with +Y up in the view, the ray must tilt towards +Y.
    Vec3d origin, dir;
    REQUIRE(unproject_pixel_to_ray(test_camera(), 100.0, 0.0, origin, dir));
    CHECK(dir.y() > 0.5);
    CHECK(dir.z() < 0.0);

    // Right-centre pixel tilts towards +X.
    REQUIRE(unproject_pixel_to_ray(test_camera(), 200.0, 100.0, origin, dir));
    CHECK(dir.x() > 0.5);
}

TEST_CASE("unproject_pixel_to_ray refuses a degenerate viewport or a singular camera",
          "[orcamcp][select]")
{
    CameraFrame bad = test_camera();
    bad.viewport = {0, 0, 0, 200};
    Vec3d origin, dir;
    CHECK_FALSE(unproject_pixel_to_ray(bad, 0.0, 0.0, origin, dir));

    CameraFrame singular = test_camera();
    singular.projection = Eigen::Matrix4d::Zero();
    CHECK_FALSE(unproject_pixel_to_ray(singular, 100.0, 100.0, origin, dir));
}
```

- [ ] **Step 2: Run to see them fail**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 2>&1 | grep -E "error" | head -3`
Expected: `CameraFrame` not declared.

- [ ] **Step 3: Implement**

Append to the header:

```cpp
// ---- Camera ---------------------------------------------------------------------------------

// What render_plate_view reports per view and pick_facet takes back: the two 4x4 matrices the
// thumbnail was drawn with and the viewport {x, y, width, height} in pixels. Pixel (0, 0) is the
// TOP-left of the saved image: OrcaMCPPlateUtils.cpp:44,63 flip the GL buffer row-wise when
// writing it, so the convention here is the image's, not OpenGL's.
struct CameraFrame
{
    Eigen::Matrix4d    view       = Eigen::Matrix4d::Identity();
    Eigen::Matrix4d    projection = Eigen::Matrix4d::Identity();
    std::array<int, 4> viewport   = {0, 0, 1, 1};
};

// The world-space ray through pixel (px, py). `origin` lies on the near plane and `dir` is a unit
// vector. False when the viewport has no area or projection * view cannot be inverted.
bool unproject_pixel_to_ray(const CameraFrame& camera, double px, double py, Vec3d& origin, Vec3d& dir);
```

Append to the `.cpp`:

```cpp
bool unproject_pixel_to_ray(const CameraFrame& camera, double px, double py, Vec3d& origin, Vec3d& dir)
{
    const int vx = camera.viewport[0], vy = camera.viewport[1];
    const int vw = camera.viewport[2], vh = camera.viewport[3];
    if (vw <= 0 || vh <= 0)
        return false;

    const Eigen::Matrix4d pv = camera.projection * camera.view;
    Eigen::FullPivLU<Eigen::Matrix4d> lu(pv);
    if (!lu.isInvertible())
        return false;
    const Eigen::Matrix4d inv = lu.inverse();

    // Normalised device coordinates. Image row 0 is the top, so y is flipped relative to NDC.
    const double nx = 2.0 * (px - vx) / vw - 1.0;
    const double ny = 1.0 - 2.0 * (py - vy) / vh;

    auto unproject = [&inv](double x, double y, double z, Vec3d& out) -> bool {
        const Eigen::Vector4d clip(x, y, z, 1.0);
        const Eigen::Vector4d world = inv * clip;
        if (std::abs(world.w()) < 1e-12)
            return false;
        out = world.head<3>() / world.w();
        return true;
    };

    Vec3d near_point, far_point;
    if (!unproject(nx, ny, -1.0, near_point) || !unproject(nx, ny, 1.0, far_point))
        return false;
    const Vec3d d = far_point - near_point;
    if (d.squaredNorm() == 0.0)
        return false;

    origin = near_point;
    dir    = d.normalized();
    return true;
}
```

Add `#include <cmath>` at the top of the `.cpp`.

- [ ] **Step 4: Build and run**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][select]"`
Expected: all pass — 12 cases.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp tests/slic3rutils/test_paint_select.cpp
git commit -m "feat: a pixel in a render can be turned back into the ray that drew it"
```

---

### Task 4: Seed fill — a connected region from a seed facet

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp` (append)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp` (append)
- Test: `tests/slic3rutils/test_paint_select.cpp` (append)

**Interfaces:**
- Produces:
  ```cpp
  constexpr double kDefaultSeedFillAngleDeg = 30.0;
  FacetAssignment assign_connected(const TriangleMesh& mesh, const Transform3d& to_plate, int seed_facet, const Vec3f& seed_point_local, double angle_deg, int state);
  ```
- Consumes `SurfacePick` from Task 2 for the seed (`facet`, `point_local`).

- [ ] **Step 1: Write the failing tests**

Append to `tests/slic3rutils/test_paint_select.cpp`:

```cpp
namespace {

Vec3f facet_centroid_local(const indexed_triangle_set& its, int facet)
{
    const Vec3i32& f = its.indices[facet];
    return (its.vertices[f[0]] + its.vertices[f[1]] + its.vertices[f[2]]) / 3.f;
}

} // namespace

TEST_CASE("assign_connected at the gizmo's default angle fills one face of a cube and stops at its edges",
          "[orcamcp][select]")
{
    // its_make_cube: 12 facets, two per face, adjacent faces meet at 90 degrees.
    const TriangleMesh mesh(Slic3r::its_make_cube(10.0, 10.0, 10.0));
    const int          seed = 0;

    const FacetAssignment a = assign_connected(mesh, Transform3d::Identity(), seed,
                                               facet_centroid_local(mesh.its, seed),
                                               kDefaultSeedFillAngleDeg, 5);
    REQUIRE(a.states.size() == 12);
    CHECK(a.states[seed] == 5);
    // A 90-degree edge is sharper than 30 degrees, so the fill covers only the seed's own face:
    // the seed facet and the coplanar facet sharing its diagonal.
    const int painted = int(std::count(a.states.begin(), a.states.end(), 5));
    CHECK(painted == 2);
    CHECK(a.unassigned == 10);
    CHECK(a.band_counts.empty());
}

TEST_CASE("assign_connected with an angle wider than the fold covers the whole cube", "[orcamcp][select]")
{
    const TriangleMesh mesh(Slic3r::its_make_cube(10.0, 10.0, 10.0));
    const FacetAssignment a = assign_connected(mesh, Transform3d::Identity(), 0,
                                               facet_centroid_local(mesh.its, 0), 100.0, 5);
    CHECK(std::all_of(a.states.begin(), a.states.end(), [](int s) { return s == 5; }));
    CHECK(a.unassigned == 0);
}

TEST_CASE("assign_connected does not leak across a gap between two shells", "[orcamcp][select]")
{
    const TriangleMesh mesh(two_cubes());
    // Seed in cube B (facets 12..23) with a permissive angle: fills all of B, none of A.
    const FacetAssignment a = assign_connected(mesh, Transform3d::Identity(), 12,
                                               facet_centroid_local(mesh.its, 12), 179.0, 3);
    for (std::size_t i = 0; i < 12; ++i)
        CHECK(a.states[i] == -1);
    for (std::size_t i = 12; i < 24; ++i)
        CHECK(a.states[i] == 3);
}

TEST_CASE("assign_connected refuses a seed facet the mesh does not have", "[orcamcp][select]")
{
    const TriangleMesh mesh(Slic3r::its_make_cube(10.0, 10.0, 10.0));
    const FacetAssignment a = assign_connected(mesh, Transform3d::Identity(), 12, Vec3f::Zero(),
                                               kDefaultSeedFillAngleDeg, 5);
    CHECK(std::all_of(a.states.begin(), a.states.end(), [](int s) { return s == -1; }));
    CHECK(a.unassigned == 12);

    const TriangleMesh empty;
    const FacetAssignment none = assign_connected(empty, Transform3d::Identity(), 0, Vec3f::Zero(),
                                                  kDefaultSeedFillAngleDeg, 5);
    CHECK(none.states.empty());
    CHECK(none.unassigned == 0);
}
```

- [ ] **Step 2: Run to see them fail**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 2>&1 | grep -E "error" | head -3`
Expected: `assign_connected` not declared.

- [ ] **Step 3: Implement**

Append to the header (add `#include "libslic3r/TriangleSelector.hpp"` at the top):

```cpp
// ---- Seed fill ------------------------------------------------------------------------------

// The gizmo's own default (GLGizmoPainterBase.hpp:279, m_smart_fill_angle = 30.f).
constexpr double kDefaultSeedFillAngleDeg = 30.0;

// The connected region reachable from `seed_facet` without crossing an edge whose dihedral angle
// exceeds `angle_deg` -- TriangleSelector::seed_fill_select_triangles (TriangleSelector.hpp:332),
// the same traversal the GUI's smart-fill mode runs, read back per original facet. `state` is
// written to every facet the fill reached, -1 to every other. `seed_point_local` is the mesh-local
// point the fill starts from (SurfacePick::point_local); it need only lie on `seed_facet`.
// Out-of-range `seed_facet`, or an empty mesh, selects nothing.
FacetAssignment assign_connected(const TriangleMesh& mesh,
                                 const Transform3d&  to_plate,
                                 int                 seed_facet,
                                 const Vec3f&        seed_point_local,
                                 double              angle_deg,
                                 int                 state);
```

Append to the `.cpp`:

```cpp
namespace {

// TriangleSelector keeps its per-triangle seed-fill flag protected. This reads it back for the
// original (unsplit) facets, which is all a fresh selector has: nothing is split until something
// paints with a cursor, and this code never does. The same subclassing move the GUI makes
// (TriangleSelectorGUI, GLGizmoPainterBase.hpp:33).
class SeedFillReader : public TriangleSelector
{
public:
    explicit SeedFillReader(const TriangleMesh& mesh) : TriangleSelector(mesh) {}

    std::vector<int> selected_original_facets() const
    {
        std::vector<int> out;
        for (int i = 0; i < m_orig_size_indices; ++i)
            if (!m_triangles[i].is_split() && m_triangles[i].is_selected_by_seed_fill())
                out.push_back(i);
        return out;
    }
};

} // namespace

FacetAssignment assign_connected(const TriangleMesh& mesh,
                                 const Transform3d&  to_plate,
                                 int                 seed_facet,
                                 const Vec3f&        seed_point_local,
                                 double              angle_deg,
                                 int                 state)
{
    FacetAssignment out;
    out.states.assign(mesh.its.indices.size(), -1);
    out.unassigned = int(out.states.size());
    if (mesh.its.indices.empty() || seed_facet < 0 || seed_facet >= int(mesh.its.indices.size()))
        return out;

    SeedFillReader selector(mesh);
    // The fill only reads the transform when painting overhangs only (highlight_by_angle_deg
    // != 0), which this never does; passed anyway, without translation, as the signature asks.
    Transform3d no_translate = to_plate;
    no_translate.translation() = Vec3d::Zero();
    // force_reselection = true: a fresh selector has nothing selected, but the early-return in
    // seed_fill_select_triangles is keyed on the start facet's flag, and asking for a recompute
    // is what a one-shot call means.
    selector.seed_fill_select_triangles(seed_point_local, seed_facet, no_translate,
                                        TriangleSelector::ClippingPlane(), float(angle_deg), 0.f, true);

    for (int facet : selector.selected_original_facets()) {
        out.states[facet] = state;
        --out.unassigned;
    }
    return out;
}
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][select]"`
Expected: all pass — 16 cases. If the 30-degree case paints more than 2 facets, check that `its_make_cube` produces coplanar pairs per face (it does: 12 facets, 6 faces) and that `angle_deg` reached the selector as degrees, not radians.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp tests/slic3rutils/test_paint_select.cpp
git commit -m "feat: the gizmo's smart fill can be driven from a facet instead of a mouse"
```

---

### Task 5: Parallel centroids, and no centroids for `selection: "all"`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp:92-105`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp:716-730` (the per-volume loop — this task's edit is superseded by Task 6's rewrite of the same loop; make it here anyway so the change is reviewable on its own)
- Test: `tests/slic3rutils/test_paint_geometry.cpp` (append)

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_paint_geometry.cpp`:

```cpp
TEST_CASE("facet_centroids gives the same answer for a mesh large enough to be split across threads",
          "[orcamcp][paint]")
{
    // Big enough that tbb::parallel_for actually partitions it. The serial reference is computed
    // inline so the test does not depend on the implementation staying serial.
    indexed_triangle_set its = Slic3r::its_make_sphere(20.0, 0.02);
    REQUIRE(its.indices.size() > 10000);
    Transform3d to_plate = Transform3d::Identity();
    to_plate.translation() = Vec3d(50.0, 60.0, 70.0);

    const std::vector<Vec3d> got = facet_centroids(its, to_plate);
    REQUIRE(got.size() == its.indices.size());
    for (std::size_t i = 0; i < its.indices.size(); i += 997) {
        const Vec3i32& f = its.indices[i];
        const Vec3d expected = to_plate * ((its.vertices[f[0]].cast<double>() +
                                            its.vertices[f[1]].cast<double>() +
                                            its.vertices[f[2]].cast<double>()) / 3.0);
        CHECK_THAT((got[i] - expected).norm(), WithinAbs(0.0, 1e-9));
    }
}
```

- [ ] **Step 2: Run it — it passes against the serial version. That is expected: this test pins behaviour, the change is performance.**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][paint]"`
Expected: pass.

- [ ] **Step 3: Parallelise `facet_centroids`**

Replace `OrcaMCPPaintGeometry.cpp:92-105` with:

```cpp
std::vector<Vec3d> facet_centroids(const indexed_triangle_set& its, const Transform3d& to_plate)
{
    std::vector<Vec3d> centroids(its.indices.size());
    // One matrix multiply per facet, embarrassingly parallel. On the 4.3-million-facet meshes a
    // generated figurine arrives with (T10), the serial loop was a measurable part of a
    // three-minute paint; the idiom is CutSurface.cpp:811-818.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, its.indices.size()),
        [&its, &to_plate, &centroids](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                const Vec3i32& face = its.indices[i];
                const Vec3d a = its.vertices[face[0]].cast<double>();
                const Vec3d b = its.vertices[face[1]].cast<double>();
                const Vec3d c = its.vertices[face[2]].cast<double>();
                // Transform the centroid rather than the three vertices: the transform is affine,
                // so the two agree, and this is one matrix multiply per facet instead of three.
                centroids[i] = to_plate * ((a + b + c) / 3.0);
            }
        });
    return centroids;
}
```

Add `#include <tbb/parallel_for.h>` to the includes of `OrcaMCPPaintGeometry.cpp`.

- [ ] **Step 4: Skip centroids for `"all"`**

In `OrcaMCPPaintTools.cpp`, inside the per-volume loop of `paint_object` (currently `:716-730`), change:

```cpp
                    const std::vector<Slic3r::Vec3d> centroids = facet_centroids(mv->mesh().its, to_plate);
                    original_facets_total += int(centroids.size());

                    FacetAssignment assignment;
                    if (request.selection == "bands")
                        assignment = assign_bands(centroids, request.axis, request.bands);
                    else if (request.selection == "box")
                        assignment = assign_box(centroids, request.box, request.state);
                    else if (request.selection == "sphere")
                        assignment = assign_sphere(centroids, request.sphere, request.state);
                    else
                        assignment = assign_all(centroids.size(), request.state);
```

to:

```cpp
                    const std::size_t facet_count = mv->mesh().its.indices.size();
                    original_facets_total += int(facet_count);

                    FacetAssignment assignment;
                    if (request.selection == "all") {
                        // Every facet, no geometry: computing 4 million centroids to then ignore
                        // them was the whole-branch review's M11.
                        assignment = assign_all(facet_count, request.state);
                    } else {
                        const std::vector<Slic3r::Vec3d> centroids = facet_centroids(mv->mesh().its, to_plate);
                        if (request.selection == "bands")
                            assignment = assign_bands(centroids, request.axis, request.bands);
                        else if (request.selection == "box")
                            assignment = assign_box(centroids, request.box, request.state);
                        else
                            assignment = assign_sphere(centroids, request.sphere, request.state);
                    }
```

- [ ] **Step 5: Build both targets, run the full suite**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8 && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests | tail -3`
Expected: 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp tests/slic3rutils/test_paint_geometry.cpp
git commit -m "fix: paint_object computed four million centroids on one core, and threw them away for selection all"
```

---

### Task 6: `paint_object` in three hops — geometry off the GUI thread

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp` — the anonymous-namespace helpers (add after `refresh_after_paint`, `:289`) and the `paint_object` handler lambda (`:676` to the end of that `register_tool` call)

**Interfaces:**
- Produces (file-local, reused by Tasks 7–9):
  ```cpp
  struct PaintPlanVolume { int index; int volume_id; std::string name; std::shared_ptr<const Slic3r::TriangleMesh> mesh; Slic3r::Transform3d to_plate; Slic3r::ObjectID volume_identity; };
  struct PaintPlan { int object_id; std::size_t instance_idx; Slic3r::ObjectID object_identity; std::vector<PaintPlanVolume> volumes; };
  PaintPlan capture_paint_plan(const PaintTarget& target);                      // main thread
  bool plan_still_valid(const PaintTarget& target, const PaintPlan& plan, std::string& error); // main thread
  ```

This task changes no behaviour a test can see; it changes *where* the work runs. The proof is Task 12's responsiveness check.

- [ ] **Step 1: Add the plan helpers**

After `refresh_after_paint` (`:289`), add:

```cpp
// What a worker thread needs to do a volume's geometry with no Model in reach: the mesh, which
// ModelVolume holds as a shared_ptr<const TriangleMesh> (Model.hpp:861) and never mutates in
// place, the transform that puts it on the plate, and enough identity to check on return that the
// scene is still the one the plan was made from.
struct PaintPlanVolume
{
    int                                          index     = 0;   // position in PaintTarget::volumes
    int                                          volume_id = -1;  // caller-facing id
    std::string                                  name;
    std::shared_ptr<const Slic3r::TriangleMesh>  mesh;
    Slic3r::Transform3d                          to_plate = Slic3r::Transform3d::Identity();
    Slic3r::ObjectID                             volume_identity;
};

struct PaintPlan
{
    int                          object_id    = -1;
    std::size_t                  instance_idx = 0;
    Slic3r::ObjectID             object_identity;
    std::vector<PaintPlanVolume> volumes;
};

// Main thread only: snapshot the immutable parts of a resolved target so the geometry can run
// on the HTTP worker thread. Copying a shared_ptr is the whole cost.
PaintPlan capture_paint_plan(const PaintTarget& target)
{
    PaintPlan plan;
    plan.object_id       = target.object_id;
    plan.instance_idx    = target.instance_idx;
    plan.object_identity = target.object->id();
    for (std::size_t i = 0; i < target.volumes.size(); ++i) {
        Slic3r::ModelVolume* mv = target.volumes[i];
        plan.volumes.push_back({int(i), target.volume_ids[i], mv->name, mv->mesh_ptr(),
                                volume_to_plate(*target.object, *mv, target.instance_idx), mv->id()});
    }
    return plan;
}

// Main thread only: true when `target`, freshly re-resolved, is still the scene `plan` was made
// from. The mesh pointer is the strongest check -- any edit that touches geometry (cut, split,
// simplify, a re-import) replaces the shared mesh rather than mutating it, so a stale pointer
// means the states computed on the worker no longer line up with the facets they would be
// written to. Writing them anyway would paint the wrong triangles and look deliberate.
bool plan_still_valid(const PaintTarget& target, const PaintPlan& plan, std::string& error)
{
    if (target.object == nullptr || target.object->id() != plan.object_identity ||
        target.volumes.size() != plan.volumes.size()) {
        error = "the scene changed while the selection was being computed (object " +
                std::to_string(plan.object_id) + " is not the one the call started on); retry";
        return false;
    }
    for (const PaintPlanVolume& pv : plan.volumes) {
        Slic3r::ModelVolume* mv = target.volumes[std::size_t(pv.index)];
        if (mv->id() != pv.volume_identity || mv->mesh_ptr().get() != pv.mesh.get()) {
            error = "the scene changed while the selection was being computed (volume " +
                    std::to_string(pv.volume_id) + "'s mesh was replaced); retry";
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 2: Rewrite the `paint_object` handler as three hops**

Replace the handler lambda of the `paint_object` `register_tool` (from `[](const nlohmann::json& params) -> nlohmann::json {` at `:676` to the matching `}` that closes the lambda) with:

```cpp
        [](const nlohmann::json& params) -> nlohmann::json {
            // Hop 1 -- main thread: validate everything, take nothing, capture the plan.
            PaintPlan    plan;
            PaintRequest request;
            PaintMode    mode = PaintMode::Color;
            {
                nlohmann::json gate = run_on_main_thread([&params, &plan, &request, &mode]() -> nlohmann::json {
                    PaintTarget target;
                    std::string error;
                    if (!resolve_paint_target(params, target, error))
                        return nlohmann::json{{"status", "error"}, {"message", error}};

                    if (params.contains("mode") &&
                        !(params["mode"].is_string() && parse_paint_mode(params["mode"].get<std::string>(), mode)))
                        return nlohmann::json{{"status", "error"},
                                              {"message", "Unknown mode; expected color, support, seam or fuzzy_skin"}};

                    if (!parse_paint_request(params, mode, target, request, error))
                        return nlohmann::json{{"status", "error"}, {"message", error}};

                    for (std::size_t i = 0; i < target.volumes.size(); ++i)
                        if (target.volumes[i]->mesh().its.indices.empty())
                            return nlohmann::json{{"status", "error"},
                                                  {"message", "volume_id " + std::to_string(target.volume_ids[i]) +
                                                              " has an empty mesh, so it has no facets to paint"}};

                    plan = capture_paint_plan(target);
                    return nlohmann::json{{"status", "success"}};
                });
                if (gate.value("status", "") != "success")
                    return gate;
            }

            // Hop 2 -- this HTTP worker thread: the geometry. Nothing here touches the Model; the
            // meshes are shared_ptr<const> snapshots and the transforms are copies. A three-minute
            // paint on a 4-million-facet mesh (T10) now leaves the GUI thread free to answer every
            // other tool.
            std::vector<FacetAssignment> assignments(plan.volumes.size());
            for (std::size_t i = 0; i < plan.volumes.size(); ++i) {
                const PaintPlanVolume& pv          = plan.volumes[i];
                const std::size_t      facet_count = pv.mesh->its.indices.size();
                if (request.selection == "all") {
                    assignments[i] = assign_all(facet_count, request.state);
                } else {
                    const std::vector<Slic3r::Vec3d> centroids = facet_centroids(pv.mesh->its, pv.to_plate);
                    if (request.selection == "bands")
                        assignments[i] = assign_bands(centroids, request.axis, request.bands);
                    else if (request.selection == "box")
                        assignments[i] = assign_box(centroids, request.box, request.state);
                    else
                        assignments[i] = assign_sphere(centroids, request.sphere, request.state);
                }
            }

            // Hop 3 -- main thread: confirm the scene is unchanged, then snapshot, write, refresh.
            return run_on_main_thread([&params, &plan, &request, &mode, &assignments]() -> nlohmann::json {
                Plater*     plater = wxGetApp().plater();
                PaintTarget target;
                std::string error;
                if (!resolve_paint_target(params, target, error) || !plan_still_valid(target, plan, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                plater->take_snapshot(_u8L("Paint Object") + " (" + paint_mode_name(mode) + ")");

                int              facets_selected       = 0;
                int              original_facets_total = 0;
                int              facets_unassigned     = 0;
                std::vector<int> band_counts(request.bands.size(), 0);
                bool             changed = false;

                for (std::size_t i = 0; i < plan.volumes.size(); ++i) {
                    Slic3r::ModelVolume*   mv         = target.volumes[std::size_t(plan.volumes[i].index)];
                    const FacetAssignment& assignment = assignments[i];
                    original_facets_total += int(assignment.states.size());
                    facets_unassigned     += assignment.unassigned;
                    for (std::size_t b = 0; b < assignment.band_counts.size() && b < band_counts.size(); ++b)
                        band_counts[b] += assignment.band_counts[b];
                    facets_selected += int(assignment.states.size()) - assignment.unassigned;
                    // False here can only mean "the annotation already held exactly this": every
                    // other reason apply_facet_states rejects a write was ruled out in hop 1, and
                    // plan_still_valid just proved the facet count did not move.
                    changed |= apply_facet_states(*mv, mode, assignment.states, request.replace);
                }

                refresh_after_paint(target);

                nlohmann::json volumes = nlohmann::json::array();
                for (std::size_t i = 0; i < plan.volumes.size(); ++i) {
                    Slic3r::ModelVolume* mv = target.volumes[std::size_t(plan.volumes[i].index)];
                    nlohmann::json by_mode  = nlohmann::json::object();
                    by_mode[paint_mode_name(mode)] = painted_json(*mv, mode);
                    volumes.push_back({{"volume_id", plan.volumes[i].volume_id},
                                       {"name", mv->name},
                                       {"original_facets", int(mv->mesh().its.indices.size())},
                                       {"bounding_box", bbox_json(mv->mesh().transformed_bounding_box(plan.volumes[i].to_plate))},
                                       {"modes", by_mode}});
                }

                std::vector<std::string> info = paint_prerequisite_messages(*target.object, mode);
                if (facets_unassigned > 0)
                    info.push_back(std::to_string(facets_unassigned) +
                                   " facets fell outside the selection and kept their previous state.");

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
                    {"original_facets_total", original_facets_total},
                    {"facets_selected", facets_selected},
                    {"facets_unassigned", facets_unassigned},
                    {"bounding_box", bbox_json(target_plate_bbox(target))},
                    {"volumes", volumes},
                    {"info_messages", info},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
                if (request.selection == "bands") {
                    result["axis"]       = paint_axis_name(request.axis);
                    result["axis_range"] = {{"from", request.range_from}, {"to", request.range_to}};
                    nlohmann::json bands = nlohmann::json::array();
                    for (std::size_t b = 0; b < request.bands.size(); ++b) {
                        nlohmann::json band = {{"from", request.bands[b].from},
                                               {"to", request.bands[b].to},
                                               {"state", request.bands[b].state},
                                               {"label", paint_state_label(mode, request.bands[b].state)},
                                               {"facet_count", b < band_counts.size() ? band_counts[b] : 0}};
                        band["filament"] = mode == PaintMode::Color && request.bands[b].state > 0
                                               ? nlohmann::json(request.bands[b].state)
                                               : nlohmann::json(nullptr);
                        bands.push_back(band);
                    }
                    result["bands"] = bands;
                }
                return result;
            });
        }
```

Before replacing, **diff the existing response assembly** (`:745-850`) against the `result` above and carry over any field the current code emits that this listing omits — the contract with `docs/tools/reference.md` must not shrink. The fields named here are the documented ones.

- [ ] **Step 3: Build both targets, run the suite**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8 && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests | tail -3`
Expected: builds clean; 0 failures.

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
git commit -m "fix: a three-minute paint no longer holds the GUI thread, so every other tool keeps answering"
```

---

### Task 7: `get_object_components`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp` — add `#include "OrcaMCPPaintSelect.hpp"`, add a `register_tool` block inside `register_paint_tools()` after `get_object_paint`'s.

**Interfaces:**
- Consumes `facet_component_ids`, `summarize_components` (Task 1); `capture_paint_plan` (Task 6).
- Produces the tool `get_object_components {object_id, volume_id?, instance_id?}`.

- [ ] **Step 1: Register the tool**

```cpp
    register_tool({
        "get_object_components",
        "List the connected shells of each part's mesh -- component id, facet count, area and a "
        "plate-frame bounding box. A generated or assembled model often has a feature (a bag, a "
        "wheel) as its own shell; paint_object {selection: \"component\", component: <id>} paints "
        "exactly that shell. Ids are stable for a given mesh: discovery order by lowest facet "
        "index. Coordinates are PLATE millimetres. On a mesh of millions of facets this takes "
        "seconds; it runs off the GUI thread, so other tools keep answering meanwhile.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
                {"volume_id", {{"type", "integer"}, {"minimum", -1},
                               {"description", "Part index within the object (0-based); omit, or pass -1, for every part"}}},
                {"instance_id", {{"type", "integer"}, {"minimum", 0},
                                 {"description", "Which instance's transform defines plate coordinates (default 0)"}}}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            PaintPlan plan;
            nlohmann::json gate = run_on_main_thread([&params, &plan]() -> nlohmann::json {
                PaintTarget target;
                std::string error;
                if (!resolve_paint_target(params, target, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};
                plan = capture_paint_plan(target);
                return nlohmann::json{{"status", "success"}, {"object_name", target.object->name}};
            });
            if (gate.value("status", "") != "success")
                return gate;

            // Worker thread: the flood fill over the neighbour index, per volume.
            nlohmann::json volumes = nlohmann::json::array();
            for (const PaintPlanVolume& pv : plan.volumes) {
                int                    count = 0;
                const std::vector<int> ids   = facet_component_ids(pv.mesh->its, count);
                std::vector<ComponentInfo> summary = summarize_components(pv.mesh->its, ids, count, pv.to_plate);
                // Largest first: the shell a caller is looking for is rarely the smallest sliver.
                std::stable_sort(summary.begin(), summary.end(),
                                 [](const ComponentInfo& a, const ComponentInfo& b) { return a.facet_count > b.facet_count; });
                nlohmann::json components = nlohmann::json::array();
                for (const ComponentInfo& c : summary)
                    components.push_back({{"component", c.component},
                                          {"facet_count", c.facet_count},
                                          {"area_mm2", c.area},
                                          {"bounding_box", bbox_json(c.bbox)}});
                volumes.push_back({{"volume_id", pv.volume_id},
                                   {"name", pv.name},
                                   {"original_facets", int(pv.mesh->its.indices.size())},
                                   {"bounding_box", bbox_json(pv.mesh->transformed_bounding_box(pv.to_plate))},
                                   {"component_count", count},
                                   {"components", components}});
            }

            return nlohmann::json{{"status", "success"},
                                  {"object_id", plan.object_id},
                                  {"object_name", gate.value("object_name", "")},
                                  {"coordinate_frame", "plate"},
                                  {"instance_id", int(plan.instance_idx)},
                                  {"volumes", volumes}};
        }
    });
```

Add `#include <algorithm>` if the file lacks it.

- [ ] **Step 2: Build; confirm the tool count is 75**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8 2>&1 | grep -c "error:"; grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'`
Expected: `0` then `75`.

- [ ] **Step 3: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
git commit -m "feat: an agent can list a model's shells before painting one of them"
```

---

### Task 8: `pick_facet`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp` — add helpers `read_vec3_array`, `parse_camera_frame` to the anonymous namespace; add a `register_tool` block after `get_object_components`'s.

**Interfaces:**
- Consumes `pick_nearest_point`, `pick_ray`, `unproject_pixel_to_ray`, `CameraFrame`, `facet_component_ids` (Tasks 1–3); `capture_paint_plan` (Task 6); the existing `read_vec3(const nlohmann::json&, Vec3d&, const char*, std::string&)` at `:290`.
- Produces: `pick_facet` and the file-local `bool parse_camera_frame(const nlohmann::json&, CameraFrame&, std::string&)`, which Task 10 must emit the inverse of.

- [ ] **Step 1: Add the camera parser**

In the anonymous namespace, after `read_vec3`:

```cpp
// The `camera` object render_plate_view returns, read back. Both matrices are 16 numbers,
// row-major, exactly as emitted; the viewport is {x, y, width, height} in pixels.
bool parse_matrix4(const nlohmann::json& value, const char* what, Eigen::Matrix4d& out, std::string& error)
{
    if (!value.is_array() || value.size() != 16) {
        error = std::string(what) + " must be an array of 16 numbers, row-major";
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        double v = 0.0;
        if (!parse_double_param(value[i], v)) {
            error = std::string(what) + "[" + std::to_string(i) + "] is not a finite number";
            return false;
        }
        out(i / 4, i % 4) = v;
    }
    return true;
}

bool parse_camera_frame(const nlohmann::json& value, CameraFrame& out, std::string& error)
{
    if (!value.is_object() || !value.contains("view_matrix") || !value.contains("projection_matrix") ||
        !value.contains("viewport")) {
        error = "camera needs view_matrix, projection_matrix and viewport -- pass the `camera` object a "
                "render_plate_view result contains, unchanged";
        return false;
    }
    if (!parse_matrix4(value["view_matrix"], "camera.view_matrix", out.view, error) ||
        !parse_matrix4(value["projection_matrix"], "camera.projection_matrix", out.projection, error))
        return false;
    const nlohmann::json& vp = value["viewport"];
    if (!vp.is_array() || vp.size() != 4) {
        error = "camera.viewport must be [x, y, width, height]";
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        int v = 0;
        if (!parse_integer_param(vp[i], v)) {
            error = "camera.viewport[" + std::to_string(i) + "] is not an integer";
            return false;
        }
        out.viewport[std::size_t(i)] = v;
    }
    return true;
}
```

- [ ] **Step 2: Register the tool**

```cpp
    register_tool({
        "pick_facet",
        "Find the facet a point, a ray, or a pixel of a render lands on. Give ONE of: point "
        "[x,y,z] (plate mm; snaps to the nearest surface), ray {origin, direction} (plate mm; "
        "first hit), or pixel [u,v] plus the `camera` object from a render_plate_view result "
        "(u right, v down, (0,0) top-left). Returns the volume, facet, plate point and normal -- "
        "feed `point` straight into paint_object {selection: \"connected\", seed: {point}}. "
        "include_component adds the shell id for paint_object {selection: \"component\"}; it "
        "costs a pass over the mesh.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
                {"volume_id", {{"type", "integer"}, {"minimum", -1},
                               {"description", "Restrict to one part (0-based); omit, or pass -1, to search every part"}}},
                {"instance_id", {{"type", "integer"}, {"minimum", 0},
                                 {"description", "Which instance's transform defines plate coordinates (default 0)"}}},
                {"point", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                           {"description", "Plate mm; the nearest surface point is picked"}}},
                {"ray", {{"type", "object"},
                         {"properties", {{"origin", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                                         {"direction", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
                         {"description", "Plate mm; the first surface along the ray is picked"}}},
                {"pixel", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2},
                           {"description", "[u, v] in the render's pixels, (0,0) top-left; needs `camera`"}}},
                {"camera", {{"type", "object"},
                            {"description", "The `camera` object a render_plate_view view returned, unchanged"}}},
                {"include_component", {{"type", "boolean"},
                                       {"description", "Also report the shell id of the hit facet (default false)"}}}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            // Resolve the query first: it needs no Model, and a bad query should fail before any hop.
            const int given = int(params.contains("point")) + int(params.contains("ray")) + int(params.contains("pixel"));
            if (given != 1)
                return nlohmann::json{{"status", "error"},
                                      {"message", "give exactly one of point, ray, or pixel (+ camera)"}};

            std::string error;
            bool        by_point = false;
            Slic3r::Vec3d point = Slic3r::Vec3d::Zero(), origin = Slic3r::Vec3d::Zero(), dir = Slic3r::Vec3d::Zero();
            if (params.contains("point")) {
                by_point = true;
                if (!read_vec3(params["point"], point, "point", error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};
            } else if (params.contains("ray")) {
                const nlohmann::json& ray = params["ray"];
                if (!ray.is_object() || !ray.contains("origin") || !ray.contains("direction") ||
                    !read_vec3(ray["origin"], origin, "ray.origin", error) ||
                    !read_vec3(ray["direction"], dir, "ray.direction", error))
                    return nlohmann::json{{"status", "error"},
                                          {"message", error.empty() ? "ray needs origin and direction" : error}};
                if (dir.squaredNorm() == 0.0)
                    return nlohmann::json{{"status", "error"}, {"message", "ray.direction must not be zero"}};
            } else {
                if (!params.contains("camera"))
                    return nlohmann::json{{"status", "error"},
                                          {"message", "pixel needs the `camera` object from the render it came from"}};
                CameraFrame camera;
                if (!parse_camera_frame(params["camera"], camera, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};
                const nlohmann::json& px = params["pixel"];
                double u = 0.0, v = 0.0;
                if (!px.is_array() || px.size() != 2 || !parse_double_param(px[0], u) || !parse_double_param(px[1], v))
                    return nlohmann::json{{"status", "error"}, {"message", "pixel must be [u, v]"}};
                if (!unproject_pixel_to_ray(camera, u, v, origin, dir))
                    return nlohmann::json{{"status", "error"},
                                          {"message", "camera cannot be inverted (degenerate viewport or matrices)"}};
            }
            bool include_component = false;
            if (params.contains("include_component") && !parse_boolean_param(params["include_component"], include_component))
                return nlohmann::json{{"status", "error"}, {"message", "include_component must be true or false"}};

            // Hop 1: the plan.
            PaintPlan plan;
            nlohmann::json gate = run_on_main_thread([&params, &plan]() -> nlohmann::json {
                PaintTarget target;
                std::string err;
                if (!resolve_paint_target(params, target, err))
                    return nlohmann::json{{"status", "error"}, {"message", err}};
                plan = capture_paint_plan(target);
                return nlohmann::json{{"status", "success"}};
            });
            if (gate.value("status", "") != "success")
                return gate;

            // Hop 2: the pick, best over every target volume.
            const PaintPlanVolume* best_volume = nullptr;
            SurfacePick            best;
            for (const PaintPlanVolume& pv : plan.volumes) {
                SurfacePick pick;
                const bool  hit = by_point ? pick_nearest_point(*pv.mesh, pv.to_plate, point, pick)
                                           : pick_ray(*pv.mesh, pv.to_plate, origin, dir, pick);
                if (hit && (best_volume == nullptr || pick.distance < best.distance)) {
                    best_volume = &pv;
                    best        = pick;
                }
            }
            if (best_volume == nullptr)
                return nlohmann::json{{"status", "error"},
                                      {"message", by_point ? "no surface found (empty meshes?)"
                                                           : "the ray does not hit the object"}};

            nlohmann::json result = {
                {"status", "success"},
                {"object_id", plan.object_id},
                {"coordinate_frame", "plate"},
                {"instance_id", int(plan.instance_idx)},
                {"volume_id", best_volume->volume_id},
                {"volume_name", best_volume->name},
                {"facet", best.facet},
                {"point", {best.point_plate.x(), best.point_plate.y(), best.point_plate.z()}},
                {"normal", {best.normal_plate.x(), best.normal_plate.y(), best.normal_plate.z()}},
                {"distance_mm", best.distance},
                {"query", by_point ? "point" : (params.contains("ray") ? "ray" : "pixel")}
            };
            if (include_component) {
                int count = 0;
                const std::vector<int> ids = facet_component_ids(best_volume->mesh->its, count);
                result["component"]       = ids[std::size_t(best.facet)];
                result["component_count"] = count;
            }
            return result;
        }
    });
```

- [ ] **Step 3: Build; confirm the count is 76**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8 2>&1 | grep -c "error:"; grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'`
Expected: `0` then `76`.

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
git commit -m "feat: what an agent can see or name in a render can now be resolved to a facet"
```

---

### Task 9: `paint_object` gains `connected` and `component`

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp` — `PaintRequest` (`:313-325`), `parse_paint_request` (`:374-518`), the `paint_object` schema and description (`:560-675`), hop 2 of the handler (Task 6).

**Interfaces:**
- Consumes `assign_connected`, `assign_component`, `facet_component_ids`, `pick_nearest_point`, `kDefaultSeedFillAngleDeg` (Tasks 1, 2, 4).

- [ ] **Step 1: Extend `PaintRequest`**

Add to `struct PaintRequest`:

```cpp
    // selection == "connected"
    bool          seed_by_point   = true;   // false: seed_volume_id + seed_facet were given
    Slic3r::Vec3d seed_point      = Slic3r::Vec3d::Zero();   // plate mm
    int           seed_volume_id  = -1;
    int           seed_facet      = -1;
    double        angle_deg       = kDefaultSeedFillAngleDeg;
    // selection == "component"
    int           component       = -1;
```

- [ ] **Step 2: Parse the two selections**

In `parse_paint_request`, before the final `error = "Unknown selection ..."`, add:

```cpp
    if (out.selection == "connected") {
        if (!params.contains("seed") || !params["seed"].is_object()) {
            error = "selection 'connected' needs seed: {point: [x,y,z]} in plate millimetres, or "
                    "seed: {volume_id, facet} from pick_facet";
            return false;
        }
        const nlohmann::json& seed = params["seed"];
        if (seed.contains("point")) {
            out.seed_by_point = true;
            if (!read_vec3(seed["point"], out.seed_point, "seed.point", error))
                return false;
        } else if (seed.contains("facet")) {
            out.seed_by_point = false;
            if (!parse_integer_param(seed["facet"], out.seed_facet) || out.seed_facet < 0) {
                error = "seed.facet must be a non-negative integer";
                return false;
            }
            out.seed_volume_id = 0;
            if (seed.contains("volume_id") && (!parse_integer_param(seed["volume_id"], out.seed_volume_id) || out.seed_volume_id < 0)) {
                error = "seed.volume_id must be a non-negative integer";
                return false;
            }
        } else {
            error = "seed needs either point or facet";
            return false;
        }
        if (params.contains("angle")) {
            if (!parse_number_field(params["angle"], "angle", out.angle_deg, error))
                return false;
            if (out.angle_deg <= 0.0 || out.angle_deg >= 180.0) {
                error = "angle must be between 0 and 180 degrees (exclusive); the gizmo's default is 30";
                return false;
            }
        }
        return parse_single_state(params, mode, out.state, error);
    }

    if (out.selection == "component") {
        if (!params.contains("component") || !parse_integer_param(params["component"], out.component) || out.component < 0) {
            error = "selection 'component' needs component: <id> from get_object_components";
            return false;
        }
        // Component ids are per volume; with several parts in scope the id is ambiguous.
        if (target.volumes.size() != 1) {
            error = "selection 'component' needs exactly one part in scope: pass volume_id (the "
                    "object has " + std::to_string(target.volumes.size()) + " parts)";
            return false;
        }
        return parse_single_state(params, mode, out.state, error);
    }
```

Update the two error strings that enumerate selections (`"selection is required: bands, box, sphere or all"` and `"Unknown selection ... expected bands, box, sphere or all"`) to include `connected` and `component`.

- [ ] **Step 3: Compute them in hop 2**

In hop 2 of the handler (Task 6), extend the per-volume branch:

```cpp
            std::string seed_error;                 // set by the connected branch on failure
            nlohmann::json resolved_seed;           // reported back so the caller sees what was filled from
            for (std::size_t i = 0; i < plan.volumes.size(); ++i) {
                const PaintPlanVolume& pv          = plan.volumes[i];
                const std::size_t      facet_count = pv.mesh->its.indices.size();
                if (request.selection == "all") {
                    assignments[i] = assign_all(facet_count, request.state);
                } else if (request.selection == "component") {
                    int                    count = 0;
                    const std::vector<int> ids   = facet_component_ids(pv.mesh->its, count);
                    if (request.component >= count) {
                        seed_error = "component " + std::to_string(request.component) + " does not exist: volume " +
                                     std::to_string(pv.volume_id) + " has " + std::to_string(count) + " components";
                        break;
                    }
                    assignments[i] = assign_component(ids, request.component, request.state);
                } else if (request.selection == "connected") {
                    // The seed lives on exactly one volume; every other volume is left alone.
                    SurfacePick seed;
                    bool        seeds_here = false;
                    if (request.seed_by_point) {
                        // Nearest volume wins; resolved on the first pass below.
                        seeds_here = pick_nearest_point(*pv.mesh, pv.to_plate, request.seed_point, seed);
                    } else if (pv.volume_id == request.seed_volume_id) {
                        seeds_here = request.seed_facet < int(facet_count);
                        if (!seeds_here) {
                            seed_error = "seed.facet " + std::to_string(request.seed_facet) + " is out of range for volume " +
                                         std::to_string(pv.volume_id) + " (" + std::to_string(facet_count) + " facets)";
                            break;
                        }
                        seed.facet = request.seed_facet;
                        const Slic3r::Vec3i32& f = pv.mesh->its.indices[std::size_t(seed.facet)];
                        seed.point_local = ((pv.mesh->its.vertices[f[0]] + pv.mesh->its.vertices[f[1]] +
                                             pv.mesh->its.vertices[f[2]]) / 3.f).cast<double>();
                        seed.point_plate = pv.to_plate * seed.point_local;
                    }
                    if (!seeds_here) {
                        assignments[i] = FacetAssignment{std::vector<int>(facet_count, -1), {}, int(facet_count)};
                        continue;
                    }
                    assignments[i] = assign_connected(*pv.mesh, pv.to_plate, seed.facet, seed.point_local.cast<float>(),
                                                      request.angle_deg, request.state);
                    resolved_seed = {{"volume_id", pv.volume_id}, {"facet", seed.facet},
                                     {"point", {seed.point_plate.x(), seed.point_plate.y(), seed.point_plate.z()}},
                                     {"snap_distance_mm", seed.distance}, {"angle", request.angle_deg}};
                } else {
                    const std::vector<Slic3r::Vec3d> centroids = facet_centroids(pv.mesh->its, pv.to_plate);
                    if (request.selection == "bands")
                        assignments[i] = assign_bands(centroids, request.axis, request.bands);
                    else if (request.selection == "box")
                        assignments[i] = assign_box(centroids, request.box, request.state);
                    else
                        assignments[i] = assign_sphere(centroids, request.sphere, request.state);
                }
            }
            if (!seed_error.empty())
                return nlohmann::json{{"status", "error"}, {"message", seed_error}};
```

**The point-seed must resolve to ONE volume.** With several volumes in scope the loop above would seed a fill on every one of them. Immediately above the loop, add this pre-pass, which turns a point seed into a facet seed on the nearest volume so the loop takes the facet branch exactly once:

```cpp
            if (request.selection == "connected" && request.seed_by_point && plan.volumes.size() > 1) {
                const PaintPlanVolume* nearest = nullptr;
                SurfacePick            nearest_pick;
                for (const PaintPlanVolume& pv : plan.volumes) {
                    SurfacePick pick;
                    if (pick_nearest_point(*pv.mesh, pv.to_plate, request.seed_point, pick) &&
                        (nearest == nullptr || pick.distance < nearest_pick.distance)) {
                        nearest      = &pv;
                        nearest_pick = pick;
                    }
                }
                if (nearest == nullptr)
                    return nlohmann::json{{"status", "error"}, {"message", "seed.point found no surface on any part"}};
                request.seed_by_point  = false;
                request.seed_volume_id = nearest->volume_id;
                request.seed_facet     = nearest_pick.facet;
            }
```

With a single volume in scope the loop's own `pick_nearest_point` call handles the point seed and reports `snap_distance_mm`; after the pre-pass the facet branch reports `snap_distance_mm` as 0, which is accurate — the snap already happened.

In hop 3, add `&resolved_seed` to the lambda's capture list (`[&params, &plan, &request, &mode, &assignments, &resolved_seed]`) and add `{"seed", resolved_seed}` to `result` when `request.selection == "connected"`.

- [ ] **Step 4: Update the schema and description**

In the `paint_object` schema `properties`, add:

```cpp
                {"seed", {{"type", "object"},
                          {"description", "selection=connected: {point: [x,y,z]} in plate mm (snapped to the "
                                          "surface), or {volume_id, facet} from pick_facet"}}},
                {"angle", {{"type", "number"},
                           {"description", "selection=connected: stop at edges sharper than this many degrees "
                                           "(default 30, the gizmo's smart-fill default)"}}},
                {"component", {{"type", "integer"}, {"minimum", 0},
                               {"description", "selection=component: a shell id from get_object_components; "
                                               "needs volume_id when the object has several parts"}}},
```

Change the `selection` enum to `{"bands", "box", "sphere", "all", "connected", "component"}` and add to the description: *"connected fills the surface region around a seed without crossing an edge sharper than `angle` — the way to paint a feature such as a bag or a sleeve; component paints one shell by id. Very large meshes (millions of facets) take seconds to minutes; the work runs off the GUI thread, but the bridge's ORCAMCP_TIMEOUT (default 120 s) may still need raising."*

- [ ] **Step 5: Build both targets, run the suite**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8 && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests | tail -3`
Expected: builds; 0 failures.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPaintTools.cpp
git commit -m "feat: paint_object can fill a feature from a seed, or paint one shell by id"
```

---

### Task 10: `render_plate_view` returns its camera

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.hpp:27` (the `RenderThumbnail` declaration)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.cpp:187-201` (the view loop) and `:287-289` (after the projection is applied)

**Interfaces:**
- Produces `struct RenderCameraInfo` and the `camera` JSON object whose keys `parse_camera_frame` (Task 8) reads: `view_matrix` (16, row-major), `projection_matrix` (16, row-major), `viewport` (4).

- [ ] **Step 1: Declare the out-param**

In `OrcaMCPPlateUtils.hpp`, before the class, add:

```cpp
// The camera a thumbnail was rendered with, so a pixel in it can be unprojected later.
struct RenderCameraInfo
{
    Transform3d        view       = Transform3d::Identity();
    Transform3d        projection = Transform3d::Identity();
    std::array<int, 4> viewport   = {0, 0, 0, 0};
    bool               perspective = true;
};
```

Change the declaration at `:27` to:

```cpp
    static void RenderThumbnail(ThumbnailData& thumbnail_data,
                                const Vec3d& camera_position, const Vec3d& target, int plate_index,
                                RenderCameraInfo* out_camera = nullptr);
```

Add `#include <array>` and `#include "libslic3r/Point.hpp"` (where `Transform3d` is defined) if not already present.

- [ ] **Step 2: Fill it after the projection is set**

In `RenderThumbnail`, immediately after `const Transform3d& projection_matrix = camera.get_projection_matrix();` (`:289`), add:

```cpp
    if (out_camera != nullptr) {
        out_camera->view        = view_matrix;
        out_camera->projection  = projection_matrix;
        out_camera->viewport    = camera.get_viewport();
        out_camera->perspective = camera_type == Camera::EType::Perspective;
    }
```

Update the definition's signature to match the declaration.

- [ ] **Step 3: Emit it per view**

In `RenderPlateView`, replace the block from `ThumbnailData data;` through `view_index++;` with:

```cpp
        ThumbnailData    data;
        data.set(resolution, resolution);
        RenderCameraInfo cam;
        RenderThumbnail(data, camera_position, target, plate_index, &cam);

        auto matrix16 = [](const Transform3d& t) {
            nlohmann::json out = nlohmann::json::array();
            const Eigen::Matrix4d& m = t.matrix();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    out.push_back(m(r, c));
            return out;
        };
        // Everything pick_facet needs to turn a pixel of this image back into a ray. Row 0 of the
        // saved image is the top: save_thumbnail_to_file flips the GL buffer (this file, :44,:63).
        nlohmann::json camera_json = {
            {"view_matrix", matrix16(cam.view)},
            {"projection_matrix", matrix16(cam.projection)},
            {"viewport", {cam.viewport[0], cam.viewport[1], cam.viewport[2], cam.viewport[3]}},
            {"type", cam.perspective ? "perspective" : "orthographic"},
            {"pixel_origin", "top_left"},
            {"camera_position", {camera_position.x(), camera_position.y(), camera_position.z()}},
            {"target", {target.x(), target.y(), target.z()}}
        };

        nlohmann::json entry;
        if (save_to_file) {
            entry["file_path"] = save_thumbnail_to_file(data, view_index);
        } else {
            entry["base64"] = encode_thumbnail_to_base64(data, false);
        }
        entry["camera"] = camera_json;
        result.push_back(entry);
        view_index++;
```

- [ ] **Step 4: Build**

Run: `cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8 2>&1 | grep -c "error:"`
Expected: `0`. (`CaptureTurntablePreview` at `:635` still calls the four-argument form; the default argument keeps it compiling.)

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.cpp
git commit -m "feat: a render now says which camera drew it, so a pixel in it can be picked"
```

---

### Task 11: Documentation and the in-app catalog

**Files:**
- Modify: `docs/tools/reference.md` — Painting Tools preamble (`:1174-1203`), `paint_object` (`:1204-1271`), insert `get_object_components` and `pick_facet` sections before `## Printer Tools` (`:1367`), `render_plate_view` response (`:983` section), Quick Reference Table Painting row (`:27`)
- Modify: `CLAUDE.md:102,104,121`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:707-718` (the `painting` catalog block)

- [ ] **Step 1: `reference.md`**

In the Quick Reference Table Painting row, add `get_object_components`, `pick_facet`.

In the `paint_object` parameter table, add rows:

| `seed` | object | `connected` | `{point: [x,y,z]}` in plate mm (snapped to the surface) or `{volume_id, facet}` from `pick_facet` |
| `angle` | number | No | `connected` only: stop at edges sharper than this (degrees). Default 30 — the gizmo's smart-fill default |
| `component` | integer | `component` | A shell id from `get_object_components`. Needs `volume_id` when the object has several parts |

Change `selection`'s row to list `bands`, `box`, `sphere`, `all`, `connected`, `component`. Under **Notes**, add:

- `connected` is the GUI's smart fill: the region reachable from the seed without crossing an edge whose dihedral angle exceeds `angle`. It is how to paint a *feature* — a bag, a sleeve, a wheel — without knowing its coordinates. The response carries `seed` as resolved: `volume_id`, `facet`, `point`, `snap_distance_mm`, `angle`.
- `component` ids are per volume and deterministic for a mesh (discovery order by lowest facet index).
- On meshes of millions of facets the geometry takes seconds to minutes. It runs off the GUI thread, so other tools answer meanwhile, but the bridge's `ORCAMCP_TIMEOUT` (default 120 s) may need raising for the paint call itself.

Add a worked example:

```json
pick_facet    {"object_id": 0, "point": [129.5, 144, 68]}
→ {"volume_id": 0, "facet": 1180231, "point": [129.4, 141.9, 68.2], ...}
paint_object  {"object_id": 0, "selection": "connected", "seed": {"point": [129.4, 141.9, 68.2]}, "filament": 16}
```

New section before `## Printer Tools`:

```markdown
### get_object_components
List the connected shells of each part's mesh.

**Parameters:** `object_id` (required), `volume_id` (optional, -1 = every part), `instance_id`
(optional, default 0).

**Response includes:** `coordinate_frame` (`"plate"`), `instance_id`, and per volume
`{volume_id, name, original_facets, bounding_box, component_count, components}` where each
component is `{component, facet_count, area_mm2, bounding_box}`, largest first. Ids are stable
for a given mesh, so `paint_object {selection: "component", component: <id>, volume_id}` paints
exactly that shell.

---

### pick_facet
Turn a point, a ray, or a pixel of a render into the facet it lands on.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `volume_id` | integer | No | Restrict to one part; omit or `-1` to search every part |
| `instance_id` | integer | No | Whose transform defines plate coordinates (default 0) |
| `point` | array | one of three | `[x, y, z]` plate mm; the nearest surface point is picked |
| `ray` | object | one of three | `{origin: [x,y,z], direction: [x,y,z]}` plate mm; first hit |
| `pixel` | array | one of three | `[u, v]` in a render's pixels, `(0,0)` top-left; needs `camera` |
| `camera` | object | with `pixel` | The `camera` object a `render_plate_view` view returned, unchanged |
| `include_component` | boolean | No | Also report the shell id (default false; costs a pass over the mesh) |

**Response includes:** `volume_id`, `volume_name`, `facet`, `point` (plate mm, on the surface),
`normal` (unit, plate frame), `distance_mm` (from the query point / ray origin), `query`, and with
`include_component`: `component`, `component_count`.

The loop this closes: `render_plate_view` → read the image → `pick_facet {pixel, camera}` →
`paint_object {selection: "connected", seed: {point}}`.

---
```

In `render_plate_view`'s section, add to the response: each view now carries `camera`:
`{view_matrix (16, row-major), projection_matrix (16, row-major), viewport [x,y,w,h], type,
pixel_origin: "top_left", camera_position, target}` — pass it to `pick_facet` unchanged.

- [ ] **Step 2: `CLAUDE.md`**

`:102` → `### MCP Tools (76 registered, 77 reachable)`; `:104` → `The server registers 76; …`; `:121` Painting row → add `` `get_object_components`, `pick_facet` ``.

- [ ] **Step 3: `get_server_info` catalog**

In the `painting` block at `OrcaMCPServer.cpp:707`, extend the `paint_object` line's text with *"selection=connected fills a feature from a seed (see pick_facet); selection=component paints one shell"*, and add:

```cpp
                        {"get_object_components", "List a part's connected shells: id, facet count, area, plate bbox. "
                                                  "Paint one with paint_object selection=component."},
                        {"pick_facet", "Point, ray, or render pixel + camera -> the facet it lands on, with its plate "
                                       "point and normal. Feed the point to paint_object selection=connected."}
```

- [ ] **Step 4: Verify the count and build**

Run: `grep -hA1 -E '^\s*register_tool\(\{' src/slic3r/GUI/OrcaMCP/*.cpp | grep -coE '^\s*"[a-z0-9_]+",'; cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8 2>&1 | grep -c "error:"`
Expected: `76` then `0`.

- [ ] **Step 5: Commit**

```bash
git add docs/tools/reference.md CLAUDE.md src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp
git commit -m "docs: the see-point-fill loop, and two more tools an agent can find"
```

---

### Task 12: Acceptance — her bag in two calls, and a server that keeps answering

This is the **only** task that starts the application. Run it with the user present. Per
`CLAUDE.md`, every check uses `mcp__orca-slicer__*` tools, never `curl` — except the one
responsiveness probe below, which deliberately bypasses the bridge to time the server itself.

**Preconditions:** the high-poly figurine the user painted on 2026-09-11 (≈4.3 M facets), or any
mesh over a million facets with a distinct feature, is available to load.

- [ ] **Step 1: Stage and start**

```bash
cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j8
pkill -x OrcaSlicer || true
rm -rf build/arm64/src/Release/OrcaSlicer.app
cp -R build/arm64/src/RelWithDebInfo/OrcaSlicer.app build/arm64/src/Release/OrcaSlicer.app
```

`mcp__orca-slicer__start_orca {}`. A macOS keychain prompt appears on the first launch of a
rebuilt binary; the user answers it. Then `get_server_info {}` — `tools_by_category.painting`
lists `pick_facet` and `get_object_components`: the running binary is this build.

- [ ] **Step 2: Load the model, select the multi-filament printer, and find the feature**

`new_project`, `load_model` the figurine, `select_preset {type: "printer", name: "Flashforge Creator 5 Pro 0.4 nozzle"}`.

```
render_plate_view {"plate_index": 0, "save_to_file": true, "resolution": 768,
                   "views": [{"camera_position": [128, 628, 60], "target": [128, 128, 60]}]}
```

Expected: the view result carries `camera` with `view_matrix` and `projection_matrix` of 16 numbers, `viewport` `[0,0,768,768]`, `pixel_origin: "top_left"`. Read the image; note the pixel `[u, v]` at the centre of the backpack.

- [ ] **Step 3: Pick by pixel (acceptance 3)**

`pick_facet {"object_id": 0, "pixel": [u, v], "camera": <the camera object, verbatim>}`

Expected: `status: success`, a `point` whose plate coordinates lie on the backpack — for the figurine, roughly `x` 118–141, `y` 134–154, `z` 53–83, the box measured by hand on 2026-09-11. If the point lands on the *front* of the figure instead, the y-flip convention in `unproject_pixel_to_ray` is inverted for this renderer; fix that one sign and re-run.

- [ ] **Step 4: Fill the bag (acceptance 1)**

```
set_mixed_filament {"slot": 16, "components": [1, 2], "ratios": [20, 80]}     # a blue, if slot 16 is not already one
paint_object {"object_id": 0, "selection": "connected", "seed": {"point": <pick.point>}, "filament": 16}
```

Expected: `status: success`, `seed` echoed with `volume_id`, `facet`, `snap_distance_mm` near 0, `angle: 30`; `facets_selected` in the low hundreds of thousands (the 2026-09-11 box paint covered 234,921). Render the back view again: the backpack is blue and the pigtails, skirt and shirt are not. **Two calls, no ruler.** If the fill leaks onto the shirt, lower `angle` (try 20); if it stops short of the pack's own flap, raise it (try 45).

- [ ] **Step 5: Components**

`get_object_components {"object_id": 0}` — expected within the bridge timeout, a sorted list; note whether the backpack is its own shell (a `facet_count` near the `facets_selected` above with a `bounding_box` inside the box from Step 3). If it is: `clear_object_paint {"object_id": 0, "mode": "color"}` then `paint_object {"object_id": 0, "selection": "component", "component": <id>, "filament": 16}` and confirm by render it matches Step 4.

- [ ] **Step 6: The server stays up (acceptance 2)**

Start a band paint over the whole mesh and, while it runs, time a direct probe of the server:

```
paint_object {"object_id": 0, "selection": "bands", "axis": "z",
              "filaments": [3,2,1,4,3,2,1,4,3,2,1,4]}
```

Immediately after issuing it (within ~5 s), in a shell:

```bash
time curl -s -m 10 -X POST http://localhost:13618/mcp -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_server_info","arguments":{}}}' \
  | head -c 80
```

Expected: a JSON prefix and `real` under 2 s while the paint is still computing. On 2026-09-11 the same probe hung for the full 10 s and the port accepted no connection. Also check `get_slicing_status {}` through the bridge answers during the paint.

- [ ] **Step 7: Undo and round trip**

`undo {}` → `get_object_paint {"object_id": 0, "mode": "color"}` shows the bag paint from Step 4 restored. `export_3mf {"output_path": "/tmp/figurine_bag.3mf"}`, `new_project`, `load_project` it, `get_object_paint` again — the same `facet_count` and `coverage_percent` for filament 16.

- [ ] **Step 8: Report, do not commit**

Report to the user: the two-call bag render; the `pick_facet` point from the pixel and whether it needed the sign fix; the probe timing during the paint; whether the bag is a shell; any step that did not match.

---

## Notes for whoever executes this

**Deliberately not built.** A cursor/brush (`select_patch`) — it subdivides triangles and is a far larger correctness surface than reading back whole facets. A job API with progress — R5 removes the blocking that motivated it; if a single call still exceeds the bridge timeout after Tasks 5–6, that is the next spec, not a side quest here. Caching the AABB tree — `pick_facet` rebuilds it per call; measure first.

**The one thing that could not be tested headlessly.** Whether the saved image's row 0 is the top is asserted from `OrcaMCPPlateUtils.cpp:44,63` and pinned in Task 3's test against a hand-built camera. Task 12 Step 3 is where a wrong sign would show, and it is a one-line fix in `unproject_pixel_to_ray` if it does.

**Where the plan argues with the earlier one.** Plan 2 excluded seed fill because "a hit point is a mouse concept". `seed_fill_select_triangles` takes a mesh-local point and a facet index (`TriangleSelector.hpp:332-338`) — nothing about a mouse — and `pick_nearest_point` produces both from a plate point. That is the whole premise of R1, and Task 4's tests prove it on a cube.
