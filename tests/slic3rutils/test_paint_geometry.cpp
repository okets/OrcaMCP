#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp"
#include "libslic3r/TriangleMesh.hpp"

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
