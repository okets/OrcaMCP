#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPMeshRepair.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPUiJob.hpp"
#include "slic3r/Utils/FixModelByCgal.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "mesh_fixtures.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// The object list's Repair, split in two so no thread but the main one changes the model: a plan
// worked out on the meshes alone, and upstream's loop applying it. The object list's Repair, the cut
// gizmo and MCP's repair_mesh all run it, so what is checked here is what each of them does. The
// meshes are built with known defects: a hole, a flat stray sheet, a closed stray shell.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using namespace mesh_fixtures;
using Catch::Matchers::WithinAbs;
using json = nlohmann::json;

namespace {

const std::function<bool()> never = [] { return false; };

CgalRepairPlan planned(const ModelObject& object, int volume_idx = -1, bool keep_painting = false)
{
    return plan_cgal_repair(capture_cgal_repair(object, volume_idx, keep_painting), {}, never);
}

CgalRepairResult repaired(ModelObject& object, int volume_idx = -1, bool keep_painting = false)
{
    CgalRepairPlan plan = planned(object, volume_idx, keep_painting);
    return apply_cgal_repair(object, keep_painting, plan);
}

int open_edges(const ModelObject& object) { return object.get_object_stl_stats().open_edges; }

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

RepairedMeshErrors reversed_facets(int count)
{
    RepairedMeshErrors errors;
    errors.facets_reversed = count;
    return errors;
}

// The facets of the object's first volume that `paint` picks painted with filament 2, and every one
// a support enforcer.
void paint_first_volume(ModelObject& object, const std::function<bool(size_t)>& paint = [](size_t) { return true; })
{
    ModelVolume&     volume = *object.volumes.front();
    TriangleSelector colour(volume.mesh());
    TriangleSelector support(volume.mesh());
    for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i) {
        if (paint(i))
            colour.set_facet(int(i), EnforcerBlockerType::Extruder2);
        support.set_facet(int(i), EnforcerBlockerType::ENFORCER);
    }
    volume.mmu_segmentation_facets.set(colour);
    volume.supported_facets.set(support);
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
    const std::string fixture       = GENERATE(as<std::string>{}, "hole", "flat sheet", "closed stray shell", "holed stray shell", "clean");
    const bool        keep_painting = GENERATE(false, true);
    INFO("fixture: " << fixture << ", keep_painting: " << keep_painting);
    OnePartObject planned_object{TriangleMesh(fixture_mesh(fixture))};
    OnePartObject in_place_object{TriangleMesh(fixture_mesh(fixture))};
    // Colour on the facets of the first shell only (the first 12 or 11), so parts split apart carry
    // different painting; support everywhere.
    for (ModelObject* object : {planned_object.object, in_place_object.object})
        paint_first_volume(*object, [](size_t facet) { return facet < 11; });

    CgalRepairPlan         plan = planned(*planned_object.object, -1, keep_painting);
    const CgalRepairResult planned_result = apply_cgal_repair(*planned_object.object, keep_painting, plan);
    CgalRepairPlan         nothing_planned{capture_cgal_repair(*in_place_object.object, -1, keep_painting)};
    const CgalRepairResult in_place_result = apply_cgal_repair(*in_place_object.object, keep_painting, nothing_planned);

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
        CHECK(a.volumes[i]->get_convex_hull().its.vertices == b.volumes[i]->get_convex_hull().its.vertices);
        CHECK(a.volumes[i]->get_convex_hull().its.indices == b.volumes[i]->get_convex_hull().its.indices);
        // The painting the plan remapped off the main thread is the painting upstream's loop remaps.
        CHECK(a.volumes[i]->is_mm_painted() == b.volumes[i]->is_mm_painted());
        CHECK(a.volumes[i]->is_fdm_support_painted() == b.volumes[i]->is_fdm_support_painted());
        CHECK((a.volumes[i]->mmu_segmentation_facets.get_data() == b.volumes[i]->mmu_segmentation_facets.get_data()));
        CHECK((a.volumes[i]->supported_facets.get_data() == b.volumes[i]->supported_facets.get_data()));
    }
    CHECK(a.instances.front()->get_matrix().isApprox(b.instances.front()->get_matrix()));
}

// The object list captures every selected volume before its first dialog, and repairs them one after
// the other: the first one's split moves the volumes after it, so each is found again by its id.
TEST_CASE("Volumes captured before any repair are each repaired by id, after an earlier split moved them", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_with(its_make_cube(1.0, 1.0, 1.0)))};
    f.object->add_volume(TriangleMesh(translated(cube_missing_facet(), Vec3f(40.f, 0.f, 0.f))));
    const ObjectID holed_id = f.object->volumes[1]->id();

    CgalRepairPlan first  = planned(*f.object, 0);
    CgalRepairPlan second = planned(*f.object, 1);

    REQUIRE(apply_cgal_repair(*f.object, false, first).error.empty());
    REQUIRE(f.object->volumes.size() == 3); // the first volume became two
    REQUIRE(f.object->volumes[2]->id() == holed_id);

    const auto               split_meshes = std::vector<std::shared_ptr<const TriangleMesh>>{f.object->volumes[0]->mesh_ptr(),
                                                                                              f.object->volumes[1]->mesh_ptr()};
    const CgalRepairResult   result       = apply_cgal_repair(*f.object, false, second);
    CHECK(result.error.empty());
    CHECK(result.first_volume == 2);
    CHECK(f.object->volumes[2]->mesh().stats().open_edges == 0);
    CHECK(f.object->volumes[0]->mesh_ptr() == split_meshes[0]);
    CHECK(f.object->volumes[1]->mesh_ptr() == split_meshes[1]);
}

TEST_CASE("A one-volume repair captured for a volume the object does not have repairs nothing", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};
    const auto    mesh_before = f.object->volumes.front()->mesh_ptr();

    CgalRepairPlan plan = planned(*f.object, 3);
    CHECK_FALSE(plan.applies_to(*f.object));
    CHECK_FALSE(apply_cgal_repair(*f.object, false, plan).error.empty());
    CHECK(f.object->volumes.front()->mesh_ptr() == mesh_before);
    CHECK(open_edges(*f.object) == 3);
}

// The object's first volume painted with `filament` on every facet.
void paint_first_volume_with(ModelObject& object, EnforcerBlockerType filament)
{
    ModelVolume&     volume = *object.volumes.front();
    TriangleSelector colour(volume.mesh());
    for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i)
        colour.set_facet(int(i), filament);
    volume.mmu_segmentation_facets.set(colour);
}

void check_same_painting(const ModelObject& a, const ModelObject& b)
{
    REQUIRE(a.volumes.size() == b.volumes.size());
    for (size_t i = 0; i < a.volumes.size(); ++i) {
        INFO("volume " << i);
        CHECK((a.volumes[i]->mmu_segmentation_facets.get_data() == b.volumes[i]->mmu_segmentation_facets.get_data()));
        CHECK((a.volumes[i]->supported_facets.get_data() == b.volumes[i]->supported_facets.get_data()));
    }
}

// The painting the plan remapped is the painting captured. Paint written before the repair is applied
// -- a paint tool, a stroke in the gizmo -- is remapped in place instead, as upstream's loop would, so
// the repair keeps the painting as it is when applied.
TEST_CASE("Painting changed between capture and apply is remapped as it is when applied", "[MeshRepair]")
{
    const std::string fixture = GENERATE(as<std::string>{}, "hole", "holed stray shell");
    INFO("fixture: " << fixture);
    OnePartObject planned_object{TriangleMesh(fixture_mesh(fixture))};
    OnePartObject reference{TriangleMesh(fixture_mesh(fixture))};
    paint_first_volume_with(*planned_object.object, EnforcerBlockerType::Extruder2);

    CgalRepairPlan plan = planned(*planned_object.object, -1, /*keep_painting=*/true);
    // Painted again, with another filament, after the capture.
    paint_first_volume_with(*planned_object.object, EnforcerBlockerType::Extruder3);
    CHECK_FALSE(plan.painting_unchanged(*planned_object.object->volumes.front()));
    REQUIRE(apply_cgal_repair(*planned_object.object, true, plan).error.empty());

    paint_first_volume_with(*reference.object, EnforcerBlockerType::Extruder3);
    CgalRepairPlan nothing_planned{capture_cgal_repair(*reference.object, -1, true)};
    REQUIRE(apply_cgal_repair(*reference.object, true, nothing_planned).error.empty());

    check_same_painting(*planned_object.object, *reference.object);
    for (const ModelVolume* volume : planned_object.object->volumes)
        CHECK(volume->is_mm_painted());
}

// Two volumes can hold the same mesh -- here the first volume is bit for bit the first part the second
// one splits into -- with different painting; each part takes its own volume's.
TEST_CASE("Parts with the same mesh in different volumes keep their own volume's painting", "[MeshRepair]")
{
    auto build = [](OnePartObject& f) {
        // The second volume: a cube and a holed stray shell. The first: that cube, as the split makes it.
        const TriangleMesh whole(cube_with(cube_missing_facet()));
        f.object->add_volume(TriangleMesh(whole));
        std::vector<TriangleMesh> parts = whole.split();
        REQUIRE(parts.size() == 2);
        f.object->volumes.front()->set_mesh(TriangleMesh(parts.front().its));
        f.object->volumes.front()->center_geometry_after_creation();
        // Colour: filament 2 on the first volume, 3 on the second.
        for (size_t v = 0; v < 2; ++v) {
            ModelVolume&     volume = *f.object->volumes[v];
            TriangleSelector colour(volume.mesh());
            for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i)
                colour.set_facet(int(i), v == 0 ? EnforcerBlockerType::Extruder2 : EnforcerBlockerType::Extruder3);
            volume.mmu_segmentation_facets.set(colour);
        }
    };
    OnePartObject planned_object{TriangleMesh(its_make_cube(1.0, 1.0, 1.0))};
    OnePartObject reference{TriangleMesh(its_make_cube(1.0, 1.0, 1.0))};
    build(planned_object);
    build(reference);

    CgalRepairPlan         plan = planned(*planned_object.object, -1, true);
    const CgalRepairResult result = apply_cgal_repair(*planned_object.object, true, plan);
    CHECK(result.error.empty());
    CHECK(result.parts_repaired_here == 0);
    CgalRepairPlan nothing_planned{capture_cgal_repair(*reference.object, -1, true)};
    REQUIRE(apply_cgal_repair(*reference.object, true, nothing_planned).error.empty());

    REQUIRE(planned_object.object->volumes.size() == 3);
    check_same_painting(*planned_object.object, *reference.object);
    CHECK_FALSE((planned_object.object->volumes[0]->mmu_segmentation_facets.get_data() ==
                 planned_object.object->volumes[1]->mmu_segmentation_facets.get_data()));
}

TEST_CASE("A volume the repair drops first does not make it skip the next one", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(flat_square())};
    f.object->add_volume(TriangleMesh(translated(cube_missing_facet(), Vec3f(20.f, 0.f, 0.f))));
    REQUIRE(open_edges(*f.object) == 4 + 3);

    const CgalRepairResult result = repaired(*f.object);
    CHECK(result.error.empty());
    CHECK(result.parts_repaired_here == 0); // found in the plan although the drop gave it a new id
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

// ---- repair_mesh's decisions and answer ---------------------------------------------------------

TEST_CASE("repair_mesh is refused while the pipeline is busy, a job runs or the app's own Repair runs", "[MeshRepair][orcamcp]")
{
    PipelineState idle;
    CHECK_FALSE(repair_refusal(idle, false, false));

    PipelineState slicing;
    slicing.process_working = true;
    slicing.is_slicing      = true;
    const auto busy         = repair_refusal(slicing, false, false);
    REQUIRE(busy);
    CHECK(busy->find("a slice is in progress") != std::string::npos);
    CHECK(mentions(*busy, "wait_for_slice"));
    CHECK(mentions(*busy, "repair_mesh again"));

    const auto job = repair_refusal(idle, true, false);
    REQUIRE(job);
    CHECK(*job == ui_job_busy_message("repair_mesh"));

    const auto dialog = repair_refusal(idle, false, true);
    REQUIRE(dialog);
    CHECK(mentions(*dialog, "own mesh repair"));
    CHECK(mentions(*dialog, "repair_mesh again"));
}

TEST_CASE("repair_mesh refuses a volume_id the object does not have, a negative one too", "[MeshRepair][orcamcp]")
{
    CHECK_FALSE(repair_volume_error(0, 2, std::nullopt)); // none given: the whole object
    CHECK_FALSE(repair_volume_error(0, 2, 1));
    for (const int bad : {2, -1}) {
        const auto error = repair_volume_error(3, 2, bad);
        REQUIRE(error);
        CHECK(mentions(*error, "volume_id " + std::to_string(bad)));
        CHECK(mentions(*error, "object 3"));
        CHECK(mentions(*error, "volume_id 0 to 1"));
    }
}

TEST_CASE("repair_mesh applies only a finished plan that changes something, in time", "[MeshRepair][orcamcp]")
{
    OnePartObject hole{TriangleMesh(cube_missing_facet())};
    OnePartObject clean{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    OnePartObject sheet{TriangleMesh(flat_square())};

    CHECK(repair_step(planned(*hole.object), false) == RepairStep::apply);
    // The one step that changes the object is the only one with an undo snapshot.
    CHECK(repair_step(planned(*clean.object), false) == RepairStep::nothing_to_repair);
    CHECK(repair_step(planned(*hole.object), true) == RepairStep::timed_out);
    CHECK(repair_step(planned(*sheet.object), false) == RepairStep::leaves_nothing);

    CgalRepairPlan canceled = planned(*hole.object);
    canceled.canceled       = true;
    CHECK(repair_step(canceled, false) == RepairStep::timed_out);
    CgalRepairPlan failed = planned(*hole.object);
    failed.error          = "Repair failed: mesh still open after hole filling.";
    failed.error_volume   = 0;
    CHECK(repair_step(failed, false) == RepairStep::failed);
}

TEST_CASE("repair_mesh says why nothing was changed, and what to call instead", "[MeshRepair][orcamcp]")
{
    OnePartObject clean{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};
    OnePartObject hole{TriangleMesh(cube_missing_facet())};
    OnePartObject sheet{TriangleMesh(flat_square())};

    const std::string nothing = repair_step_message(RepairStep::nothing_to_repair, planned(*clean.object), 0, -1, 105.0,
                                                    repair_facts_json(*clean.object));
    CHECK(nothing.find("Nothing to repair") != std::string::npos);
    CHECK(nothing.find("1 repair recorded") != std::string::npos);

    const std::string timed_out = repair_step_message(RepairStep::timed_out, planned(*hole.object), 0, -1, 105.0,
                                                      repair_facts_json(*hole.object));
    CHECK(timed_out.find("105 s") != std::string::npos);
    CHECK(timed_out.find("ORCAMCP_TIMEOUT") != std::string::npos);
    CHECK(timed_out.find("nothing was changed") != std::string::npos);

    CgalRepairPlan failed = planned(*hole.object);
    failed.error          = "Repair failed: mesh still open after hole filling.";
    failed.error_volume   = 0;
    const std::string failure = repair_step_message(RepairStep::failed, failed, 0, -1, 105.0, json::object());
    CHECK(mentions(failure, "failed on volume 0"));
    CHECK(mentions(failure, "mesh still open after hole filling"));
    CHECK(mentions(failure, "Nothing was changed"));

    const std::string leaves = repair_step_message(RepairStep::leaves_nothing, planned(*sheet.object), 0, -1, 105.0,
                                                   repair_facts_json(*sheet.object));
    CHECK(leaves.find("flat or empty") != std::string::npos);
    CHECK(leaves.find("delete_object") != std::string::npos);

    // Never a GUI button: what an agent is told to do, it can do itself.
    for (const std::string& message : {nothing, timed_out, leaves})
        for (const char* gui : {"click", "Click", "the user", "GUI", "button"}) {
            INFO(message << " names " << gui);
            CHECK(message.find(gui) == std::string::npos);
        }
}

TEST_CASE("repair_mesh's answer carries the numbers before and after, the parts, painting and volumes", "[MeshRepair][orcamcp]")
{
    OnePartObject  f{TriangleMesh(cube_with(flat_square()))};
    const json     before = repair_facts_json(*f.object);
    CgalRepairPlan plan   = planned(*f.object);
    REQUIRE(apply_cgal_repair(*f.object, false, plan).error.empty());
    const json after = repair_facts_json(*f.object);

    for (const char* key : {"facets", "shells", "open_edges", "manifold", "repaired", "errors_repaired", "repaired_errors",
                            "mesh_warning", "volumes", "painted", "position"})
        CHECK(before.contains(key));
    CHECK(before["open_edges"] == 4);
    CHECK(before["mesh_warning"] == true);
    CHECK(after["open_edges"] == 0);
    CHECK(after["shells"] == 1);
    CHECK(after["mesh_warning"] == false);

    const json answer = repair_answer_json(0, "Test object", -1, false, false, before, after, plan);
    CHECK(answer["status"] == "success");
    CHECK(answer["changed"] == true);
    CHECK(answer["object_id"] == 0);
    CHECK(answer["object_name"] == "Test object");
    CHECK_FALSE(answer.contains("volume_id"));
    CHECK(answer["keep_painting"] == false);
    CHECK(answer["keep_painting_from"] == "app_setting");
    CHECK(answer["volumes_before"] == 1);
    CHECK(answer["volumes_after"] == 1);
    CHECK(answer["parts"] == json{{"split", 2}, {"dropped", 1}, {"repaired", 0}});
    CHECK(answer["before"] == before);
    CHECK(answer["after"] == after);

    const json one_volume = repair_answer_json(0, "Test object", 0, true, true, before, after, plan);
    CHECK(one_volume["volume_id"] == 0);
    CHECK(one_volume["keep_painting"] == true);
    CHECK(one_volume["keep_painting_from"] == "argument");
}

// Hidden ([.]): where the time goes on large meshes -- how long the main thread's two steps (capture,
// apply) and the plan take -- printed, not asserted, since it depends on the machine and the build.
// Run it by its tag; the painted cases take minutes on the -O0 build (the remap, in the plan).
TEST_CASE("The repair's steps are timed on large meshes", "[.][MeshRepairTiming]")
{
    using Clock  = std::chrono::steady_clock;
    auto seconds = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };
    auto holed   = [](indexed_triangle_set its) {
        its.indices.erase(its.indices.begin() + std::ptrdiff_t(its.indices.size() / 2));
        return its;
    };
    auto spheres = [&holed](int count) {
        indexed_triangle_set shells;
        for (int i = 0; i < count; ++i)
            its_merge(shells, translated(holed(its_make_sphere(1., 2. * PI / 32.)), Vec3f(3.f * float(i % 20), 3.f * float(i / 20), 0.f)));
        return shells;
    };
    const std::vector<std::pair<std::string, indexed_triangle_set>> cases = {
        {"one sphere with a hole", holed(its_make_sphere(20., 2. * PI / 707.))},
        {"one smaller sphere with a hole", holed(its_make_sphere(20., 2. * PI / 100.))},
        {"200 small spheres, each with a hole", spheres(200)},
        {"10 small spheres, each with a hole", spheres(10)}};

    for (const bool keep_painting : {false, true})
        for (const auto& [name, mesh] : cases) {
            if (keep_painting && mesh.indices.size() > 20000)
                continue; // the painting remap takes minutes per 10,000 painted facets on the dev build
            OnePartObject f{TriangleMesh(mesh)};
            if (keep_painting)
                paint_first_volume(*f.object);
            const auto       t0     = Clock::now();
            CgalRepairInput  input  = capture_cgal_repair(*f.object, -1, keep_painting);
            const auto       t1     = Clock::now();
            CgalRepairPlan   plan   = plan_cgal_repair(std::move(input), {}, never);
            const auto       t2     = Clock::now();
            CgalRepairResult result = apply_cgal_repair(*f.object, keep_painting, plan);
            const auto       t3     = Clock::now();
            WARN(name << ", " << mesh.indices.size() << " facets, keep_painting " << keep_painting << ": capture "
                      << seconds(t0, t1) << " s, plan " << seconds(t1, t2) << " s, apply " << seconds(t2, t3)
                      << " s; parts split " << plan.parts_split << ", repaired " << plan.parts_repaired
                      << ", repaired in apply " << result.parts_repaired_here << (result.error.empty() ? "" : ", error " + result.error));
        }
}
