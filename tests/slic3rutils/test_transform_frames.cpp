#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cmath>

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
using Slic3r::GUI::OrcaMCP::transform_instances_in_plate_frame;
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
