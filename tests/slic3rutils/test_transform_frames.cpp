#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

// The frame the transform tools move, rotate, scale and mirror in. The handlers themselves need a
// Plater, but the part that was wrong is pure geometry: whether a displacement written in plate
// millimetres is applied in plate axes or in the object's own, which only differ once the instance
// carries a rotation. Every fixture here is therefore built around an instance rotated 90 degrees
// about X -- the arrangement the bug was found on, where a -84 mm Y move came out as a +84 mm Z
// move and left the part floating 84 mm above the bed.

using Slic3r::BoundingBoxf3;
using Slic3r::Model;
using Slic3r::ModelInstance;
using Slic3r::ModelObject;
using Slic3r::ModelVolume;
using Slic3r::Transform3d;
using Slic3r::TriangleMesh;
using Slic3r::Vec3d;
using Slic3r::GUI::OrcaMCP::InstancesOnPlate;
using Slic3r::GUI::OrcaMCP::instances_on_plate;
using Slic3r::GUI::OrcaMCP::object_world_box;
using Slic3r::GUI::OrcaMCP::should_drop_to_bed;
using Slic3r::GUI::OrcaMCP::transform_instances_in_plate_frame;
using Slic3r::GUI::OrcaMCP::transform_instances_on_bed;
using Catch::Matchers::WithinAbs;
namespace Geometry = Slic3r::Geometry;

namespace {

struct RotatedObject
{
    Model        model;
    ModelObject* object = nullptr;
};

// A 10 x 20 x 30 box whose instance is rotated 90 degrees about X and parked at (100, 100, 0).
// modify_to_center_geometry = false keeps the mesh at its literal coordinates so the world box
// below stays hand-checkable. The instance rotation sends local (x, y, z) to world (x, -z, y), so
// the world box is x[100, 110], y[70, 100], z[0, 20] -- extents 10 x 30 x 20, which is the point:
// every axis has a different length, so a transform applied along the wrong one is visible.
RotatedObject make_rotated_box(const Vec3d& instance_offset = Vec3d(100, 100, 0))
{
    RotatedObject built;
    built.object = built.model.add_object();
    const TriangleMesh mesh(Slic3r::its_make_cube(10.0, 20.0, 30.0));
    built.object->add_volume(mesh, false);
    ModelInstance* instance = built.object->add_instance();
    instance->set_offset(instance_offset);
    instance->set_rotation(Vec3d(M_PI / 2.0, 0.0, 0.0));
    return built;
}

BoundingBoxf3 world_box(const ModelObject& object) { return object.instance_bounding_box(0); }

void check_vec(const Vec3d& actual, const Vec3d& expected, double tol = 1e-9)
{
    CHECK_THAT(actual.x(), WithinAbs(expected.x(), tol));
    CHECK_THAT(actual.y(), WithinAbs(expected.y(), tol));
    CHECK_THAT(actual.z(), WithinAbs(expected.z(), tol));
}

void check_box(const BoundingBoxf3& box, const Vec3d& min, const Vec3d& max, double tol = 1e-9)
{
    CHECK_THAT(box.min.x(), WithinAbs(min.x(), tol));
    CHECK_THAT(box.min.y(), WithinAbs(min.y(), tol));
    CHECK_THAT(box.min.z(), WithinAbs(min.z(), tol));
    CHECK_THAT(box.max.x(), WithinAbs(max.x(), tol));
    CHECK_THAT(box.max.y(), WithinAbs(max.y(), tol));
    CHECK_THAT(box.max.z(), WithinAbs(max.z(), tol));
}

} // namespace

TEST_CASE("the fixture's world box is where the instance rotation puts it", "[transform_frames]")
{
    // If this drifts, every expectation below is measured against the wrong starting point.
    RotatedObject built = make_rotated_box();
    check_box(world_box(*built.object), Vec3d(100, 70, 0), Vec3d(110, 100, 20));
}

TEST_CASE("a move is applied in plate axes, not the object's own", "[transform_frames]")
{
    RotatedObject built = make_rotated_box();

    // The reproduction's displacement: y = -170 requested from y = -85.88, i.e. -84.12 mm of Y.
    built.object->translate_instances(Vec3d(0.0, -84.12, 0.0));

    const BoundingBoxf3 box = world_box(*built.object);
    check_box(box, Vec3d(100, 70 - 84.12, 0), Vec3d(110, 100 - 84.12, 20), 1e-9);
    // The half that mattered on the real project: Z did not move.
    CHECK_THAT(box.min.z(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("ModelObject::translate is the wrong instrument for a plate-frame move", "[transform_frames]")
{
    // This is what move_object called before. It is kept as a test rather than a comment because it
    // is the whole reason the line above says translate_instances: translating the volumes puts the
    // displacement under the instance rotation, which turns a Y request into a Z one.
    RotatedObject built = make_rotated_box();
    const BoundingBoxf3 before = world_box(*built.object);

    built.object->translate(Vec3d(0.0, -84.12, 0.0));

    const BoundingBoxf3 after = world_box(*built.object);
    CHECK_THAT(after.min.y() - before.min.y(), WithinAbs(0.0, 1e-9));           // Y never moved
    CHECK_THAT(std::abs(after.min.z() - before.min.z()), WithinAbs(84.12, 1e-9)); // Z did, by the Y amount
}

TEST_CASE("a rotation turns about the plate's axis, about the object's centre", "[transform_frames]")
{
    RotatedObject built = make_rotated_box();
    const Vec3d centre = world_box(*built.object).center();

    // 90 degrees about the plate's Z, the vertical. The world extents are 10 x 30 x 20, so a true
    // vertical turn swaps X and Y and leaves Z alone: 30 x 10 x 20, centred where it was.
    transform_instances_in_plate_frame(*built.object, Geometry::rotation_transform(Vec3d(0, 0, M_PI / 2.0)));

    const BoundingBoxf3 box = world_box(*built.object);
    CHECK_THAT(box.size().x(), WithinAbs(30.0, 1e-9));
    CHECK_THAT(box.size().y(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(box.size().z(), WithinAbs(20.0, 1e-9));
    CHECK_THAT(box.center().x(), WithinAbs(centre.x(), 1e-9));
    CHECK_THAT(box.center().y(), WithinAbs(centre.y(), 1e-9));
    CHECK_THAT(box.center().z(), WithinAbs(centre.z(), 1e-9));
}

TEST_CASE("ModelObject::rotate turns about the object's own axis instead", "[transform_frames]")
{
    // What rotate_object called before: the same nominal "rotate 90 about Z" leaves the vertical
    // extent changed, because the object's local Z is horizontal in the world here.
    RotatedObject built = make_rotated_box();

    built.object->rotate(M_PI / 2.0, Slic3r::Axis::Z);

    const BoundingBoxf3 box = world_box(*built.object);
    CHECK_THAT(box.size().z(), WithinAbs(10.0, 1e-9));  // was 20 -- a vertical turn cannot do this
    CHECK_THAT(box.size().y(), WithinAbs(30.0, 1e-9));
}

TEST_CASE("rotating the instance is what keeps rotation_degrees honest", "[transform_frames]")
{
    // The response of rotate_object reports instances[0]->get_rotation(). Turning the volumes left
    // that untouched, so the number a caller read back never reflected what they asked for.
    RotatedObject built = make_rotated_box();
    const Vec3d before = built.object->instances[0]->get_rotation();

    built.object->rotate(M_PI / 2.0, Slic3r::Axis::Z);
    CHECK(built.object->instances[0]->get_rotation().isApprox(before));

    RotatedObject turned = make_rotated_box();
    transform_instances_in_plate_frame(*turned.object, Geometry::rotation_transform(Vec3d(0, 0, M_PI / 2.0)));
    CHECK_FALSE(turned.object->instances[0]->get_rotation().isApprox(before));
}

TEST_CASE("a non-uniform scale stretches along the plate's axis", "[transform_frames]")
{
    RotatedObject built = make_rotated_box();
    const Vec3d centre = world_box(*built.object).center();

    // Double the plate's Y. World extents 10 x 30 x 20 become 10 x 60 x 20.
    transform_instances_in_plate_frame(*built.object, Geometry::scale_transform(Vec3d(1.0, 2.0, 1.0)));

    const BoundingBoxf3 box = world_box(*built.object);
    CHECK_THAT(box.size().x(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(box.size().y(), WithinAbs(60.0, 1e-9));
    CHECK_THAT(box.size().z(), WithinAbs(20.0, 1e-9));
    CHECK_THAT(box.center().y(), WithinAbs(centre.y(), 1e-9));
}

TEST_CASE("ModelObject::scale stretches along the object's own axis instead", "[transform_frames]")
{
    // What scale_object called before: the same nominal "double Y" doubles the object's height.
    RotatedObject built = make_rotated_box();

    built.object->scale(Vec3d(1.0, 2.0, 1.0));

    const BoundingBoxf3 box = world_box(*built.object);
    CHECK_THAT(box.size().y(), WithinAbs(30.0, 1e-9));  // untouched
    CHECK_THAT(box.size().z(), WithinAbs(40.0, 1e-9));  // doubled, and nobody asked
}

TEST_CASE("a scale leaves the instance's reported scaling factor true", "[transform_frames]")
{
    RotatedObject built = make_rotated_box();
    built.object->scale(Vec3d(2.0, 2.0, 2.0));
    // The old path: the object is twice the size and the number the tool reports still says 1.
    CHECK_THAT(built.object->instances[0]->get_scaling_factor().x(), WithinAbs(1.0, 1e-9));

    RotatedObject scaled = make_rotated_box();
    transform_instances_in_plate_frame(*scaled.object, Geometry::scale_transform(Vec3d(2.0, 2.0, 2.0)));
    CHECK_THAT(scaled.object->instances[0]->get_scaling_factor().x(), WithinAbs(2.0, 1e-9));
}

TEST_CASE("a mirror reflects across a plate plane through the object's centre", "[transform_frames]")
{
    RotatedObject built = make_rotated_box();
    const BoundingBoxf3 before = world_box(*built.object);

    // Mirror across the plate's Z: the box is unmoved, but the instance is now left-handed.
    CHECK_FALSE(built.object->instances[0]->is_left_handed());
    transform_instances_in_plate_frame(*built.object, Geometry::scale_transform(Vec3d(1.0, 1.0, -1.0)));

    check_box(world_box(*built.object), before.min, before.max, 1e-9);
    CHECK(built.object->instances[0]->is_left_handed());
}

TEST_CASE("ModelObject::mirror moves the object as a side effect", "[transform_frames]")
{
    // What mirror_object called before: the mesh is reflected about the volume origin, not the
    // object's centre, so the object jumps by its own width -- here, in the wrong axis too.
    RotatedObject built = make_rotated_box();
    const BoundingBoxf3 before = world_box(*built.object);

    built.object->mirror(Slic3r::Axis::Z);

    const BoundingBoxf3 after = world_box(*built.object);
    CHECK_THAT(std::abs(after.center().y() - before.center().y()), WithinAbs(30.0, 1e-9));
    CHECK_THAT(after.center().z() - before.center().z(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("every instance of an object is transformed, each about its own centre", "[transform_frames]")
{
    // move_object, rotate_object and the rest address an object, not an instance, and report one
    // position for it -- so leaving a sibling instance behind would be its own surprise. A rotation
    // turns each copy where it stands rather than swinging the pair about a shared centre, which is
    // what the GUI's synchronize_unselected_instances does.
    RotatedObject built = make_rotated_box();
    ModelInstance* second = built.object->add_instance(*built.object->instances[0]);
    second->set_offset(Vec3d(300.0, 100.0, 0.0));

    const Vec3d first_centre  = built.object->instance_bounding_box(0).center();
    const Vec3d second_centre = built.object->instance_bounding_box(1).center();

    transform_instances_in_plate_frame(*built.object, Geometry::rotation_transform(Vec3d(0, 0, M_PI / 2.0)));

    const BoundingBoxf3 first  = built.object->instance_bounding_box(0);
    const BoundingBoxf3 second_box = built.object->instance_bounding_box(1);
    CHECK_THAT(first.center().x(), WithinAbs(first_centre.x(), 1e-9));
    CHECK_THAT(second_box.center().x(), WithinAbs(second_centre.x(), 1e-9));
    CHECK_THAT(first.size().x(), WithinAbs(30.0, 1e-9));
    CHECK_THAT(second_box.size().x(), WithinAbs(30.0, 1e-9));

    // A translation moves both by the same amount, so the pair keeps its spacing.
    built.object->translate_instances(Vec3d(0.0, -50.0, 0.0));
    CHECK_THAT(built.object->instance_bounding_box(1).center().x() -
                   built.object->instance_bounding_box(0).center().x(),
               WithinAbs(200.0, 1e-9));
    CHECK_THAT(built.object->instance_bounding_box(0).center().y(),
               WithinAbs(first_centre.y() - 50.0, 1e-9));
}

// Staying on the bed. The GUI drops an instance back onto Z = 0 after every scale, rotate and
// mirror unless it was already sinking (GLCanvas3D::do_scale / do_rotate / do_mirror); the MCP
// tools used to leave Z wherever the centre-pivoted transform put it, which is how a uniform scale
// of 1.49 left a figurine's feet 24 mm below the bed.

TEST_CASE("the bed drop follows the GUI's sinking rule", "[transform_frames]")
{
    CHECK(should_drop_to_bed(0.0, -12.0));    // resting, pushed under the bed: back up
    CHECK(should_drop_to_bed(0.0, 5.0));      // resting, lifted off it: back down
    CHECK(should_drop_to_bed(10.0, 20.0));    // floating before counts as not sinking
    CHECK_FALSE(should_drop_to_bed(-3.0, -8.0)); // sinking stays sinking
    CHECK(should_drop_to_bed(-3.0, 4.0));     // ... unless the transform lifted it clear of the bed
    CHECK_FALSE(should_drop_to_bed(0.0, 0.0)); // already there
}

TEST_CASE("a scale about the centre leaves a resting object on the bed", "[transform_frames]")
{
    // World box z[0, 20]; 1.5x about its centre (z = 10) would reach z = -5.
    RotatedObject built = make_rotated_box();
    transform_instances_on_bed(*built.object, Geometry::scale_transform(Vec3d(1.5, 1.5, 1.5)));

    const BoundingBoxf3 box = world_box(*built.object);
    CHECK_THAT(box.min.z(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(box.size().z(), WithinAbs(30.0, 1e-6));
}

TEST_CASE("a rotation leaves a resting object on the bed", "[transform_frames]")
{
    // 90 degrees about the plate's X swaps the 30 mm Y extent into Z: about the centre (z = 10)
    // that spans z[-5, 25] until the drop.
    RotatedObject built = make_rotated_box();
    transform_instances_on_bed(*built.object, Geometry::rotation_transform(Vec3d(M_PI / 2.0, 0, 0)));

    const BoundingBoxf3 box = world_box(*built.object);
    CHECK_THAT(box.min.z(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(box.size().z(), WithinAbs(30.0, 1e-6));
}

TEST_CASE("a mirror leaves a resting object on the bed", "[transform_frames]")
{
    RotatedObject built = make_rotated_box();
    transform_instances_on_bed(*built.object, Geometry::scale_transform(Vec3d(1.0, 1.0, -1.0)));
    CHECK_THAT(world_box(*built.object).min.z(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("a sinking object is left sinking by a transform", "[transform_frames]")
{
    // Sunk 5 mm on purpose: z[-5, 15]. A 1.5x scale about z = 5 gives z[-10, 20], and stays there.
    RotatedObject built = make_rotated_box(Vec3d(100, 100, -5));
    transform_instances_on_bed(*built.object, Geometry::scale_transform(Vec3d(1.5, 1.5, 1.5)));
    CHECK_THAT(world_box(*built.object).min.z(), WithinAbs(-10.0, 1e-6));
}

TEST_CASE("an instance with auto_drop off is not dropped", "[transform_frames]")
{
    RotatedObject built = make_rotated_box();
    built.object->instances[0]->auto_drop = false;
    transform_instances_on_bed(*built.object, Geometry::scale_transform(Vec3d(1.5, 1.5, 1.5)));
    CHECK_THAT(world_box(*built.object).min.z(), WithinAbs(-5.0, 1e-6));
}

TEST_CASE("each instance is dropped by its own amount", "[transform_frames]")
{
    RotatedObject built = make_rotated_box();
    ModelInstance* sunk = built.object->add_instance(*built.object->instances[0]);
    sunk->set_offset(Vec3d(300.0, 100.0, -5.0));

    transform_instances_on_bed(*built.object, Geometry::scale_transform(Vec3d(1.5, 1.5, 1.5)));

    CHECK_THAT(built.object->instance_bounding_box(0).min.z(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(built.object->instance_bounding_box(1).min.z(), WithinAbs(-10.0, 1e-6));
}

namespace {

// A T: a 10 x 10 x 40 stem under a 30 x 10 x 5 bar, tilted 30 degrees about the plate's Y and
// dropped onto the bed. Its approximate box -- the mesh's own box, turned with the instance -- has
// corners where the T has no material, below its lowest real point and wider than it.
RotatedObject make_tilted_t()
{
    RotatedObject built;
    built.object = built.model.add_object();
    TriangleMesh mesh(Slic3r::its_make_cube(10.0, 10.0, 40.0));
    TriangleMesh bar(Slic3r::its_make_cube(30.0, 10.0, 5.0));
    mesh.translate(10.0, 0.0, 0.0);
    bar.translate(0.0, 0.0, 40.0);
    mesh.merge(bar);
    built.object->add_volume(mesh, false);
    built.object->add_instance()->set_offset(Vec3d(100, 100, 0));
    transform_instances_on_bed(*built.object, Geometry::rotation_transform(Vec3d(0, M_PI / 6.0, 0)));
    return built;
}

} // namespace

TEST_CASE("a tilted part dropped onto the bed touches it by its exact box", "[transform_frames]")
{
    RotatedObject built = make_tilted_t();
    CHECK_THAT(object_world_box(*built.object).min.z(), WithinAbs(0.0, 1e-6));
    CHECK(built.object->bounding_box_approx().min.z() < -1.0);  // why no tool may report this one
    CHECK(Slic3r::GUI::OrcaMCP::object_within_plate(object_world_box(*built.object),
                                                    BoundingBoxf3(Vec3d(0, 0, 0), Vec3d(256, 256, 256))));
}

TEST_CASE("a rotated object is reported by its exact box", "[transform_frames]")
{
    // get_object_info, get_scene_info and load_model all describe an object through
    // model_object_summary_json, and the occupancy footprint through GetObjectFootprint. On the
    // tilted T both used to report the approximate box: min z below the bed while it rests on it.
    RotatedObject       built = make_tilted_t();
    const BoundingBoxf3 exact = object_world_box(*built.object);

    const nlohmann::json summary = Slic3r::GUI::OrcaMCP::model_object_summary_json(*built.object, 0);
    CHECK_THAT(summary["bounding_box"]["min"]["z"].get<double>(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(summary["bounding_box"]["min"]["x"].get<double>(), WithinAbs(exact.min.x(), 1e-9));
    CHECK_THAT(summary["bounding_box"]["size_x"].get<double>(), WithinAbs(exact.size().x(), 1e-9));
    CHECK_THAT(summary["position"]["z"].get<double>(), WithinAbs(exact.center().z(), 1e-9));

    const Slic3r::GUI::ObjectFootprint footprint =
        Slic3r::GUI::OrcaMCPPlateUtils::GetObjectFootprint(*built.object, exact, Slic3r::DynamicPrintConfig());
    CHECK_THAT(footprint.body.min.x(), WithinAbs(exact.min.x(), 1e-9));
    CHECK_THAT(footprint.body.max.x(), WithinAbs(exact.max.x(), 1e-9));
    CHECK(footprint.body.size().x() < built.object->bounding_box_approx().size().x() - 1.0);
}

// One object, instances on several plates. Every per-plate description of it -- get_scene_info's
// entry, its occupancy footprint, a render's fit -- is built from the instances that plate holds.
// get_scene_info used to give the box spanning them all, so plate 0's footprint for a part with a
// copy on plate 1 was 347 mm wide.

namespace {

// A 10 x 20 x 30 box with instances at x = 100, 400 and 150 (the second on "plate 1").
RotatedObject make_three_instances()
{
    RotatedObject built;
    built.object = built.model.add_object();
    built.object->add_volume(TriangleMesh(Slic3r::its_make_cube(10.0, 20.0, 30.0)), false);
    for (double x : {100.0, 400.0, 150.0})
        built.object->add_instance()->set_offset(Vec3d(x, 100.0, 0.0));
    return built;
}

} // namespace

TEST_CASE("an object's instances on one plate are the ones that plate holds", "[transform_frames]")
{
    RotatedObject built = make_three_instances();
    const auto on_plate_0 = [](int instance) { return instance != 1; };

    const InstancesOnPlate here = instances_on_plate(*built.object, on_plate_0);
    CHECK(here.ids == std::vector<int>{0, 2});
    REQUIRE(here.box.defined);
    check_box(here.box, Vec3d(100, 100, 0), Vec3d(160, 120, 30));

    const InstancesOnPlate there = instances_on_plate(*built.object, [](int instance) { return instance == 1; });
    CHECK(there.ids == std::vector<int>{1});
    check_box(there.box, Vec3d(400, 100, 0), Vec3d(410, 120, 30));

    const InstancesOnPlate nowhere = instances_on_plate(*built.object, [](int) { return false; });
    CHECK(nowhere.ids.empty());
    CHECK_FALSE(nowhere.box.defined);
}

TEST_CASE("an object's per-plate entry and footprint cover that plate's instances only", "[transform_frames]")
{
    RotatedObject built = make_three_instances();
    const InstancesOnPlate here = instances_on_plate(*built.object, [](int instance) { return instance != 1; });

    const nlohmann::json entry = Slic3r::GUI::OrcaMCP::model_object_summary_json(*built.object, 0, here);
    CHECK_THAT(entry["bounding_box"]["max"]["x"].get<double>(), WithinAbs(160.0, 1e-9));  // not 410
    CHECK_THAT(entry["position"]["x"].get<double>(), WithinAbs(130.0, 1e-9));
    CHECK(entry["instance_count"] == 3);  // the object's, as get_object_info reports it
    CHECK(entry["instances_on_plate"] == nlohmann::json({0, 2}));

    const Slic3r::GUI::ObjectFootprint footprint =
        Slic3r::GUI::OrcaMCPPlateUtils::GetObjectFootprint(*built.object, here.box, Slic3r::DynamicPrintConfig());
    CHECK_THAT(footprint.body.min.x(), WithinAbs(100.0, 1e-9));
    CHECK_THAT(footprint.body.max.x(), WithinAbs(160.0, 1e-9));
}

TEST_CASE("a plate's entry for an object takes its transform from a copy on that plate", "[transform_frames]")
{
    // Instance 0 stands on plate 0 unrotated; the copy on plate 1 is turned 90 degrees and scaled.
    // Plate 1's entry used to report instance 0's rotation and scale, from the other plate.
    RotatedObject built = make_three_instances();
    built.object->instances[1]->set_rotation(Vec3d(0.0, 0.0, M_PI / 2.0));
    built.object->instances[1]->set_scaling_factor(Vec3d(2.0, 2.0, 2.0));
    const InstancesOnPlate there = instances_on_plate(*built.object, [](int instance) { return instance == 1; });

    const nlohmann::json plate_1 = Slic3r::GUI::OrcaMCP::model_object_summary_json(*built.object, 0, there);
    CHECK_THAT(plate_1["rotation_degrees"]["z"].get<double>(), WithinAbs(90.0, 1e-9));
    CHECK_THAT(plate_1["scale"]["x"].get<double>(), WithinAbs(2.0, 1e-9));

    // The object-wide description (get_object_info, load_model) stays instance 0's.
    const nlohmann::json object_wide = Slic3r::GUI::OrcaMCP::model_object_summary_json(*built.object, 0);
    CHECK_THAT(object_wide["rotation_degrees"]["z"].get<double>(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(object_wide["scale"]["x"].get<double>(), WithinAbs(1.0, 1e-9));
}

// transform_objects applied each entry as it read it, so [{0, position}, {0, scale 0}] moved object 0,
// then rejected the scale, and its results loop left out every entry for object 0: the caller heard only
// of the error, and the moved object was never re-homed onto the plate it now stood on. Every entry is
// read and checked first now, and the batch is applied only when none is rejected.
TEST_CASE("transform_objects checks every entry before it applies any", "[orcamcp][transform_frames]")
{
    using Slic3r::GUI::OrcaMCP::read_transform_entries;
    using Slic3r::GUI::OrcaMCP::TransformEntry;

    const nlohmann::json moved_then_bad_scale = nlohmann::json::parse(
        R"([{"object_id": 0, "position": {"x": 200}}, {"object_id": 0, "scale": {"x": 0}}])");
    const std::vector<TransformEntry> entries = read_transform_entries(moved_then_bad_scale, /*object_count=*/1);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].error.empty());
    CHECK(entries[1].error.find("Scale factors must be positive") != std::string::npos);

    const nlohmann::json bad_id = nlohmann::json::parse(R"([{"object_id": 3, "rotation": {"z": 90}}, {"object_id": -1}])");
    for (const TransformEntry& entry : read_transform_entries(bad_id, 1))
        CHECK(entry.error == "Invalid object_id");

    const nlohmann::json valid = nlohmann::json::parse(
        R"([{"object_id": 0, "scale": {"uniform": 2}}, {"object_id": 1, "scale": {"y": 0.5}}, {"object_id": 1}])");
    const std::vector<TransformEntry> read = read_transform_entries(valid, 2);
    REQUIRE(read.size() == 3);
    for (const TransformEntry& entry : read)
        CHECK(entry.error.empty());
    check_vec(read[0].scale, Vec3d(2, 2, 2));
    check_vec(read[1].scale, Vec3d(1, 0.5, 1));
    check_vec(read[2].scale, Vec3d::Ones());
    CHECK(read[1].object_id == 1);
}

// The apply loop read position and rotation from the JSON itself, after the entries before it were
// applied: {"rotation": {"z": "90"}} threw "type must be number" halfway through the batch, and the
// object an earlier entry moved stayed moved, never re-homed. Every value is read, and a value of the
// wrong kind rejected, before anything is applied.
TEST_CASE("an entry with a value of the wrong kind is rejected before anything is applied", "[orcamcp][transform_frames]")
{
    using Slic3r::GUI::OrcaMCP::read_transform_entries;
    using Slic3r::GUI::OrcaMCP::TransformEntry;
    const auto error_of = [](const char* entry_json) {
        const nlohmann::json batch = nlohmann::json::array({nlohmann::json::parse(entry_json)});
        return read_transform_entries(batch, /*object_count=*/2).front().error;
    };

    const nlohmann::json reviewer = nlohmann::json::parse(
        R"([{"object_id": 0, "position": {"x": 100}}, {"object_id": 1, "rotation": {"z": "90"}}])");
    const std::vector<TransformEntry> entries = read_transform_entries(reviewer, 2);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].error.empty());
    CHECK(entries[1].error.find("rotation.z must be a number") != std::string::npos);

    CHECK(error_of(R"({"object_id": 0, "position": {"x": null}})").find("position.x must be a number") != std::string::npos);
    CHECK(error_of(R"({"object_id": 0, "position": {"y": [1]}})").find("position.y must be a number") != std::string::npos);
    CHECK(error_of(R"({"object_id": 0, "position": [1, 2]})").find("position must be an object") != std::string::npos);
    CHECK(error_of(R"({"object_id": 0, "rotation": 90})").find("rotation must be an object") != std::string::npos);
    CHECK(error_of(R"({"object_id": 0, "scale": 2})").find("scale must be an object") != std::string::npos);
    CHECK(error_of(R"({"object_id": 0, "scale": {"uniform": "2"}})").find("scale.uniform must be a number") != std::string::npos);
    CHECK(error_of(R"({"object_id": 0, "scale": {"z": true}})").find("scale.z must be a number") != std::string::npos);
    CHECK(error_of(R"({"object_id": "0"})").find("object_id must be an integer") != std::string::npos);
    CHECK(error_of(R"({"position": {"x": 1}})").find("object_id is missing") != std::string::npos);
    CHECK(error_of("5").find("must be an object") != std::string::npos);
}

TEST_CASE("an entry's position, rotation and scale are read whole", "[orcamcp][transform_frames]")
{
    using Slic3r::GUI::OrcaMCP::read_transform_entries;
    const nlohmann::json batch = nlohmann::json::parse(
        R"([{"object_id": 0, "position": {"x": 10, "z": 2}, "rotation": {"z": 90}, "scale": {"x": 2}}])");
    const auto entry = read_transform_entries(batch, 1).front();
    CHECK(entry.error.empty());
    REQUIRE(entry.position.axis[0].has_value());
    CHECK_THAT(*entry.position.axis[0], WithinAbs(10.0, 1e-12));
    CHECK_FALSE(entry.position.axis[1].has_value());
    REQUIRE(entry.position.axis[2].has_value());
    CHECK_THAT(*entry.position.axis[2], WithinAbs(2.0, 1e-12));
    // An axis the entry does not give stays where the object is.
    check_vec(entry.position.value_or(Vec3d(1, 5, 9)), Vec3d(10, 5, 2));
    check_vec(entry.rotation, Vec3d(0, 0, 90));
    check_vec(entry.scale, Vec3d(2, 1, 1));
}

// transform_objects {} read params["transforms"] from a const json that has none: undefined behaviour in
// a release build (nlohmann only asserts, then dereferences end()).
TEST_CASE("transform_objects without a transforms array is an error naming it", "[orcamcp][transform_frames]")
{
    using Slic3r::GUI::OrcaMCP::transforms_argument_error;
    const auto missing = transforms_argument_error(nlohmann::json::object());
    REQUIRE(missing.has_value());
    CHECK(missing->find("transforms is required") != std::string::npos);
    const auto not_array = transforms_argument_error(nlohmann::json::parse(R"({"transforms": {"object_id": 0}})"));
    REQUIRE(not_array.has_value());
    CHECK(not_array->find("transforms must be an array") != std::string::npos);
    CHECK_FALSE(transforms_argument_error(nlohmann::json::parse(R"({"transforms": []})")).has_value());
}

// move_object, rotate_object and scale_object read the same x, y, z, with the same reader.
TEST_CASE("the transform tools' axes are numbers, each optional", "[orcamcp][transform_frames]")
{
    using Slic3r::GUI::OrcaMCP::PlateAxes;
    using Slic3r::GUI::OrcaMCP::read_plate_axes;
    PlateAxes axes;
    CHECK_FALSE(read_plate_axes(nlohmann::json::parse(R"({"x": 1, "relative": false})"), "", axes).has_value());
    check_vec(axes.value_or(Vec3d::Zero()), Vec3d(1, 0, 0));
    const auto error = read_plate_axes(nlohmann::json::parse(R"({"y": "a"})"), "", axes);
    REQUIRE(error.has_value());
    CHECK(error->find("y must be a number") != std::string::npos);
}

TEST_CASE("a scale is valid only with positive, finite factors", "[orcamcp][transform_frames]")
{
    using Slic3r::GUI::OrcaMCP::valid_scale_factors;
    CHECK(valid_scale_factors(Vec3d(1, 2, 0.5)));
    CHECK_FALSE(valid_scale_factors(Vec3d(1, 0, 1)));
    CHECK_FALSE(valid_scale_factors(Vec3d(-1, 1, 1)));
    CHECK_FALSE(valid_scale_factors(Vec3d(1, 1, std::nan(""))));
    CHECK_FALSE(valid_scale_factors(Vec3d(INFINITY, 1, 1)));
}

TEST_CASE("flatten_object orients an object only when the orient job would orient that object alone",
          "[orcamcp][transform_frames]")
{
    // The job orients the selection, and an empty selection orients every object: an object it leaves
    // out of the selection would have every other object turned in its place.
    using Slic3r::GUI::OrcaMCP::flatten_refusal;
    using Locked = std::vector<int>;
    CHECK_FALSE(flatten_refusal(2, /*printable=*/true, /*instances=*/2, /*on_locked_plates=*/Locked{}, /*job_running=*/false).has_value());
    CHECK(flatten_refusal(2, true, 1, Locked{}, /*job_running=*/true) ==
          "another job (an arrange, an orient or a bed fill) is running: poll get_slicing_status until ui_job is null, then call "
          "flatten_object again");
    CHECK(flatten_refusal(2, /*printable=*/false, 1, Locked{}, false) ==
          "object 2 is marked not printable, and only printable objects are oriented: turn it with rotate_object instead");
    CHECK(flatten_refusal(2, true, /*instances=*/0, Locked{}, false) == "object 2 has no instance to orient");
    CHECK(flatten_refusal(2, true, /*instances=*/2, /*on_locked_plates=*/Locked{0, 1}, false) ==
          "object 2 is on a locked plate, which is never oriented: unlock the plate, or turn it with rotate_object");
}

TEST_CASE("flatten_object refuses an object only some of whose instances are on a locked plate", "[orcamcp][transform_frames]")
{
    // The job turns the unlocked instances, then drops the whole object by its first instance's new
    // bottom: every instance moves by that, the locked ones too, and one can end up in the bed.
    using Slic3r::GUI::OrcaMCP::flatten_refusal;
    CHECK(flatten_refusal(2, true, /*instances=*/3, /*on_locked_plates=*/std::vector<int>{1, 2}, false) ==
          "object 2 has instances 1, 2 on a locked plate, which the orient would move up or down with the others "
          "without turning them: unlock their plate, or move them to another plate, then call flatten_object again");
    CHECK(flatten_refusal(2, true, /*instances=*/2, /*on_locked_plates=*/std::vector<int>{1}, false) ==
          "object 2 has instance 1 on a locked plate, which the orient would move up or down with the others "
          "without turning it: unlock its plate, or move it to another plate, then call flatten_object again");
}

TEST_CASE("flatten_object starts the orient job only on a selection of exactly the named object", "[orcamcp][transform_frames]")
{
    // A 3D view whose reload is still postponed has no volumes for a new object: selecting it leaves
    // the selection empty, and the orient job, finding nothing selected, orients every object.
    using Slic3r::GUI::OrcaMCP::flatten_selection_refusal;
    using Selected = std::map<int, std::set<int>>;
    CHECK_FALSE(flatten_selection_refusal(2, 2, Selected{{2, {0, 1}}}).has_value());
    const std::string not_in_view =
        "the 3D view has not caught up with object 2 yet, so the orient would not be this object alone, and was "
        "not started: show the Prepare tab, then call flatten_object again";
    CHECK(flatten_selection_refusal(2, 2, Selected{}) == not_in_view);
    CHECK(flatten_selection_refusal(2, 2, Selected{{2, {0}}}) == not_in_view);
    CHECK(flatten_selection_refusal(2, 1, Selected{{1, {0}}}) == not_in_view);
    CHECK(flatten_selection_refusal(2, 1, Selected{{1, {0}}, {2, {0}}}) == not_in_view);
}
