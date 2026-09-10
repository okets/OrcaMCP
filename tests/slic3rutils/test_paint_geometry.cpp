#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp"

#include <string>
#include <vector>

// paint_object's geometry. Bands and regions are resolved here, away from the Model and the GUI,
// because this is the arithmetic that decides which triangle gets which filament -- the part that
// has to be right before anything is written to a FacetsAnnotation.

using namespace Slic3r::GUI::OrcaMCP;
using Slic3r::Vec3d;
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
