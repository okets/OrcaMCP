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

TEST_CASE("a pick refuses a transform it cannot invert, rather than answering NaN",
          "[orcamcp][select]")
{
    // scale_object takes a scale of 0 without complaint, so an object really can be sitting on the
    // plate under a rank-deficient transform. Its inverse is full of infinities, and every answer
    // derived from it -- the local query point, the plate point, the normal -- comes back NaN,
    // which nlohmann serialises as null under status "success". Refused instead, out loud.
    const TriangleMesh mesh(Slic3r::its_make_cube(10.0, 10.0, 10.0));
    Eigen::Matrix3d    flattened = Eigen::Matrix3d::Identity();
    flattened(2, 2) = 0.0;
    Transform3d flat = Transform3d::Identity();
    flat.linear() = flattened;

    CHECK_FALSE(plate_transform_is_invertible(flat));
    CHECK(plate_transform_is_invertible(Transform3d::Identity()));

    SurfacePick pick;
    CHECK_FALSE(pick_nearest_point(mesh, flat, Vec3d(5.0, 5.0, 20.0), pick));
    CHECK_FALSE(pick_ray(mesh, flat, Vec3d(5.0, 5.0, 50.0), Vec3d(0.0, 0.0, -1.0), pick));

    // The normal is where the NaN was actually produced (the inverse transpose of the linear
    // part), so it is guarded at its own door too: a caller reaching it directly gets the zero
    // vector this function already returns for "no normal here".
    CHECK(facet_normal_plate(mesh.its, 0, flat).isZero());

    // A very small but uniform scale is not singular -- its inverse is perfectly well defined --
    // and a fixed epsilon on the determinant would have thrown it out with the flattened one.
    Transform3d tiny = Transform3d::Identity();
    tiny.linear() = Eigen::Matrix3d::Identity() * 1e-6;
    CHECK(plate_transform_is_invertible(tiny));
    CHECK(pick_nearest_point(mesh, tiny, Vec3d(0.0, 0.0, 1.0), pick));
    CHECK(pick.normal_plate.norm() > 0.5);
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

namespace {

// Deliberately asymmetric: every one of the sixteen elements is a different number, and m(r, c)
// != m(c, r) for every off-diagonal pair. That is the whole point -- a symmetric matrix, or the
// pretty ones above with their handful of non-zero entries, would survive a transposed emitter or
// a transposed parser unchanged and the round trip would pass while the contract was broken.
Eigen::Matrix4d counting_matrix(double base)
{
    Eigen::Matrix4d m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m(r, c) = base + r * 4 + c;   // row-major counting order, so 1,2,3... reads across rows
    return m;
}

// The object render_plate_view builds: the three required keys from the shared emitter, plus the
// informational keys it adds on top. Written here the way the renderer writes it
// (OrcaMCPPlateUtils.cpp, RenderPlateView) so the extras are exercised too.
nlohmann::json render_plate_view_camera_json(const CameraFrame& cam)
{
    nlohmann::json out     = camera_frame_to_json(cam);
    out["type"]            = "perspective";
    out["pixel_origin"]    = "top_left";
    out["camera_position"] = {10.0, 20.0, 30.0};
    out["target"]          = {1.0, 2.0, 3.0};
    return out;
}

} // namespace

// The contract between the two halves of see -> point: render_plate_view emits the camera it drew
// a view with, pick_facet reads that object back. Nothing errors when the two disagree about key
// names or row order -- the agent just gets a confident answer about the wrong triangle -- so this
// round trip is the only thing that can catch a mismatch.
TEST_CASE("a rendered camera survives the trip out to JSON and back unchanged", "[orcamcp][select]")
{
    CameraFrame sent;
    sent.view       = counting_matrix(1.0);    // 1 .. 16
    sent.projection = counting_matrix(101.0);  // 101 .. 116, so the two cannot be swapped unseen
    sent.viewport   = {7, 11, 640, 480};       // x != y and width != height, for the same reason

    const nlohmann::json emitted = render_plate_view_camera_json(sent);

    // The wire shape itself, not just the round trip: 16 numbers in row-major order. A parser that
    // agreed with a column-major emitter would still round-trip, and would still be wrong.
    REQUIRE(emitted["view_matrix"].is_array());
    REQUIRE(emitted["view_matrix"].size() == 16);
    for (int i = 0; i < 16; ++i)
        CHECK(emitted["view_matrix"][i].get<double>() == 1.0 + i);
    CHECK(emitted["viewport"] == nlohmann::json({7, 11, 640, 480}));

    CameraFrame received;
    std::string error;
    REQUIRE(parse_camera_frame(emitted, received, error));
    CHECK(error.empty());

    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            CHECK(received.view(r, c) == sent.view(r, c));
            CHECK(received.projection(r, c) == sent.projection(r, c));
        }
    CHECK(received.viewport == sent.viewport);

    // And the unprojection agrees, which is what the round trip is for: the same pixel of the same
    // view must give the same ray on both sides.
    CameraFrame real_sent = test_camera();
    real_sent.viewport    = {0, 0, 200, 200};
    CameraFrame real_received;
    REQUIRE(parse_camera_frame(render_plate_view_camera_json(real_sent), real_received, error));
    Vec3d o1, d1, o2, d2;
    REQUIRE(unproject_pixel_to_ray(real_sent, 37.0, 149.0, o1, d1));
    REQUIRE(unproject_pixel_to_ray(real_received, 37.0, 149.0, o2, d2));
    CHECK_THAT((o1 - o2).norm(), WithinAbs(0.0, 1e-12));
    CHECK_THAT((d1 - d2).norm(), WithinAbs(0.0, 1e-12));
}

// The emitter adds keys the parser never asked for, and is free to add more. A parser that
// rejected an unknown key would break every caller the moment the renderer gained a field.
TEST_CASE("parse_camera_frame ignores the keys render_plate_view adds for the reader",
          "[orcamcp][select]")
{
    nlohmann::json camera = render_plate_view_camera_json(test_camera());
    REQUIRE(camera.contains("type"));
    REQUIRE(camera.contains("pixel_origin"));
    REQUIRE(camera.contains("camera_position"));
    REQUIRE(camera.contains("target"));
    camera["something_a_later_version_adds"] = 42;

    CameraFrame out;
    std::string error;
    CHECK(parse_camera_frame(camera, out, error));
}

TEST_CASE("parse_camera_frame refuses a camera missing any of the three keys it needs",
          "[orcamcp][select]")
{
    CameraFrame out;
    std::string error;

    for (const char* key : {"view_matrix", "projection_matrix", "viewport"}) {
        nlohmann::json camera = camera_frame_to_json(test_camera());
        camera.erase(key);
        CHECK_FALSE(parse_camera_frame(camera, out, error));
        // One message naming all three, so a caller that dropped a key is told what to send back.
        CHECK(error.find("view_matrix") != std::string::npos);
    }

    // Present but the wrong shape -- a 9-element matrix, a 3-element viewport -- is refused too,
    // and named, since a silently zero-filled matrix would point the pick anywhere at all.
    nlohmann::json short_matrix        = camera_frame_to_json(test_camera());
    short_matrix["projection_matrix"]  = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK_FALSE(parse_camera_frame(short_matrix, out, error));
    CHECK(error.find("camera.projection_matrix") != std::string::npos);

    nlohmann::json short_viewport    = camera_frame_to_json(test_camera());
    short_viewport["viewport"]       = {0, 0, 200};
    CHECK_FALSE(parse_camera_frame(short_viewport, out, error));
    CHECK(error.find("camera.viewport") != std::string::npos);

    // Not an object at all.
    CHECK_FALSE(parse_camera_frame(nlohmann::json::array(), out, error));
}

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
