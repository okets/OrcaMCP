#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>
#include <string>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateOccupancy.hpp"

// The arithmetic behind "what else is standing on this plate". An agent that had the complete list
// of model objects still collided with the prime tower, because the tower was in neither the scene
// report nor the render: the occupancy list it reasoned over was missing an occupant. These are the
// rectangle sums that make the list complete -- the tower body plus its brim, whether that fits
// inside the plate, whether it lands on something, and how far an object's brim reaches past it.
//
// Reading the configs and walking the PartPlateList needs a live Plater and wxWidgets, so the
// half of the feature that fills these numbers in is not reachable headlessly and is not covered
// here. What is covered is every decision that turns those numbers into an answer.

using Catch::Matchers::WithinAbs;
using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;

namespace {
BoundingBoxf rect(double min_x, double min_y, double max_x, double max_y)
{
    BoundingBoxf b;
    b.min = Vec2d(min_x, min_y);
    b.max = Vec2d(max_x, max_y);
    b.defined = true;
    return b;
}

// The default Bambu-style 256 x 256 plate, and the same plate as plate 4 of a four-plate project,
// which sits several hundred millimetres along +X in the frame get_scene_info reports.
BoundingBoxf first_plate() { return rect(0, 0, 256, 256); }
BoundingBoxf far_plate() { return rect(307, -307, 563, -51); }
} // namespace

// ---- the tower's own rectangle ----------------------------------------------------------------

TEST_CASE("the prime tower footprint includes its brim on all four sides", "[plate_occupancy]")
{
    // A 60 x 40 tower at plate-frame (100, 150) with a 3 mm brim.
    const BoundingBoxf f = prime_tower_footprint(Vec2d(100, 150), Vec2d(60, 40), 3.0);
    CHECK_THAT(f.min.x(), WithinAbs(97.0, 1e-9));
    CHECK_THAT(f.min.y(), WithinAbs(147.0, 1e-9));
    CHECK_THAT(f.max.x(), WithinAbs(163.0, 1e-9));
    CHECK_THAT(f.max.y(), WithinAbs(193.0, 1e-9));

    // The wrong version of this function -- body only, brim dropped -- is what leaves an agent
    // placing a part flush against the reported edge and being told the tower is too close. That
    // part occupies x[93, 98]; it clears the body at x=100 but sits squarely on the brim at x=97.
    const BoundingBoxf body = prime_tower_footprint(Vec2d(100, 150), Vec2d(60, 40), 0.0);
    const BoundingBoxf part = rect(93, 160, 98, 180);
    CHECK_FALSE(footprints_overlap(part, body));
    CHECK(footprints_overlap(part, f));
}

TEST_CASE("a brim width of zero leaves the tower body exactly as it is", "[plate_occupancy]")
{
    const BoundingBoxf f = prime_tower_footprint(Vec2d(10, 20), Vec2d(50, 50), 0.0);
    CHECK_THAT(f.min.x(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(f.max.y(), WithinAbs(70.0, 1e-9));
}

TEST_CASE("a negative margin never shrinks a footprint", "[plate_occupancy]")
{
    // prime_tower_brim_width is negative when the brim is automatic. Letting that through would
    // report an occupied area smaller than the printed one, which is the direction that collides.
    const BoundingBoxf f = expand_footprint(rect(0, 0, 10, 10), -5.0);
    CHECK_THAT(f.min.x(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(f.max.x(), WithinAbs(10.0, 1e-9));
}

TEST_CASE("footprint_of drops Z and keeps the bed shadow", "[plate_occupancy]")
{
    const BoundingBoxf f = footprint_of(BoundingBoxf3(Vec3d(5, 6, 0), Vec3d(15, 26, 40)));
    CHECK_THAT(f.min.x(), WithinAbs(5.0, 1e-9));
    CHECK_THAT(f.max.y(), WithinAbs(26.0, 1e-9));
    CHECK(f.defined);
    CHECK_FALSE(footprint_of(BoundingBoxf3()).defined);
}

// ---- containment and collision ------------------------------------------------------------------

TEST_CASE("a tower footprint is measured against the plate it stands on", "[plate_occupancy]")
{
    const BoundingBoxf on_far_plate = prime_tower_footprint(Vec2d(400, -200), Vec2d(60, 40), 3.0);
    CHECK(footprint_within(on_far_plate, far_plate()));
    // Measuring it against plate 1 -- the mistake that made a correct cross-plate move look like a
    // placement error -- calls a legal position out of bounds.
    CHECK_FALSE(footprint_within(on_far_plate, first_plate()));
}

TEST_CASE("touching the plate edge is inside, crossing it is not", "[plate_occupancy]")
{
    // Exactly on the limit prime_tower_position_range reports must not then be refused.
    CHECK(footprint_within(rect(0, 0, 256, 256), first_plate()));
    CHECK_FALSE(footprint_within(rect(-0.001, 0, 256, 256), first_plate()));
    CHECK_FALSE(footprint_within(rect(0, 0, 256.001, 256), first_plate()));
    CHECK_FALSE(footprint_within(rect(0, -0.001, 256, 256), first_plate()));
    CHECK_FALSE(footprint_within(rect(0, 0, 256, 256.001), first_plate()));
}

TEST_CASE("two footprints that only share an edge do not overlap", "[plate_occupancy]")
{
    CHECK_FALSE(footprints_overlap(rect(0, 0, 10, 10), rect(10, 0, 20, 10)));
    CHECK_FALSE(footprints_overlap(rect(0, 0, 10, 10), rect(0, 10, 10, 20)));
    CHECK(footprints_overlap(rect(0, 0, 10, 10), rect(9.9, 0, 20, 10)));
    // Fully contained still counts.
    CHECK(footprints_overlap(rect(0, 0, 100, 100), rect(10, 10, 20, 20)));
    CHECK(footprints_overlap(rect(10, 10, 20, 20), rect(0, 0, 100, 100)));
    // Disjoint in one axis only is still disjoint.
    CHECK_FALSE(footprints_overlap(rect(0, 0, 10, 10), rect(5, 50, 15, 60)));
}

// ---- where the tower is allowed to stand --------------------------------------------------------

TEST_CASE("the tower position range matches the clamp the estimator applies", "[plate_occupancy]")
{
    // The numbers PartPlate::estimate_wipe_tower_polygon works with: a 256 x 256 plate, a 60 x 40
    // tower, WIPE_TOWER_MARGIN (1 mm) plus a 3 mm brim as the margin, and the same 3 mm resolved
    // brim subtracted a second time at the far edge -- which is upstream's arithmetic, not a typo
    // here: the low edge counts the brim once and the far edge counts it twice.
    const PrimeTowerRange r = prime_tower_position_range(256, 256, 60, 40, 1.0 + 3.0, 3.0);
    CHECK(r.fits);
    CHECK_THAT(r.min_x, WithinAbs(4.0, 1e-9));
    CHECK_THAT(r.max_x, WithinAbs(189.0, 1e-9));
    CHECK_THAT(r.min_y, WithinAbs(4.0, 1e-9));
    CHECK_THAT(r.max_y, WithinAbs(209.0, 1e-9));
}

TEST_CASE("a tower too big for the plate reports that it does not fit", "[plate_occupancy]")
{
    const PrimeTowerRange r = prime_tower_position_range(180, 180, 200, 40, 1.0, 0.0);
    CHECK_FALSE(r.fits);
    // The bounds are still returned, inverted, so a caller can say by how much it misses.
    CHECK(r.max_x < r.min_x);
    CHECK(r.max_y > r.min_y);
}

// ---- is there a tower here at all ---------------------------------------------------------------

namespace {
// A two-filament plate with objects on it and a tower enabled: the case the reporting exists for.
PrimeTowerConditions printed_tower()
{
    PrimeTowerConditions c;
    c.is_fff                    = true;
    c.enable_prime_tower        = true;
    c.project_filament_count    = 4;
    c.plate_has_objects         = true;
    c.plate_filament_count      = 2;
    c.plate_printable_instances = 3;
    return c;
}
} // namespace

TEST_CASE("a multi-filament plate with objects prints a tower", "[plate_occupancy]")
{
    CHECK(prime_tower_verdict(printed_tower()) == PrimeTowerVerdict::Printed);
}

TEST_CASE("every reason a plate has no tower is reported as its own reason", "[plate_occupancy]")
{
    // "no tower here" and "tower at X" must be distinguishable, and so must the several different
    // ways of having no tower: turning enable_prime_tower back on fixes one of them and none of the
    // others.
    {
        PrimeTowerConditions c = printed_tower();
        c.is_fff = false;
        CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::NotFff);
    }
    {
        PrimeTowerConditions c = printed_tower();
        c.enable_prime_tower = false;
        CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::Disabled);
    }
    {
        PrimeTowerConditions c = printed_tower();
        c.project_filament_count = 1;
        c.plate_filament_count   = 1;
        CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::SingleFilamentProject);
    }
    {
        PrimeTowerConditions c = printed_tower();
        c.gcode_only_mode = true;
        CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::GcodeOnlyMode);
    }
    {
        // The four-plate project this all came from: a four-filament project, but this one plate
        // happens to use a single filament, so it has no tower while its neighbours do.
        PrimeTowerConditions c = printed_tower();
        c.plate_filament_count = 1;
        CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::SingleFilamentPlate);
    }
    {
        PrimeTowerConditions c = printed_tower();
        c.plate_has_objects = false;
        c.plate_filament_count = 2;
        CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::EmptyPlate);
    }
    {
        PrimeTowerConditions c = printed_tower();
        c.plate_sequential = true;
        CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::SequentialMultiObject);
    }
}

TEST_CASE("printing by object still allows a tower when there is one object", "[plate_occupancy]")
{
    PrimeTowerConditions c = printed_tower();
    c.plate_sequential          = true;
    c.plate_printable_instances = 1;
    CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::Printed);
}

TEST_CASE("a smooth timelapse forces a tower onto a single-filament plate", "[plate_occupancy]")
{
    PrimeTowerConditions c = printed_tower();
    c.project_filament_count = 1;
    c.plate_filament_count   = 1;
    c.timelapse_smooth       = true;
    CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::Printed);

    c.timelapse_smooth    = false;
    c.wrapping_detection  = true;
    CHECK(prime_tower_verdict(c) == PrimeTowerVerdict::Printed);
}

TEST_CASE("every verdict has its own token and explanation", "[plate_occupancy]")
{
    // The token goes into the response as `reason`; a duplicated or empty one would make two
    // different situations indistinguishable to a caller branching on it.
    const PrimeTowerVerdict all[] = {
        PrimeTowerVerdict::Printed,               PrimeTowerVerdict::NotFff,
        PrimeTowerVerdict::Disabled,              PrimeTowerVerdict::SingleFilamentProject,
        PrimeTowerVerdict::GcodeOnlyMode,         PrimeTowerVerdict::SequentialMultiObject,
        PrimeTowerVerdict::EmptyPlate,            PrimeTowerVerdict::SingleFilamentPlate,
    };
    std::set<std::string> tokens;
    for (PrimeTowerVerdict v : all) {
        const std::string token = prime_tower_verdict_token(v);
        CHECK_FALSE(token.empty());
        CHECK(token != "unknown");
        CHECK(std::string(prime_tower_verdict_explanation(v)).size() > 10);
        CHECK(tokens.insert(token).second);
    }
}

// ---- object brim --------------------------------------------------------------------------------

TEST_CASE("no_brim and inner_only do not reach past the object outline", "[plate_occupancy]")
{
    for (const char* type : {"no_brim", "inner_only"}) {
        const ObjectBrimExtent e = object_brim_extent(type, 5.0, 0.1);
        CHECK_THAT(e.extent_mm, WithinAbs(0.0, 1e-9));
        CHECK_THAT(e.upper_bound_mm, WithinAbs(0.0, 1e-9));
        CHECK(e.exact);
    }
}

TEST_CASE("an outer brim reaches exactly the gap plus its width past the object", "[plate_occupancy]")
{
    const ObjectBrimExtent e = object_brim_extent("outer_only", 5.0, 0.1);
    CHECK_THAT(e.extent_mm, WithinAbs(5.1, 1e-9));
    CHECK_THAT(e.upper_bound_mm, WithinAbs(5.1, 1e-9));
    CHECK(e.exact);

    // The object's bounding box is not its printed footprint: a 20 mm part at the origin actually
    // occupies x[-5.1, 25.1], and a neighbour at x[26, 40] clears it while one at x[25, 40] does not.
    const BoundingBoxf printed = expand_footprint(rect(0, 0, 20, 20), e.extent_mm);
    CHECK(footprints_overlap(printed, rect(25, 0, 40, 20)));
    CHECK_FALSE(footprints_overlap(printed, rect(26, 0, 40, 20)));
}

TEST_CASE("an automatic brim is reported as an estimate with its own upper bound", "[plate_occupancy]")
{
    // auto_brim is the default brim_type, and the slicer recomputes the width per volume group at
    // slice time. Reporting the configured value as exact would be a lie; reporting the 18 mm cap
    // as the extent would make every default plate look nearly full. Both numbers, labelled.
    const ObjectBrimExtent e = object_brim_extent("auto_brim", 0.0, 0.0);
    CHECK_FALSE(e.exact);
    CHECK_THAT(e.extent_mm, WithinAbs(0.0, 1e-9));
    CHECK_THAT(e.upper_bound_mm, WithinAbs(kAutoBrimWidthCapMm, 1e-9));

    const ObjectBrimExtent ears = object_brim_extent("brim_ears", 5.0, 0.1);
    CHECK_FALSE(ears.exact);
    CHECK_THAT(ears.extent_mm, WithinAbs(5.1, 1e-9));
    CHECK_THAT(ears.upper_bound_mm, WithinAbs(0.1 + kAutoBrimWidthCapMm, 1e-9));
}

TEST_CASE("an unrecognised brim type is reported as inexact rather than as no brim", "[plate_occupancy]")
{
    // A brim type added upstream after this code was written must not silently become "no brim",
    // which would under-report the occupied area.
    const ObjectBrimExtent e = object_brim_extent("some_future_brim", 4.0, 0.2);
    CHECK_FALSE(e.exact);
    CHECK_THAT(e.extent_mm, WithinAbs(4.2, 1e-9));
}

// ---- the camera up vector -------------------------------------------------------------------------

TEST_CASE("a straight-down camera gets an up vector that is not parallel to it", "[plate_occupancy]")
{
    // The bug: render_plate_view asked for a plan view, Camera::look_at was handed up = +Z, the
    // cross product with a straight-down view direction was zero, and Eigen's normalized() returned
    // that zero vector unchanged rather than failing. The view matrix came back with an all-zero
    // 3x3 basis -- [0,0,0,-0, 0,0,0,-0, 0,0,1,-620, 0,0,0,1] -- the image rendered normally, and
    // pick_facet could not invert the matrix it was handed alongside it.
    const Vec3d camera(128, 128, 620);
    const Vec3d target(128, 128, 20);

    CHECK_THAT(Vec3d::UnitZ().cross((camera - target).normalized()).norm(), WithinAbs(0.0, 1e-12));

    const Vec3d up = stable_camera_up(camera, target);
    CHECK(up.isApprox(Vec3d::UnitY()));
    // What look_at actually needs: a cross product long enough to normalize.
    CHECK(up.cross((camera - target).normalized()).norm() > 0.5);
}

TEST_CASE("a camera looking straight up is handled too", "[plate_occupancy]")
{
    const Vec3d up = stable_camera_up(Vec3d(0, 0, -100), Vec3d(0, 0, 0));
    CHECK(up.cross(Vec3d(0, 0, -1)).norm() > 0.5);
}

TEST_CASE("an ordinary three-quarter camera keeps the preferred up vector", "[plate_occupancy]")
{
    // Nothing about the existing views may change: the turntable previews are all oblique, and
    // swapping their up vector would silently rotate every image this tool has ever returned.
    CHECK(stable_camera_up(Vec3d(400, 400, 300), Vec3d(128, 128, 20)).isApprox(Vec3d::UnitZ()));
    CHECK(stable_camera_up(Vec3d(128, 400, 300), Vec3d(128, 128, 20)).isApprox(Vec3d::UnitZ()));
    // Even a very steep view, as long as it is not exactly vertical.
    CHECK(stable_camera_up(Vec3d(128.5, 128, 620), Vec3d(128, 128, 20)).isApprox(Vec3d::UnitZ()));
}

TEST_CASE("a camera sitting on its target keeps the preferred up vector", "[plate_occupancy]")
{
    // There is no view direction to be parallel to; look_at will produce nonsense either way, and
    // inventing a different up vector would not make it better.
    CHECK(stable_camera_up(Vec3d(10, 10, 10), Vec3d(10, 10, 10)).isApprox(Vec3d::UnitZ()));
}
