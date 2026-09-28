#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/Utils/FixModelByCgal.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "mesh_fixtures.hpp"

#include <algorithm>
#include <string>
#include <vector>

// The object list's Repair, split in two so no thread but the main one changes the model: a plan
// worked out on the meshes alone, and upstream's loop applying it. The object list's Repair, the cut
// gizmo and MCP's repair_mesh all run it, so what is checked here is what each of them does. The
// meshes are built with known defects: a hole, a flat stray sheet, a closed stray shell.

using namespace Slic3r;
using namespace mesh_fixtures;
using Catch::Matchers::WithinAbs;

namespace {

const std::function<bool()> never = [] { return false; };

CgalRepairPlan planned(const ModelObject& object, int volume_idx = -1)
{
    return plan_cgal_repair(capture_cgal_repair(object, volume_idx), {}, never);
}

CgalRepairResult repaired(ModelObject& object, int volume_idx = -1, bool keep_painting = false)
{
    CgalRepairPlan plan = planned(object, volume_idx);
    return apply_cgal_repair(object, keep_painting, plan);
}

int open_edges(const ModelObject& object) { return object.get_object_stl_stats().open_edges; }

RepairedMeshErrors reversed_facets(int count)
{
    RepairedMeshErrors errors;
    errors.facets_reversed = count;
    return errors;
}

// Every facet of the object's first volume painted with filament 2.
void paint_first_volume(ModelObject& object)
{
    ModelVolume&     volume = *object.volumes.front();
    TriangleSelector selector(volume.mesh());
    for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i)
        selector.set_facet(int(i), EnforcerBlockerType::Extruder2);
    volume.mmu_segmentation_facets.set(selector);
}

void check_same_box(const BoundingBoxf3& actual, const BoundingBoxf3& expected)
{
    for (int axis = 0; axis < 3; ++axis) {
        CHECK_THAT(actual.min[axis], WithinAbs(expected.min[axis], 1e-4));
        CHECK_THAT(actual.max[axis], WithinAbs(expected.max[axis], 1e-4));
    }
}

indexed_triangle_set fixture_mesh(const std::string& name)
{
    if (name == "hole")
        return cube_missing_facet();
    if (name == "flat sheet")
        return cube_with(flat_square());
    if (name == "closed stray shell")
        return cube_with(its_make_cube(1.0, 1.0, 1.0));
    if (name == "holed stray shell")
        return cube_with(cube_missing_facet());
    return its_make_cube(10.0, 10.0, 10.0);
}

} // namespace

TEST_CASE("A hole is closed, and the object keeps its one volume", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};
    REQUIRE(open_edges(*f.object) == 3);

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.parts_repaired == 1);
    CHECK(plan.parts_split == 0);
    CHECK(plan.parts_dropped == 0);
    CHECK_FALSE(plan.changes_nothing());

    const CgalRepairResult result = apply_cgal_repair(*f.object, false, plan);
    CHECK(result.error.empty());
    CHECK(result.parts_repaired_here == 0);
    CHECK(f.object->volumes.size() == 1);
    CHECK(open_edges(*f.object) == 0);
    // The repaired mesh records no repairs, so the object list shows no warning for it.
    CHECK_FALSE(f.object->get_object_stl_stats().repaired());
}

TEST_CASE("A flat stray sheet is dropped, and the rest stays where it was", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_with(flat_square()))};
    REQUIRE(open_edges(*f.object) == 4);
    REQUIRE(f.object->volumes.front()->mesh().stats().number_of_parts == 2);

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.parts_split == 2);
    CHECK(plan.parts_dropped == 1);
    CHECK(plan.parts_repaired == 0);

    const CgalRepairResult result = apply_cgal_repair(*f.object, false, plan);
    CHECK(result.error.empty());
    REQUIRE(f.object->volumes.size() == 1);
    CHECK(open_edges(*f.object) == 0);
    CHECK(f.object->volumes.front()->mesh().stats().number_of_parts == 1);
    // The cube's volume transform moved into the instance; on the plate the cube did not move.
    check_same_box(f.object->bounding_box_exact(), BoundingBoxf3(Vec3d::Zero(), Vec3d(10., 10., 10.)));
}

TEST_CASE("A closed stray shell becomes a volume of its own and is not deleted", "[MeshRepair]")
{
    OnePartObject     f{TriangleMesh(cube_with(its_make_cube(1.0, 1.0, 1.0)))};
    const BoundingBoxf3 before = f.object->bounding_box_exact();

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.parts_split == 2);
    CHECK(plan.parts_dropped == 0);
    CHECK(plan.parts_repaired == 0);

    const CgalRepairResult result = apply_cgal_repair(*f.object, false, plan);
    CHECK(result.error.empty());
    REQUIRE(f.object->volumes.size() == 2);
    const std::string name = f.object->volumes[0]->name.substr(0, f.object->volumes[0]->name.size() - 2);
    CHECK(f.object->volumes[0]->name == name + "_1");
    CHECK(f.object->volumes[1]->name == name + "_2");
    for (const ModelVolume* volume : f.object->volumes) {
        CHECK(volume->mesh().stats().open_edges == 0);
        CHECK(volume->config.has("extruder")); // each part keeps the filament the whole had
    }
    check_same_box(f.object->bounding_box_exact(), before);
}

TEST_CASE("A closed one-shell mesh has nothing to repair, and its recorded repairs stay", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};
    const auto    mesh_before = f.object->volumes.front()->mesh_ptr();

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.changes_nothing());
    CHECK_FALSE(plan.leaves_no_model_part());

    // The object list's Repair still applies it: the mesh is left as it was.
    CHECK(apply_cgal_repair(*f.object, false, plan).error.empty());
    CHECK(f.object->volumes.front()->mesh_ptr() == mesh_before);
    CHECK(f.object->get_object_stl_stats().repaired());
}

TEST_CASE("Repairing one volume leaves the object's other volumes as they were", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    f.object->add_volume(TriangleMesh(translated(cube_missing_facet(), Vec3f(20.f, 0.f, 0.f))));
    const auto     untouched_mesh = f.object->volumes[0]->mesh_ptr();
    const ObjectID untouched_id   = f.object->volumes[0]->id();

    const CgalRepairResult result = repaired(*f.object, 1);
    CHECK(result.error.empty());
    REQUIRE(f.object->volumes.size() == 2);
    CHECK(f.object->volumes[0]->mesh_ptr() == untouched_mesh);
    CHECK(f.object->volumes[0]->id() == untouched_id);
    CHECK(f.object->volumes[1]->mesh().stats().open_edges == 0);
    CHECK(result.first_volume == 1);
    CHECK(result.last_volume == 1);
}

// The object list's Repair used to repair in place on its worker thread; MCP never did. Both now plan
// first and apply after, and this is what holds them to upstream's result: the planned repair and the
// repair done in place, part by part as upstream did it, build the same object.
TEST_CASE("The planned repair builds the same object as the repair done in place", "[MeshRepair]")
{
    const std::string fixture = GENERATE(as<std::string>{}, "hole", "flat sheet", "closed stray shell", "holed stray shell", "clean");
    INFO("fixture: " << fixture);
    OnePartObject planned_object{TriangleMesh(fixture_mesh(fixture))};
    OnePartObject in_place_object{TriangleMesh(fixture_mesh(fixture))};

    CgalRepairPlan         plan = planned(*planned_object.object);
    const CgalRepairResult planned_result = apply_cgal_repair(*planned_object.object, false, plan);
    CgalRepairPlan         nothing_planned{capture_cgal_repair(*in_place_object.object, -1)};
    const CgalRepairResult in_place_result = apply_cgal_repair(*in_place_object.object, false, nothing_planned);

    CHECK(planned_result.error.empty());
    CHECK(in_place_result.error.empty());
    CHECK(planned_result.parts_repaired_here == 0); // the plan foresaw every part the loop repaired
    CHECK(in_place_result.parts_repaired_here == plan.parts_repaired);

    const ModelObject& a = *planned_object.object;
    const ModelObject& b = *in_place_object.object;
    REQUIRE(a.volumes.size() == b.volumes.size());
    for (size_t i = 0; i < a.volumes.size(); ++i) {
        INFO("volume " << i);
        CHECK(a.volumes[i]->name == b.volumes[i]->name);
        CHECK(a.volumes[i]->mesh().its.vertices == b.volumes[i]->mesh().its.vertices);
        CHECK(a.volumes[i]->mesh().its.indices == b.volumes[i]->mesh().its.indices);
        CHECK(a.volumes[i]->get_matrix().isApprox(b.volumes[i]->get_matrix()));
    }
    CHECK(a.instances.front()->get_matrix().isApprox(b.instances.front()->get_matrix()));
}

TEST_CASE("A volume the repair drops first does not make it skip the next one", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(flat_square())};
    f.object->add_volume(TriangleMesh(translated(cube_missing_facet(), Vec3f(20.f, 0.f, 0.f))));
    REQUIRE(open_edges(*f.object) == 4 + 3);

    const CgalRepairResult result = repaired(*f.object);
    CHECK(result.error.empty());
    REQUIRE(f.object->volumes.size() == 1);
    CHECK(open_edges(*f.object) == 0);
}

TEST_CASE("A repair that would leave no part to print is refused before anything changes", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(flat_square())};
    const auto    mesh_before = f.object->volumes.front()->mesh_ptr();

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.leaves_no_model_part());
    CHECK_FALSE(apply_cgal_repair(*f.object, false, plan).error.empty());
    REQUIRE(f.object->volumes.size() == 1);
    CHECK(f.object->volumes.front()->mesh_ptr() == mesh_before);

    // Repaired in place, part by part, the loop stops before it deletes the last part.
    CgalRepairPlan nothing_planned{capture_cgal_repair(*f.object, -1)};
    CHECK_FALSE(apply_cgal_repair(*f.object, false, nothing_planned).error.empty());
    CHECK(f.object->volumes.size() == 1);
}

TEST_CASE("Painting is cleared by a repair unless it is kept", "[MeshRepair]")
{
    const bool keep_painting = GENERATE(false, true);
    INFO("keep_painting: " << keep_painting);
    OnePartObject f{TriangleMesh(cube_missing_facet())};
    paint_first_volume(*f.object);
    REQUIRE(f.object->volumes.front()->is_mm_painted());

    CHECK(repaired(*f.object, -1, keep_painting).error.empty());
    CHECK(f.object->volumes.front()->is_mm_painted() == keep_painting);
}

TEST_CASE("A canceled plan stops between parts and applies nothing", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_with(cube_missing_facet()))};
    const auto    mesh_before = f.object->volumes.front()->mesh_ptr();

    int            asked = 0;
    CgalRepairPlan plan  = plan_cgal_repair(capture_cgal_repair(*f.object, -1), {}, [&asked] { return ++asked > 1; });
    CHECK(plan.canceled);
    CHECK(asked == 2);

    CHECK_FALSE(apply_cgal_repair(*f.object, false, plan).error.empty());
    REQUIRE(f.object->volumes.size() == 1);
    CHECK(f.object->volumes.front()->mesh_ptr() == mesh_before);
}

TEST_CASE("A plan no longer applies once the mesh it read has changed", "[MeshRepair]")
{
    OnePartObject  f{TriangleMesh(cube_missing_facet())};
    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.applies_to(*f.object));

    f.object->volumes.front()->set_mesh(TriangleMesh(its_make_cube(5.0, 5.0, 5.0)));
    CHECK_FALSE(plan.applies_to(*f.object));
    CHECK_FALSE(apply_cgal_repair(*f.object, false, plan).error.empty());
    CHECK(f.object->volumes.front()->mesh().stats().number_of_facets == 12);
}

TEST_CASE("The captured object is found by its place in the model's list, and not once it is gone", "[MeshRepair]")
{
    Model        model;
    ModelObject* first  = model.add_object();
    ModelObject* second = model.add_object();
    first->add_volume(TriangleMesh(its_make_cube(10.0, 10.0, 10.0)));
    second->add_volume(TriangleMesh(cube_missing_facet()));
    CgalRepairPlan plan = planned(*second);

    CHECK(cgal_repair_object_index(model, plan) == 1);
    model.delete_object(size_t(0));
    CHECK(cgal_repair_object_index(model, plan) == 0);
    model.delete_object(size_t(0));
    CHECK(cgal_repair_object_index(model, plan) == -1);
}

TEST_CASE("The plan reports the progress the Repair dialog shows", "[MeshRepair]")
{
    OnePartObject            f{TriangleMesh(cube_with(cube_missing_facet()))};
    std::vector<std::string> messages;
    std::vector<unsigned>    percents;
    plan_cgal_repair(capture_cgal_repair(*f.object, -1),
                     [&](const std::string& message, unsigned percent) {
                         messages.push_back(message);
                         percents.push_back(percent);
                     },
                     never);

    REQUIRE_FALSE(messages.empty());
    CHECK(messages.front() == "Repairing model object");
    CHECK(std::find(messages.begin(), messages.end(), "Split into 2 parts") != messages.end());
    CHECK(messages.back() == "Repair finished");
    CHECK(percents.back() == 100);
}
