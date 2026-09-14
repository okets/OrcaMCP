#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"

// The "is this object on the plate" test the transform tools report as on_bed. Only the arithmetic
// is here: resolving *which* plate an object landed on needs a PartPlateList and a live Plater, so
// the re-homing half of rehome_and_report_placement cannot be reached from a headless test.

using Slic3r::BoundingBoxf3;
using Slic3r::Vec3d;
using Slic3r::GUI::OrcaMCP::object_within_plate;

namespace {
// A 256 x 256 plate whose origin is not the world origin: plate 4 of a four-plate project sits at
// x[307,563] y[-307,-51], which is the arrangement the cross-plate move was found on.
BoundingBoxf3 far_plate() { return BoundingBoxf3(Vec3d(307, -307, 0), Vec3d(563, -51, 250)); }
BoundingBoxf3 first_plate() { return BoundingBoxf3(Vec3d(0, 0, 0), Vec3d(256, 256, 250)); }

BoundingBoxf3 part_at(const Vec3d& centre, double half = 10.0)
{
    return BoundingBoxf3(centre - Vec3d(half, half, half), centre + Vec3d(half, half, half));
}
} // namespace

TEST_CASE("an object inside the plate it was moved to is on the bed", "[object_placement]")
{
    // The reproduction: moved to (462, -105), squarely inside plate 4.
    const BoundingBoxf3 part(Vec3d(452, -115, 0), Vec3d(472, -95, 20));
    CHECK(object_within_plate(part, far_plate()));
    // ...and measuring the same object against plate 1, which is what asking the *selected* plate
    // used to do, calls a correct move a placement error.
    CHECK_FALSE(object_within_plate(part, first_plate()));
}

TEST_CASE("an object over a plate edge is not on the bed", "[object_placement]")
{
    CHECK_FALSE(object_within_plate(part_at(Vec3d(2, 128, 10), 10.0), first_plate()));   // past min x
    CHECK_FALSE(object_within_plate(part_at(Vec3d(250, 128, 10), 10.0), first_plate())); // past max x
    CHECK_FALSE(object_within_plate(part_at(Vec3d(128, 2, 10), 10.0), first_plate()));   // past min y
    CHECK_FALSE(object_within_plate(part_at(Vec3d(128, 250, 10), 10.0), first_plate())); // past max y
    CHECK(object_within_plate(part_at(Vec3d(128, 128, 10), 10.0), first_plate()));
}

TEST_CASE("bed contact is tolerated but sinking into the bed is not", "[object_placement]")
{
    const BoundingBoxf3 plate = first_plate();
    // Exactly on the bed, and the float-noise margin the GUI allows.
    CHECK(object_within_plate(BoundingBoxf3(Vec3d(10, 10, 0.0), Vec3d(30, 30, 20)), plate));
    CHECK(object_within_plate(BoundingBoxf3(Vec3d(10, 10, -0.05), Vec3d(30, 30, 20)), plate));
    // Sunk below it: a part that would be printed through the bed.
    CHECK_FALSE(object_within_plate(BoundingBoxf3(Vec3d(10, 10, -5.0), Vec3d(30, 30, 20)), plate));
}

TEST_CASE("an object taller than the plate box is still on the bed", "[object_placement]")
{
    // Z is only checked downwards: exceeding the build height is the slicer's own error to report,
    // and calling it "outside the printable area" would send a caller looking for an x/y problem.
    CHECK(object_within_plate(BoundingBoxf3(Vec3d(10, 10, 0), Vec3d(30, 30, 400)), first_plate()));
}
