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
