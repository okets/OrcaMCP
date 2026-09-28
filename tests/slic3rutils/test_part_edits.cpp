#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "mesh_fixtures.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

TriangleMesh cube_at(double size, const Vec3d& min_corner)
{
    indexed_triangle_set its = its_make_cube(size, size, size);
    return TriangleMesh(mesh_fixtures::translated(its, min_corner.cast<float>()));
}

// A volume holding `mesh` as it is: no re-centring, so its coordinates are the object's.
ModelVolume* add_part(ModelObject& object, const TriangleMesh& mesh, ModelVolumeType type = ModelVolumeType::MODEL_PART)
{
    ModelVolume* volume = object.add_volume(mesh, false);
    volume->set_type(type);
    return volume;
}

// Two 10 mm cubes, part 0 at the origin and part 1 20 mm along the object's X; one instance at
// `offset`, turned `z_degrees` about the plate's Z.
ModelObject& two_part_object(Model& model, const Vec3d& offset = Vec3d(100, 100, 0), double z_degrees = 0.)
{
    ModelObject& object = *model.add_object();
    object.name         = "pair";
    add_part(object, cube_at(10., Vec3d::Zero()));
    add_part(object, cube_at(10., Vec3d(20., 0., 0.)));
    ModelInstance* instance = object.add_instance();
    instance->set_offset(offset);
    instance->set_rotation(Vec3d(0., 0., Geometry::deg2rad(z_degrees)));
    return object;
}

void check_vec(const Vec3d& actual, const Vec3d& expected, double tol = 1e-6)
{
    CHECK_THAT(actual.x(), WithinAbs(expected.x(), tol));
    CHECK_THAT(actual.y(), WithinAbs(expected.y(), tol));
    CHECK_THAT(actual.z(), WithinAbs(expected.z(), tol));
}

} // namespace

// ---- add_volume's primitive: the object list moves the instance's transform into the volumes ----

// Adding a primitive part or modifier moves instance 0's rotation and scale into every volume
// (ObjectList::apply_object_instance_transfrom_to_all_volumes). Upstream then reset instance 0 alone and
// moved every instance by instance 0's offset: on 2026-09-28 a second copy of a 1.5x object jumped from
// (128, 112) to (256, 256), off the bed, and grew to 2.25x.
TEST_CASE("Moving the instance's transform into the volumes keeps every copy of the object where it was", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model, Vec3d(128, 144, 0), 30.);
    object.instances[0]->set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    ModelInstance* second = object.add_instance();
    second->set_offset(Vec3d(128, 80, 0));
    second->set_rotation(Vec3d(0., 0., Geometry::deg2rad(120.)));
    second->set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    std::vector<BoundingBoxf3> before;
    for (std::size_t i = 0; i < object.instances.size(); ++i)
        before.push_back(object.instance_bounding_box(i));

    Slic3r::GUI::bake_instance_transform_into_volumes(object, /*need_update_assemble_matrix=*/false);

    for (std::size_t i = 0; i < object.instances.size(); ++i) {
        INFO("instance " << i);
        check_vec(object.instance_bounding_box(i).min, before[i].min, 1e-4);
        check_vec(object.instance_bounding_box(i).max, before[i].max, 1e-4);
    }
    // Instance 0 keeps only its offset: its rotation and scale are in the volumes now.
    CHECK(object.instances[0]->get_matrix_no_offset().matrix().isApprox(Transform3d::Identity().matrix(), 1e-9));
    check_vec(object.instances[0]->get_offset(), Vec3d(128, 144, 0));
}
