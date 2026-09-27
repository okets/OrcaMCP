#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.hpp"

#include "plate_list_fixtures.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// The "is this object on the plate" test the transform tools report as on_bed, the placement fields
// they write from it, and get_scene_info's list of instances on no plate. The plate lists here are
// the CLI's, without the app (plate_list_fixtures.hpp); which plate holds an instance is
// test_plate_instances.cpp's.

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

TEST_CASE("an object inside the plate it was moved to is on the bed", "[ObjectPlacement]")
{
    // The reproduction: moved to (462, -105), squarely inside plate 4.
    const BoundingBoxf3 part(Vec3d(452, -115, 0), Vec3d(472, -95, 20));
    CHECK(object_within_plate(part, far_plate()));
    // ...and measuring the same object against plate 1, which is what asking the *selected* plate
    // used to do, calls a correct move a placement error.
    CHECK_FALSE(object_within_plate(part, first_plate()));
}

TEST_CASE("an object over a plate edge is not on the bed", "[ObjectPlacement]")
{
    CHECK_FALSE(object_within_plate(part_at(Vec3d(2, 128, 10), 10.0), first_plate()));   // past min x
    CHECK_FALSE(object_within_plate(part_at(Vec3d(250, 128, 10), 10.0), first_plate())); // past max x
    CHECK_FALSE(object_within_plate(part_at(Vec3d(128, 2, 10), 10.0), first_plate()));   // past min y
    CHECK_FALSE(object_within_plate(part_at(Vec3d(128, 250, 10), 10.0), first_plate())); // past max y
    CHECK(object_within_plate(part_at(Vec3d(128, 128, 10), 10.0), first_plate()));
}

TEST_CASE("bed contact is tolerated but sinking into the bed is not", "[ObjectPlacement]")
{
    const BoundingBoxf3 plate = first_plate();
    // Exactly on the bed, and the float-noise margin the GUI allows.
    CHECK(object_within_plate(BoundingBoxf3(Vec3d(10, 10, 0.0), Vec3d(30, 30, 20)), plate));
    CHECK(object_within_plate(BoundingBoxf3(Vec3d(10, 10, -0.05), Vec3d(30, 30, 20)), plate));
    // Sunk below it: a part that would be printed through the bed.
    CHECK_FALSE(object_within_plate(BoundingBoxf3(Vec3d(10, 10, -5.0), Vec3d(30, 30, 20)), plate));
}

TEST_CASE("an object taller than the plate box is still on the bed", "[ObjectPlacement]")
{
    // Z is only checked downwards: exceeding the build height is the slicer's own error to report,
    // and calling it "outside the printable area" would send a caller looking for an x/y problem.
    CHECK(object_within_plate(BoundingBoxf3(Vec3d(10, 10, 0), Vec3d(30, 30, 400)), first_plate()));
}

// ==================== AN OBJECT WHOSE INSTANCES STAND ON SEVERAL PLATES ====================
// Each instance is measured by its own box on the plate it is on. Measuring the box around every
// instance against instance 0's plate called an object with a copy on each of two plates "outside
// the printable area of plate 0".

using Slic3r::GUI::OrcaMCP::InstancePlacement;
using namespace plate_list_fixtures;

namespace {
nlohmann::json placement_of(const std::vector<InstancePlacement>& placements)
{
    nlohmann::json result{{"placement_warning", "left from an earlier answer"}};
    Slic3r::GUI::OrcaMCP::write_placement(result, placements);
    return result;
}
} // namespace

TEST_CASE("An object with an instance inside each of two plates is on the bed", "[ObjectPlacement][orcamcp]")
{
    const nlohmann::json result = placement_of({{0, 0, true, Vec3d(128, 128, 10)}, {1, 1, true, Vec3d(435.2, 128, 10)}});
    CHECK(result["on_bed"] == true);
    CHECK_FALSE(result.contains("placement_warning"));
    CHECK(result["plate_index"] == 0); // instance 0's
    CHECK(result["plate_indices"] == nlohmann::json::array({0, 1}));
    REQUIRE(result["instance_placement"].size() == 2);
    CHECK(result["instance_placement"][1]["instance_id"] == 1);
    CHECK(result["instance_placement"][1]["plate_index"] == 1);
    CHECK(result["instance_placement"][1]["on_bed"] == true);
    CHECK_THAT(result["instance_placement"][1]["position"]["x"].get<double>(), Catch::Matchers::WithinAbs(435.2, 1e-9));
}

TEST_CASE("An instance outside its plate or on none takes the object off the bed, and the warning names it",
          "[ObjectPlacement][orcamcp]")
{
    const nlohmann::json result = placement_of({{0, 0, true, Vec3d(128, 128, 10)},
                                                {1, 1, false, Vec3d(560, 128, 10)},
                                                {2, -1, false, Vec3d(2000, 128, 10)}});
    CHECK(result["on_bed"] == false);
    CHECK(result["plate_indices"] == nlohmann::json::array({0, 1}));
    CHECK(result["instance_placement"][2]["plate_index"].is_null());
    const std::string warning = result["placement_warning"];
    CHECK(warning.find("Instance 1 positioned outside the printable area of plate 1") != std::string::npos);
    CHECK(warning.find("instance 2 is not on any plate") != std::string::npos);
    CHECK(warning.find("Instance 0") == std::string::npos);
}

TEST_CASE("A single-instance object's placement reads as it always has", "[ObjectPlacement][orcamcp]")
{
    CHECK(placement_of({{0, 0, false, Vec3d(250, 128, 10)}})["placement_warning"] ==
          "Object positioned outside the printable area of plate 0");
    CHECK(placement_of({{0, -1, false, Vec3d(2000, 128, 10)}})["placement_warning"] == "Object is not on any plate");

    const nlohmann::json none = placement_of({}); // an object with no instance
    CHECK(none["plate_index"].is_null());
    CHECK(none["plate_indices"].empty());
    CHECK(none["on_bed"] == false);
    CHECK(none["placement_warning"] == "Object is not on any plate");
}

TEST_CASE("Each instance is measured by its own box on the plate it is on", "[ObjectPlacement][orcamcp]")
{
    Slic3r::Model                                     model;
    const std::unique_ptr<Slic3r::GUI::PartPlateList> plates = plate_list_for(model, 2);
    const Vec3d                                       second = centre_of(*plates, 1);
    const Slic3r::ModelObject&                        cube   = add_cube(model, *plates, {centre_of(*plates, 0), second});

    const std::vector<Slic3r::GUI::OrcaMCP::InstancePlacement> placements =
        Slic3r::GUI::OrcaMCP::instance_placements(cube, 0, *plates);
    REQUIRE(placements.size() == 2);
    CHECK(placements[0].plate_index == 0);
    CHECK(placements[0].on_bed);
    CHECK(placements[1].plate_index == 1);
    CHECK(placements[1].on_bed); // the box around both instances fits neither plate
    CHECK_THAT(placements[1].position.x(), Catch::Matchers::WithinAbs(second.x(), 1e-6));
}

TEST_CASE("An instance on no plate is listed as unplaced, even when the object's other instances are on plates",
          "[ObjectPlacement][orcamcp]")
{
    Slic3r::Model                                     model;
    const std::unique_ptr<Slic3r::GUI::PartPlateList> plates = plate_list_for(model, 2);
    const Vec3d                                       off_every_plate(2000.0, 128.0, k_cube_size / 2.0);
    add_cube(model, *plates, {centre_of(*plates, 0), off_every_plate}); // object 0: one copy placed, one not
    add_cube(model, *plates, {centre_of(*plates, 1)});                   // object 1: on its plate
    add_cube(model, *plates, {off_every_plate});                         // object 2: on no plate at all
    REQUIRE(plates->find_instance(0, 1) == -1);

    const nlohmann::json unplaced = Slic3r::GUI::OrcaMCPPlateUtils::UnplacedObjectsJson(model, *plates, {}, false);

    REQUIRE(unplaced.size() == 2);
    CHECK(unplaced[0]["object_index"] == 0);
    CHECK(unplaced[0]["instance_count"] == 2);
    CHECK(unplaced[0]["unplaced_instances"] == nlohmann::json::array({1}));
    CHECK_THAT(unplaced[0]["position"]["x"].get<double>(), // that copy's
               Catch::Matchers::WithinAbs(off_every_plate.x(), 1e-6));
    CHECK(unplaced[1]["object_index"] == 2);
    CHECK(unplaced[1]["unplaced_instances"] == nlohmann::json::array({0}));
}
