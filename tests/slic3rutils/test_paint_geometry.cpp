#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"

#include <algorithm>
#include <string>
#include <vector>

// paint_object's geometry. Bands and regions are resolved here, away from the Model and the GUI,
// because this is the arithmetic that decides which triangle gets which filament -- the part that
// has to be right before anything is written to a FacetsAnnotation.

using namespace Slic3r::GUI::OrcaMCP;
using Slic3r::Vec3d;
using Slic3r::Vec3f;
using Slic3r::Vec3i32;
using Slic3r::Transform3d;
using Slic3r::Model;
using Slic3r::ModelObject;
using Slic3r::ModelVolume;
using Slic3r::ModelInstance;
using Slic3r::EnforcerBlockerType;
using Slic3r::TriangleSelector;
using Slic3r::TriangleMesh;
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

    // A unit cube has six unit-square faces: 6 mm2 total, independent of triangulation.
    CHECK_THAT(its_surface_area(Slic3r::its_make_cube(1.0, 1.0, 1.0)), WithinAbs(6.0, 1e-9));
}

TEST_CASE("facet_centroids applies a rotation, not just a translation", "[orcamcp][paint]")
{
    const indexed_triangle_set its = two_triangle_rectangle();

    // A 90 degree rotation about Z sends (x, y, 0) to (-y, x, 0).
    Transform3d to_plate = Transform3d::Identity();
    to_plate.rotate(Eigen::AngleAxisd(M_PI / 2.0, Vec3d::UnitZ()));
    const std::vector<Vec3d> rotated = facet_centroids(its, to_plate);
    REQUIRE(rotated.size() == 2);
    // Facet 0's centroid (8/3, 2, 0) rotates to (-2, 8/3, 0).
    CHECK_THAT(rotated[0].x(), WithinAbs(-2.0, 1e-9));
    CHECK_THAT(rotated[0].y(), WithinAbs(8.0 / 3.0, 1e-9));
    CHECK_THAT(rotated[0].z(), WithinAbs(0.0, 1e-9));
}

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

TEST_CASE("assign_bands, assign_box, assign_sphere and assign_all handle zero facets",
          "[orcamcp][paint]")
{
    // No centroids at all -- an object with no facets, or a caller who filtered them all out
    // upstream. None of the four should crash or report a spurious unassigned count.
    const std::vector<Vec3d> none;

    const std::vector<PaintBand> bands = {{5, 0.0, 10.0}, {6, 10.0, 20.0}};
    const FacetAssignment        banded = assign_bands(none, PaintAxis::Y, bands);
    CHECK(banded.states.empty());
    REQUIRE(banded.band_counts.size() == 2);
    CHECK(banded.band_counts[0] == 0);
    CHECK(banded.band_counts[1] == 0);
    CHECK(banded.unassigned == 0);

    const FacetAssignment boxed = assign_box(none, {Vec3d(0, 0, 0), Vec3d(10, 10, 10)}, 3);
    CHECK(boxed.states.empty());
    CHECK(boxed.unassigned == 0);

    const FacetAssignment sphered = assign_sphere(none, {Vec3d(0, 0, 0), 2.0}, 2);
    CHECK(sphered.states.empty());
    CHECK(sphered.unassigned == 0);

    const FacetAssignment everything = assign_all(0, 1);
    CHECK(everything.states.empty());
    CHECK(everything.unassigned == 0);
}

TEST_CASE("assign_box and assign_sphere report every facet unassigned when the region misses",
          "[orcamcp][paint]")
{
    const std::vector<Vec3d> centroids = {Vec3d(50.0, 50.0, 50.0), Vec3d(60.0, 60.0, 60.0)};

    const FacetAssignment boxed = assign_box(centroids, {Vec3d(0, 0, 0), Vec3d(10, 10, 10)}, 3);
    CHECK(boxed.states[0] == -1);
    CHECK(boxed.states[1] == -1);
    CHECK(boxed.unassigned == 2);

    const FacetAssignment sphered = assign_sphere(centroids, {Vec3d(0, 0, 0), 2.0}, 2);
    CHECK(sphered.states[0] == -1);
    CHECK(sphered.states[1] == -1);
    CHECK(sphered.unassigned == 2);
}

TEST_CASE("assign_bands puts a centroid exactly on an interior band edge in the upper band",
          "[orcamcp][paint]")
{
    // Mirrors band_index_for_value's own boundary rule, but exercised through assign_bands so
    // the whole-selection entry point is proven, not just the lookup it delegates to.
    const std::vector<Vec3d>     centroids = {Vec3d(0.0, 10.0, 0.0)};
    const std::vector<PaintBand> bands     = {{5, 0.0, 10.0}, {6, 10.0, 20.0}};

    const FacetAssignment assignment = assign_bands(centroids, PaintAxis::Y, bands);

    CHECK(assignment.states[0] == 6);
    CHECK(assignment.unassigned == 0);
    REQUIRE(assignment.band_counts.size() == 2);
    CHECK(assignment.band_counts[0] == 0);
    CHECK(assignment.band_counts[1] == 1);
}

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
    // Building this from a flat mesh (e.g. two_triangle_rectangle()) logs
    // "its_convex_hull: Unable to create convex hull" (TriangleMesh.cpp:1366): qhull can't build a
    // 3-D hull from coplanar points. Expected here, not a bug -- fixtures that don't need a
    // hand-checkable centroid use its_make_cube instead to keep the test output clean.
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

TEST_CASE("every PaintMode round-trips through paint_mode_name and parse_paint_mode",
          "[orcamcp][paint]")
{
    for (PaintMode mode : {PaintMode::Color, PaintMode::Support, PaintMode::Seam, PaintMode::FuzzySkin}) {
        PaintMode parsed = PaintMode::Color;
        REQUIRE(parse_paint_mode(paint_mode_name(mode), parsed));
        CHECK(parsed == mode);
    }
}

TEST_CASE("parse_paint_mode matches parse_paint_axis's case-insensitivity", "[orcamcp][paint]")
{
    // Both parsers are reached from the same MCP tool's parameters; disagreeing about case would
    // cost a caller a wasted round trip on something like "Color".
    PaintMode mode = PaintMode::Seam;
    REQUIRE(parse_paint_mode("Color", mode));
    CHECK(mode == PaintMode::Color);
    REQUIRE(parse_paint_mode("FUZZY_SKIN", mode));
    CHECK(mode == PaintMode::FuzzySkin);
    REQUIRE(parse_paint_mode("SeAm", mode));
    CHECK(mode == PaintMode::Seam);
}

TEST_CASE("annotation_for_mode selects the member the GUI gizmo writes", "[orcamcp][paint]")
{
    // A closed cube, not the flat rectangle: this test checks addresses, not centroids, so there
    // is no reason to pay for the convex-hull warning a coplanar mesh logs.
    HeadlessObject built = make_headless_object(Slic3r::its_make_cube(1.0, 1.0, 1.0), Vec3d(0, 0, 0));

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
    // "fuzzy_skin" is a synonym for "enforcer" in this mode: it is the token
    // paint_state_label(FuzzySkin, 1) hands back, and it has to parse to the same state or an
    // agent that reads a facet's state and echoes it into a write gets rejected for it.
    state = -99;
    REQUIRE(parse_paint_state(PaintMode::FuzzySkin, "fuzzy_skin", state));
    CHECK(state == 1);
    // The synonym is FuzzySkin-only -- Support has no use for it.
    CHECK_FALSE(parse_paint_state(PaintMode::Support, "fuzzy_skin", state));

    // Case is folded, the same as parse_paint_mode and parse_paint_axis. All three read
    // parameters of one MCP tool, so accepting "Support" for mode while rejecting "Enforcer"
    // for state would cost a caller a round trip over a distinction that means nothing.
    state = -99;
    REQUIRE(parse_paint_state(PaintMode::Support, "ENFORCER", state));
    CHECK(state == 1);
    REQUIRE(parse_paint_state(PaintMode::Support, "Blocker", state));
    CHECK(state == 2);
    REQUIRE(parse_paint_state(PaintMode::Support, "NoNe", state));
    CHECK(state == 0);
    REQUIRE(parse_paint_state(PaintMode::FuzzySkin, "Fuzzy_Skin", state));
    CHECK(state == 1);
    // Folding case does not widen what is accepted.
    CHECK_FALSE(parse_paint_state(PaintMode::FuzzySkin, "BLOCKER", state));
    CHECK_FALSE(parse_paint_state(PaintMode::Support, "ENFORCE", state));

    // Colour states are filament slot numbers, not names.
    CHECK_FALSE(parse_paint_state(PaintMode::Color, "enforcer", state));
    CHECK_FALSE(parse_paint_state(PaintMode::Support, "yes", state));
}

TEST_CASE("paint_state_label and parse_paint_state round-trip for every valid state",
          "[orcamcp][paint]")
{
    // Color's states are filament-slot integers, not names: parse_paint_state rejects Color
    // outright by design ("colour rejects every name" -- its states go through the tool layer as
    // an int, never through this parser), so it has no name-based round trip to check here.
    // Support and Seam accept none/enforcer/blocker; FuzzySkin's only valid states are none and
    // its enforcer (state 2 -- "blocker" -- has no meaning for FuzzySkin).
    const std::vector<PaintMode> enforcer_blocker_modes = {PaintMode::Support, PaintMode::Seam};
    for (PaintMode mode : enforcer_blocker_modes) {
        for (int state = 0; state <= 2; ++state) {
            int parsed = -99;
            REQUIRE(parse_paint_state(mode, paint_state_label(mode, state), parsed));
            CHECK(parsed == state);
        }
    }
    for (int state = 0; state <= 1; ++state) {
        int parsed = -99;
        REQUIRE(parse_paint_state(PaintMode::FuzzySkin, paint_state_label(PaintMode::FuzzySkin, state), parsed));
        CHECK(parsed == state);
    }
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

TEST_CASE("volume_to_plate composes a translated volume with a rotated, translated instance",
          "[orcamcp][paint]")
{
    // A pure-translation instance cannot tell instance.get_matrix() * volume.get_matrix() apart
    // from the reverse order, or from either matrix alone: translations commute. Giving the
    // instance a rotation makes the two orders diverge, so this assertion is load-bearing.
    HeadlessObject built = make_headless_object(two_triangle_rectangle(), Vec3d(150.0, 150.0, 0.0));
    built.volume->set_offset(Vec3d(10.0, 20.0, 0.0));
    built.object->instances[0]->set_rotation(Vec3d(0.0, 0.0, M_PI / 2.0));

    const Transform3d to_plate = volume_to_plate(*built.object, *built.volume, 0);
    const std::vector<Vec3d> centroids = facet_centroids(built.volume->mesh().its, to_plate);

    REQUIRE(centroids.size() == 2);
    // Facet 0's local centroid (8/3, 2, 0) plus the volume offset (10, 20, 0) is (10 + 8/3, 22, 0).
    // The instance's 90 degree Z rotation sends (x, y, 0) to (-y, x, 0) (see
    // "facet_centroids applies a rotation" above), giving (-22, 10 + 8/3, 0); its offset (150, 150,
    // 0) then applies on top: (150 - 22, 150 + 10 + 8/3, 0).
    // The other multiplication order -- volume.get_matrix() * instance.get_matrix() -- would
    // rotate the local centroid first instead: (-2, 8/3, 0) + instance offset + volume offset =
    // (158, 172 + 2/3, 0), a different answer, which is what makes this test prove the order.
    CHECK_THAT(centroids[0].x(), WithinAbs(150.0 - 22.0, 1e-9));
    CHECK_THAT(centroids[0].y(), WithinAbs(150.0 + 10.0 + 8.0 / 3.0, 1e-9));
}

TEST_CASE("volume_to_plate falls back to the volume matrix alone when the object has no instance",
          "[orcamcp][paint]")
{
    // Documented in the header: an object with no instance at all yields the volume matrix alone,
    // rather than indexing an empty instances vector. A cube stands in for the rectangle here
    // since this test compares matrices, not a hand-checked centroid.
    Model model;
    ModelObject* object = model.add_object();
    const TriangleMesh mesh(Slic3r::its_make_cube(1.0, 1.0, 1.0));
    ModelVolume* volume = object->add_volume(mesh, false);
    volume->set_offset(Vec3d(1.0, 2.0, 3.0));

    CHECK(volume_to_plate(*object, *volume, 0).isApprox(volume->get_matrix()));
}

TEST_CASE("brim_point_to_object pins world z to the underside before converting to object-local",
          "[orcamcp][paint]")
{
    // set_brim_ears's one rule: an ear always sits on the bottom of the object, the same z
    // GLGizmoBrimEars.cpp (~395-397) assigns before converting a click to object-local.
    Model model;
    ModelObject* object = model.add_object();
    ModelInstance* instance = object->add_instance();
    instance->set_offset(Vec3d(100.0, 50.0, 0.0));

    const Vec3f local = brim_point_to_object(*object, 110.0, 60.0, 0);
    CHECK_THAT(double(local.x()), WithinAbs(10.0, 1e-6));
    CHECK_THAT(double(local.y()), WithinAbs(10.0, 1e-6));
    CHECK_THAT(double(local.z()), WithinAbs(-0.0001, 1e-9));
}

TEST_CASE("brim_point_to_object and brim_point_to_plate round-trip through a rotated, "
          "translated instance",
          "[orcamcp][paint]")
{
    // brim_point_to_object is set_brim_ears's write-side conversion; brim_point_to_plate is
    // brim_ears_json's read-side conversion (Task 8). They have to invert each other exactly, or
    // an ear placed at a plate coordinate would read back somewhere else.
    Model model;
    ModelObject* object = model.add_object();
    ModelInstance* instance = object->add_instance();
    instance->set_offset(Vec3d(150.0, 150.0, 0.0));
    instance->set_rotation(Vec3d(0.0, 0.0, M_PI / 2.0));

    const Vec3f local = brim_point_to_object(*object, 200.0, 130.0, 0);
    const Vec3d back  = brim_point_to_plate(*object, local, 0);

    CHECK_THAT(back.x(), WithinAbs(200.0, 1e-6));
    CHECK_THAT(back.y(), WithinAbs(130.0, 1e-6));
    CHECK_THAT(back.z(), WithinAbs(-0.0001, 1e-6));

    // An out-of-range instance falls back to instance 0, the same way volume_to_plate does.
    const Vec3f local_fallback = brim_point_to_object(*object, 200.0, 130.0, 99);
    CHECK(local_fallback.isApprox(local));
}

TEST_CASE("brim_point_to_plate falls back to identity when the object has no instance at all",
          "[orcamcp][paint]")
{
    Model model;
    ModelObject* object = model.add_object();
    const Vec3f local(1.0f, 2.0f, -0.0001f);

    const Vec3d plate = brim_point_to_plate(*object, local, 0);
    CHECK_THAT(plate.x(), WithinAbs(1.0, 1e-6));
    CHECK_THAT(plate.y(), WithinAbs(2.0, 1e-6));
    CHECK_THAT(plate.z(), WithinAbs(-0.0001, 1e-6));
}

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

TEST_CASE("max_paint_state_for bounds each mode by what its own gizmo can write",
          "[orcamcp][paint]")
{
    // One shared bound, because a copy of it per calling tool is how the four modes drift apart.
    CHECK(max_paint_state_for(PaintMode::Color) == max_paint_state());
    CHECK(max_paint_state_for(PaintMode::Support) == int(EnforcerBlockerType::BLOCKER));
    CHECK(max_paint_state_for(PaintMode::Seam) == int(EnforcerBlockerType::BLOCKER));
    // FuzzySkin has no blocker: FUZZY_SKIN aliases ENFORCER and the gizmo paints nothing else.
    CHECK(max_paint_state_for(PaintMode::FuzzySkin) == int(EnforcerBlockerType::FUZZY_SKIN));
    CHECK(max_paint_state_for(PaintMode::FuzzySkin) < max_paint_state_for(PaintMode::Seam));
}

TEST_CASE("apply_facet_states accepts the highest state each mode allows", "[orcamcp][paint]")
{
    // The boundary itself is legal -- an off-by-one in the guard would make filament 32,
    // a blocker, or a fuzzy skin enforcer unpaintable, and nothing else would notice.
    // A cube, not the flat rectangle: this test checks states, not hand-checked centroids, and
    // a coplanar fixture logs an its_convex_hull error per volume built.
    const PaintMode modes[] = {PaintMode::Color, PaintMode::Support, PaintMode::Seam,
                               PaintMode::FuzzySkin};
    for (PaintMode mode : modes) {
        HeadlessObject built = make_headless_object(Slic3r::its_make_cube(1.0, 1.0, 1.0),
                                                    Vec3d(0, 0, 0));
        const std::size_t facets = built.volume->mesh().its.indices.size();
        const int         top    = max_paint_state_for(mode);
        CHECK(apply_facet_states(*built.volume, mode, std::vector<int>(facets, top), true));

        const std::vector<PaintedStateInfo> painted = read_volume_paint(*built.volume, mode);
        REQUIRE(painted.size() == 1);
        CHECK(painted[0].state == top);
        CHECK(painted[0].facet_count == int(facets));
        // Every facet painted, so this state's share of the surface is all of it.
        CHECK_THAT(painted[0].area_ratio, WithinAbs(1.0, 1e-9));

        // One past the boundary is rejected for that mode, even where another mode allows it:
        // a blocker is legal for Seam and not for FuzzySkin, and 33 is legal for nothing.
        CHECK_FALSE(apply_facet_states(*built.volume, mode, std::vector<int>(facets, top + 1), true));
    }
}

TEST_CASE("a rejected apply_facet_states leaves the annotation byte-identical", "[orcamcp][paint]")
{
    // Returning false is not enough: a rejected call must not have painted a prefix, because a
    // half-painted model looks deliberate and nothing downstream can tell it from an intended one.
    HeadlessObject    built  = make_headless_object(Slic3r::its_make_cube(1.0, 1.0, 1.0),
                                                    Vec3d(0, 0, 0));
    const std::size_t facets = built.volume->mesh().its.indices.size();
    REQUIRE(facets > 1);
    REQUIRE(apply_facet_states(*built.volume, PaintMode::Color, std::vector<int>(facets, 5), true));
    const TriangleSelector::TriangleSplittingData before =
        built.volume->mmu_segmentation_facets.get_data();

    // Too few entries. This is the case a std::min clamp would have let through, repainting the
    // first facet to 9 and reporting success.
    CHECK_FALSE(apply_facet_states(*built.volume, PaintMode::Color, {9}, true));
    CHECK(built.volume->mmu_segmentation_facets.get_data() == before);

    // Too many entries: equally a caller bug, since a FacetAssignment is sized to the facet count.
    CHECK_FALSE(apply_facet_states(*built.volume, PaintMode::Color,
                                   std::vector<int>(facets + 1, 9), true));
    CHECK(built.volume->mmu_segmentation_facets.get_data() == before);

    // A state above the mode's ceiling. EnforcerBlockerType is an int8_t the serializer bit-packs,
    // so writing this would be clamped much later, far from the call that caused it.
    std::vector<int> too_high(facets, 5);
    too_high.back() = max_paint_state() + 1;
    CHECK_FALSE(apply_facet_states(*built.volume, PaintMode::Color, too_high, true));
    CHECK(built.volume->mmu_segmentation_facets.get_data() == before);

    // A negative that is not the documented -1 sentinel is a caller bug, not "leave alone".
    std::vector<int> bad_sentinel(facets, 5);
    bad_sentinel.back() = -2;
    CHECK_FALSE(apply_facet_states(*built.volume, PaintMode::Color, bad_sentinel, true));
    CHECK(built.volume->mmu_segmentation_facets.get_data() == before);

    // All -1: a legal call that asks for no change at all, so it is applied and reports false
    // because the annotation already held exactly this -- not because it was rejected.
    CHECK_FALSE(apply_facet_states(*built.volume, PaintMode::Color,
                                   std::vector<int>(facets, -1), false));
    CHECK(built.volume->mmu_segmentation_facets.get_data() == before);
}

TEST_CASE("apply_facet_states refuses a volume with no facets", "[orcamcp][paint]")
{
    // A mesh with no triangles has no facet to address, so every states vector is the wrong size
    // for it, including the empty one. Guarded explicitly rather than left to fall out of the
    // size comparison, because 0 == 0 would otherwise report success for a no-op.
    HeadlessObject built = make_headless_object(indexed_triangle_set(), Vec3d(0, 0, 0));
    REQUIRE(built.volume->mesh().its.indices.empty());

    CHECK_FALSE(apply_facet_states(*built.volume, PaintMode::Color, {}, true));
    CHECK_FALSE(apply_facet_states(*built.volume, PaintMode::Color, {5}, true));
    CHECK(built.volume->mmu_segmentation_facets.empty());
    CHECK(read_volume_paint(*built.volume, PaintMode::Color).empty());
}
