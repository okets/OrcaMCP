#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPInstanceBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cmath>

// The exact box of one instance, which every per-plate description of an object is built from
// (get_scene_info, a render's fit, the first-layer plan). ModelObject::instance_bounding_box walks
// every vertex on every call: about 1.1 s per get_scene_info on a 1M-facet mesh in the -O0 dev
// build, paid again on each call though nothing had changed. The cache answers from what it
// computed last as long as nothing the box depends on has changed, and it must never answer a box
// the model no longer has.

using Slic3r::BoundingBoxf3;
using Slic3r::Model;
using Slic3r::ModelInstance;
using Slic3r::ModelObject;
using Slic3r::ModelVolume;
using Slic3r::ModelVolumeType;
using Slic3r::TriangleMesh;
using Slic3r::Vec3d;
using Slic3r::GUI::OrcaMCP::InstanceBoxCache;
using Catch::Matchers::WithinAbs;

namespace {

// Two instances of a two-part object: a 10 x 20 x 30 box and a sphere beside it, plus a modifier,
// which is not printed and so is no part of the box. Every axis has its own length and each instance
// its own rotation, so a box read off the wrong transform shows.
struct Fixture
{
    Model        model;
    ModelObject* object = nullptr;

    Fixture()
    {
        object = model.add_object();
        object->add_volume(TriangleMesh(Slic3r::its_make_cube(10.0, 20.0, 30.0)), false);
        ModelVolume* ball = object->add_volume(TriangleMesh(Slic3r::its_make_sphere(6.0, M_PI / 12.0)), false);
        ball->set_offset(Vec3d(25.0, 0.0, 5.0));
        ModelVolume* modifier = object->add_volume(TriangleMesh(Slic3r::its_make_cube(80.0, 80.0, 80.0)), ModelVolumeType::PARAMETER_MODIFIER, false);
        modifier->set_offset(Vec3d(-40.0, -40.0, -40.0));

        ModelInstance* first = object->add_instance();
        first->set_offset(Vec3d(100.0, 100.0, 0.0));
        first->set_rotation(Vec3d(M_PI / 2.0, 0.0, M_PI / 6.0));
        ModelInstance* second = object->add_instance();
        second->set_offset(Vec3d(40.0, 150.0, 0.0));
        second->set_rotation(Vec3d(0.0, M_PI / 5.0, 0.0));
        second->set_scaling_factor(Vec3d(1.5, 0.5, 2.0));
        second->set_mirror(Vec3d(-1.0, 1.0, 1.0));
    }
};

void check_same_box(const BoundingBoxf3& box, const BoundingBoxf3& expected)
{
    REQUIRE(box.defined == expected.defined);
    for (int axis = 0; axis < 3; ++axis) {
        CHECK_THAT(box.min[axis], WithinAbs(expected.min[axis], 1e-12));
        CHECK_THAT(box.max[axis], WithinAbs(expected.max[axis], 1e-12));
    }
}

} // namespace

TEST_CASE("an instance's box is the one ModelObject::instance_bounding_box gives, however it is turned", "[orcamcp][InstanceBox]")
{
    Fixture         f;
    InstanceBoxCache cache;
    for (size_t i = 0; i < f.object->instances.size(); ++i)
        check_same_box(cache.box(*f.object, i), f.object->instance_bounding_box(i));
}

TEST_CASE("a box read again with nothing changed is not computed again", "[orcamcp][InstanceBox]")
{
    Fixture         f;
    InstanceBoxCache cache;
    cache.box(*f.object, 0);
    const size_t walks = cache.part_walks();
    CHECK(walks == 2); // the two model parts; the modifier is no part of the box

    check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
    CHECK(cache.part_walks() == walks);
}

TEST_CASE("moving one instance computes that instance's box again, and only that one", "[orcamcp][InstanceBox]")
{
    Fixture         f;
    InstanceBoxCache cache;
    cache.box(*f.object, 0);
    cache.box(*f.object, 1);
    const size_t walks = cache.part_walks();

    f.object->instances[1]->set_offset(Vec3d(60.0, 20.0, 3.0));
    check_same_box(cache.box(*f.object, 1), f.object->instance_bounding_box(1));
    check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
    CHECK(cache.part_walks() == walks + 2);
}

TEST_CASE("a part's new mesh, new placement or new type is seen", "[orcamcp][InstanceBox]")
{
    Fixture         f;
    InstanceBoxCache cache;
    cache.box(*f.object, 0);

    SECTION("a new mesh")
    {
        f.object->volumes[1]->set_mesh(TriangleMesh(Slic3r::its_make_cube(40.0, 5.0, 5.0)));
        check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
    }
    SECTION("the part moved within the object")
    {
        f.object->volumes[1]->set_offset(Vec3d(-30.0, 12.0, 0.0));
        check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
    }
    SECTION("the modifier made a part")
    {
        f.object->volumes[2]->set_type(ModelVolumeType::MODEL_PART);
        check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
    }
    SECTION("a part made a modifier")
    {
        f.object->volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
        check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
    }
    SECTION("a part deleted")
    {
        f.object->delete_volume(1);
        check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
    }
}

// Upstream changes a mesh in place in two places, behind the const of its shared pointer: the
// unit conversions scale it (scale_geometry_after_creation) with no transform changing, and
// center_geometry_after_creation moves it. The same mesh at the same address is then another shape.
TEST_CASE("a mesh upstream scales or moves in place is seen", "[orcamcp][InstanceBox]")
{
    Fixture         f;
    InstanceBoxCache cache;
    cache.box(*f.object, 0);

    SECTION("scaled in place")
    {
        f.object->volumes[0]->scale_geometry_after_creation(Slic3r::Vec3f(2.0f, 2.0f, 2.0f));
        check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
    }
    SECTION("centred in place")
    {
        f.object->volumes[0]->center_geometry_after_creation();
        check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
    }
}

// A slice's Print holds copies of the model objects that keep their ids and share their meshes, and
// the first-layer plan reads those.
TEST_CASE("a copy of the object, as a Print holds it, is answered from the same entries", "[orcamcp][InstanceBox]")
{
    Fixture         f;
    InstanceBoxCache cache;
    cache.box(*f.object, 0);
    const size_t walks = cache.part_walks();

    Model copy(f.model);
    check_same_box(cache.box(*copy.objects[0], 0), f.object->instance_bounding_box(0));
    CHECK(cache.part_walks() == walks);

    copy.objects[0]->instances[0]->set_offset(Vec3d(0.0, 0.0, 0.0));
    check_same_box(cache.box(*copy.objects[0], 0), copy.objects[0]->instance_bounding_box(0));
    check_same_box(cache.box(*f.object, 0), f.object->instance_bounding_box(0));
}

TEST_CASE("an object with no model part has no box", "[orcamcp][InstanceBox]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(TriangleMesh(Slic3r::its_make_cube(5.0, 5.0, 5.0)), ModelVolumeType::PARAMETER_MODIFIER, false);
    object->add_instance();
    InstanceBoxCache cache;
    CHECK_FALSE(cache.box(*object, 0).defined);
    CHECK_FALSE(object->instance_bounding_box(0).defined);
}
