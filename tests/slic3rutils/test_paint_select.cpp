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
