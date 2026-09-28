#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPMeshHealth.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <test_utils.hpp>
#include "mesh_fixtures.hpp"

#include <string>

// The object list's warning icon beside an object, and the tooltip under it, are what a user sees
// when a mesh has errors. Everything here reads them from the model alone -- the list's own
// mesh_errors_info -- so a headless caller reports exactly what the list shows. The meshes are
// built with known defects, and the tooltip text is spelled out in full: it is the list's wording,
// and a change to it should fail here before an agent quotes a sentence the GUI no longer shows.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;
using namespace mesh_fixtures;

namespace {

const char* const k_hole_tooltip = "Remaining errors:\n\t3 non-manifold edges\n\nClick the icon to repair model object";

RepairedMeshErrors reversed_facets(int count)
{
    RepairedMeshErrors errors;
    errors.facets_reversed = count;
    return errors;
}

std::string utf8(const wxString& text) { return text.ToUTF8().data(); }

} // namespace

TEST_CASE("A hole shows the warning icon, with the list's remaining-errors tooltip", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};
    const TriangleMeshStats stats = f.object->get_object_stl_stats();

    wxString   sidebar;
    int        open_edges = -1;
    const auto info       = GUI::mesh_errors_info(stats, &sidebar, &open_edges);
    const auto tooltip    = GUI::mesh_errors_info(stats);

    CHECK(tooltip.warning_icon_name == "obj_warning");
    CHECK(GUI::get_warning_icon_name(stats) == "obj_warning");
    CHECK(utf8(tooltip.tooltip) == k_hole_tooltip);
    // With sidebar_info asked for, the list's sidebar line comes back and the tooltip loses its
    // "click the icon" line, as ObjectList::get_mesh_errors_info always did.
    CHECK(utf8(sidebar) == "Error: 3 non-manifold edges.");
    CHECK(utf8(info.tooltip) == "Remaining errors:\n\t3 non-manifold edges\n");
    CHECK(open_edges == 3);

    // The same text for the object's only volume, from the volume's own stats.
    CHECK(utf8(GUI::mesh_errors_info(f.object->volumes[0]->mesh().stats()).tooltip) == k_hole_tooltip);
}

TEST_CASE("Recorded repairs show the warning icon and say how many were repaired", "[MeshHealth][orcamcp]")
{
    // A 3MF's mesh_stat element reaches the mesh through this constructor (bbs_3mf.cpp); it is the
    // only way a repair count gets into a loaded mesh (from_stl records none).
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};
    const TriangleMeshStats stats = f.object->get_object_stl_stats();

    wxString   sidebar;
    const auto tooltip = GUI::mesh_errors_info(stats);
    GUI::mesh_errors_info(stats, &sidebar);

    CHECK(tooltip.warning_icon_name == "obj_warning");
    CHECK(utf8(tooltip.tooltip) == "1 error repaired\n\nClick the icon to repair model object");
    CHECK(utf8(sidebar) == "1 error repaired");
}

TEST_CASE("Repairs and a hole together are both named in the tooltip", "[MeshHealth][orcamcp]")
{
    RepairedMeshErrors errors;
    errors.edges_fixed = 2;
    OnePartObject f{TriangleMesh(cube_missing_facet(), errors)};
    const TriangleMeshStats stats = f.object->get_object_stl_stats();

    wxString   sidebar;
    const auto tooltip = GUI::mesh_errors_info(stats);
    GUI::mesh_errors_info(stats, &sidebar);

    CHECK(utf8(tooltip.tooltip) ==
          "2 errors repaired\nRemaining errors:\n\t3 non-manifold edges\n\nClick the icon to repair model object");
    CHECK(utf8(sidebar) == "Error: 3 non-manifold edges.\n2 errors repaired");
}

TEST_CASE("A clean mesh has no warning icon and no tooltip", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    const TriangleMeshStats stats = f.object->get_object_stl_stats();

    const auto info = GUI::mesh_errors_info(stats);
    CHECK(info.warning_icon_name.empty());
    CHECK(info.tooltip.empty());
    CHECK(GUI::get_warning_icon_name(stats).empty());
}

TEST_CASE("The object row counts a modifier's open edges, as the list does", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    f.object->add_volume(TriangleMesh(cube_missing_facet()), ModelVolumeType::PARAMETER_MODIFIER);

    CHECK(GUI::get_warning_icon_name(f.object->get_object_stl_stats()) == "obj_warning");
    CHECK(GUI::get_warning_icon_name(f.object->volumes[0]->mesh().stats()).empty());
    CHECK(GUI::get_warning_icon_name(f.object->volumes[1]->mesh().stats()) == "obj_warning");
}

TEST_CASE("The repair count the tooltip states is the sum of the recorded repairs, as the model counts them", "[MeshHealth][orcamcp]")
{
    RepairedMeshErrors errors;
    errors.edges_fixed       = 1;
    errors.degenerate_facets = 2;
    errors.facets_removed    = 3;
    errors.facets_reversed   = 4;
    errors.backwards_edges   = 5;
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), errors)};

    CHECK(GUI::repaired_errors_count(errors) == 15);
    CHECK(GUI::repaired_errors_count(f.object->get_object_stl_stats().repaired_errors) == f.object->get_repaired_errors_count());
    CHECK(GUI::repaired_errors_count(RepairedMeshErrors()) == 0);
}

TEST_CASE("Mesh health reports a hole's numbers with the list's icon and tooltip", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};

    const MeshHealth h = object_mesh_health(*f.object);
    CHECK(h.facets() == 11);
    CHECK(h.shells() == 1);
    CHECK(h.open_edges() == 3);
    CHECK_FALSE(h.manifold());
    CHECK_FALSE(h.repaired());
    CHECK(h.errors_repaired() == 0);
    CHECK(h.warning);
    // The list's own text, word for word, from the same function the list calls.
    CHECK(mesh_warning_tooltip(h) == utf8(GUI::mesh_errors_info(f.object->get_object_stl_stats()).tooltip));
    CHECK(mesh_warning_tooltip(h) == k_hole_tooltip);
    CHECK(mesh_warning_reason(h) == "Error: 3 non-manifold edges.");
}

TEST_CASE("Mesh health of a clean mesh has no warning, tooltip or reason", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};

    const MeshHealth h = object_mesh_health(*f.object);
    CHECK(h.facets() == 12);
    CHECK(h.shells() == 1);
    CHECK(h.manifold());
    CHECK_FALSE(h.repaired());
    CHECK_FALSE(h.warning);
    CHECK(mesh_warning_tooltip(h).empty());
    CHECK(mesh_warning_reason(h).empty());
}

TEST_CASE("Two disjoint cubes in one part are two shells, and no warning", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(separate_cubes(2))};

    const MeshHealth h = object_mesh_health(*f.object);
    CHECK(h.shells() == 2);
    CHECK(h.facets() == 24);
    CHECK(h.manifold());
    CHECK_FALSE(h.warning);
}

TEST_CASE("Recorded repairs are reported field by field, with the list's count", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};

    const MeshHealth h = object_mesh_health(*f.object);
    CHECK(h.stats.repaired_errors.facets_reversed == 1);
    CHECK(h.errors_repaired() == 1);
    CHECK(h.errors_repaired() == f.object->get_repaired_errors_count());
    CHECK(h.repaired());
    CHECK(h.manifold());
    CHECK(h.warning);
    CHECK(mesh_warning_tooltip(h) == "1 error repaired\n\nClick the icon to repair model object");
    CHECK(mesh_warning_reason(h) == "1 error repaired");
}

TEST_CASE("A reason naming a hole and repairs is one line", "[MeshHealth][orcamcp]")
{
    RepairedMeshErrors errors;
    errors.edges_fixed = 2;
    OnePartObject f{TriangleMesh(cube_missing_facet(), errors)};

    CHECK(mesh_warning_reason(object_mesh_health(*f.object)) == "Error: 3 non-manifold edges. 2 errors repaired");
}

TEST_CASE("An STL loaded with a reversed facet is repaired silently and records nothing", "[MeshHealth][orcamcp]")
{
    // Orca's STL import flips the facet back (admesh's repair) but from_stl does not keep admesh's
    // counts (the block that did is #if 0, TriangleMesh.cpp), so there is nothing for the list, or
    // this tool, to report. Only a 3MF's recorded mesh_stat ever carries repair counts.
    indexed_triangle_set its = its_make_cube(10.0, 10.0, 10.0);
    std::swap(its.indices[5][1], its.indices[5][2]);
    ScopedTemporaryFile stl(".stl");
    REQUIRE(its_write_stl_ascii(stl.string().c_str(), "reversed", its));

    Model model;
    REQUIRE(load_stl(stl.string().c_str(), &model));
    REQUIRE(model.objects.size() == 1);
    model.objects.front()->add_instance();

    const MeshHealth h = object_mesh_health(*model.objects.front());
    CHECK(h.facets() == 12);
    CHECK(h.shells() == 1);
    CHECK(h.open_edges() == 0);
    CHECK_FALSE(h.repaired());
    CHECK_FALSE(h.warning);
}

TEST_CASE("An object's health counts every volume's errors and only the parts' facets and shells", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    f.object->add_volume(TriangleMesh(separate_cubes(2)), ModelVolumeType::PARAMETER_MODIFIER);
    f.object->add_volume(TriangleMesh(cube_missing_facet()), ModelVolumeType::PARAMETER_MODIFIER);

    const MeshHealth object = object_mesh_health(*f.object);
    CHECK(object.facets() == 12);
    CHECK(object.shells() == 1);
    CHECK(object.open_edges() == 3);
    CHECK(object.warning);

    CHECK_FALSE(volume_mesh_health(*f.object, 0).warning);
    CHECK(volume_mesh_health(*f.object, 1).shells() == 2);
    CHECK(volume_mesh_health(*f.object, 1).facets() == 24);
    CHECK(volume_mesh_health(*f.object, 2).open_edges() == 3);
    CHECK(mesh_warning_tooltip(volume_mesh_health(*f.object, 2)) == k_hole_tooltip);
}

TEST_CASE("A model's mesh health is one reading per object, by object index", "[MeshHealth][orcamcp]")
{
    Model model;
    for (bool hole : {false, true}) {
        ModelObject* object = model.add_object();
        object->add_volume(hole ? TriangleMesh(cube_missing_facet()) : TriangleMesh(its_make_cube(10.0, 10.0, 10.0)));
        object->add_instance();
    }

    const std::vector<MeshHealth> health = model_mesh_health(model);
    REQUIRE(health.size() == 2);
    CHECK_FALSE(health[0].warning);
    CHECK(health[1].warning);
    CHECK(health[1].open_edges() == 3);
    CHECK(model_mesh_health(Model()).empty());
}

TEST_CASE("The mesh numbers are one JSON shape, and features adds the object's volume", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};

    const nlohmann::json numbers = mesh_numbers_json(object_mesh_health(*f.object));
    CHECK(numbers == nlohmann::json{{"facets", 12},
                                    {"shells", 1},
                                    {"open_edges", 0},
                                    {"manifold", true},
                                    {"repaired", true},
                                    {"errors_repaired", 1},
                                    {"repaired_errors",
                                     {{"edges_fixed", 0},
                                      {"degenerate_facets", 0},
                                      {"facets_removed", 0},
                                      {"facets_reversed", 1},
                                      {"backwards_edges", 0}}}});

    nlohmann::json features = mesh_features_json(object_mesh_health(*f.object));
    CHECK_THAT(features["volume_mm3"].get<double>(), WithinAbs(1000.0, 1e-3));
    features.erase("volume_mm3");
    CHECK(features == numbers);

    // The object's volume is its first instance's, scaled as the list's sidebar measures it.
    f.object->instances.front()->set_scaling_factor(Vec3d(2.0, 2.0, 2.0));
    CHECK_THAT(mesh_features_json(object_mesh_health(*f.object))["volume_mm3"].get<double>(), WithinAbs(8000.0, 1e-3));
}

TEST_CASE("get_mesh_health lists a multi-shell part's shells, most facets first, in plate millimetres", "[MeshHealth][orcamcp]")
{
    indexed_triangle_set its = its_make_cube(2.0, 2.0, 2.0);
    its_merge(its, translated(its_make_sphere(1.0, PI / 8.0), Vec3f(10.f, 0.f, 0.f)));
    OnePartObject f{TriangleMesh(std::move(its))};
    f.object->add_volume(TriangleMesh(its_make_cube(1.0, 1.0, 1.0)), ModelVolumeType::PARAMETER_MODIFIER);
    f.object->instances.front()->set_offset(Vec3d(100.0, 50.0, 0.0));

    MeshHealthReport report = mesh_health_report(*f.object, 7);
    REQUIRE(report.shell_jobs.size() == 1);  // the part; the one-shell modifier needs no list
    add_shell_lists(report);

    const nlohmann::json& r = report.response;
    CHECK(r["status"] == "success");
    CHECK(r["object_id"] == 7);
    CHECK(r["object_name"] == "Test object");
    CHECK(r["summary"]["shells"] == 2);
    REQUIRE(r["volumes"].size() == 2);

    const nlohmann::json& part = r["volumes"][0];
    CHECK(part["volume_id"] == 0);
    CHECK(part["type"] == "part");
    CHECK(part["shells"] == 2);
    const nlohmann::json& list = part["shell_list"];
    CHECK(list["total"] == 2);
    CHECK(list["listed"] == 2);
    CHECK(list["coordinate_frame"] == "plate");
    CHECK(list["instance_id"] == 0);
    REQUIRE(list["shells"].size() == 2);
    CHECK(list["shells"][0]["component"] == 1);  // the sphere: more facets than the cube
    CHECK(list["shells"][1]["facet_count"] == 12);
    // The cube's shell sits where the instance put it: 2 mm wide, about x = 100.
    const nlohmann::json& cube_box = list["shells"][1]["bounding_box"];
    CHECK_THAT(cube_box["max"]["x"].get<double>() - cube_box["min"]["x"].get<double>(), WithinAbs(2.0, 1e-6));
    CHECK(cube_box["min"]["x"].get<double>() > 90.0);
    CHECK(cube_box["max"]["x"].get<double>() < 110.0);

    CHECK(r["volumes"][1]["type"] == "modifier");
    CHECK_FALSE(r["volumes"][1].contains("shell_list"));
}

TEST_CASE("get_mesh_health lists at most ten shells and says how many there are", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(separate_cubes(12))};

    MeshHealthReport report = mesh_health_report(*f.object, 0);
    add_shell_lists(report);

    const nlohmann::json& list = report.response["volumes"][0]["shell_list"];
    CHECK(list["total"] == 12);
    CHECK(list["listed"] == 10);
    CHECK(list["shells"].size() == 10);
}

TEST_CASE("get_mesh_health reports a hole's icon, tooltip and reason for the object and its part", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};

    MeshHealthReport report = mesh_health_report(*f.object, 0);
    CHECK(report.shell_jobs.empty());
    add_shell_lists(report);

    const nlohmann::json& r = report.response;
    CHECK(r["mesh_warning"] == true);
    CHECK(r["tooltip"] == k_hole_tooltip);
    CHECK(r["mesh_warning_reason"] == "Error: 3 non-manifold edges.");
    CHECK(r["summary"]["open_edges"] == 3);
    CHECK(r["volumes"][0]["mesh_warning"] == true);
    CHECK(r["volumes"][0]["tooltip"] == k_hole_tooltip);
    CHECK(r["volumes"][0]["mesh_warning_reason"] == "Error: 3 non-manifold edges.");
    CHECK(r["volumes"][0]["open_edges"] == 3);
}

// One shape everywhere: mesh_warning always, and the text that explains it only when it is true.
TEST_CASE("get_mesh_health leaves the tooltip and reason out of a row without the icon", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    f.object->add_volume(TriangleMesh(cube_missing_facet()), ModelVolumeType::PARAMETER_MODIFIER);

    MeshHealthReport report = mesh_health_report(*f.object, 0);
    const nlohmann::json& r = report.response;
    CHECK(r["mesh_warning"] == true);  // the object row counts the modifier's hole
    const nlohmann::json& part = r["volumes"][0];
    CHECK(part["mesh_warning"] == false);
    CHECK_FALSE(part.contains("tooltip"));
    CHECK_FALSE(part.contains("mesh_warning_reason"));
    CHECK(r["volumes"][1]["mesh_warning"] == true);

    OnePartObject clean{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    const nlohmann::json c = mesh_health_report(*clean.object, 0).response;
    CHECK(c["mesh_warning"] == false);
    CHECK_FALSE(c.contains("tooltip"));
    CHECK_FALSE(c.contains("mesh_warning_reason"));
}

// The object list's "Click the icon to repair model object" is advice for a mouse. What an agent is
// told instead: MCP cannot repair, and what the slicer does with such a mesh.
TEST_CASE("An agent is told what it can do about a flagged mesh, never to click the icon", "[MeshHealth][orcamcp]")
{
    OnePartObject hole{TriangleMesh(cube_missing_facet())};
    OnePartObject repaired{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};
    OnePartObject clean{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};

    const std::string hole_advice = mesh_warning_advice(object_mesh_health(*hole.object));
    CHECK(hole_advice.find("cannot repair") != std::string::npos);
    CHECK(hole_advice.find("2 mm") != std::string::npos);  // the slicer's per-layer gap closing
    CHECK(hole_advice.find("Click") == std::string::npos);

    const std::string repaired_advice = mesh_warning_advice(object_mesh_health(*repaired.object));
    CHECK(repaired_advice.find("prints as it is") != std::string::npos);
    CHECK(repaired_advice.find("Click") == std::string::npos);

    CHECK(mesh_warning_advice(object_mesh_health(*clean.object)).empty());
}

TEST_CASE("A MeshErrors warning names the object, the list's reason and what to do", "[MeshHealth][orcamcp]")
{
    Model model;
    for (int i = 0; i < 3; ++i) {
        ModelObject* object = model.add_object();
        object->name        = "Object " + std::to_string(i);
        object->add_volume(i == 1 ? TriangleMesh(cube_missing_facet()) : TriangleMesh(its_make_cube(10.0, 10.0, 10.0)));
        object->add_instance();
    }
    const std::vector<MeshHealth> health = model_mesh_health(model);

    const nlohmann::json entries = mesh_error_warnings(model, health);
    REQUIRE(entries.size() == 1);
    const nlohmann::json& entry = entries[0];
    CHECK(entry["level"] == "warning");
    CHECK(entry["type"] == "MeshErrors");
    CHECK(entry["object_id"] == 1);
    CHECK(entry["object_name"] == "Object 1");
    const std::string message = entry["message"];
    CHECK(message.rfind("Error: 3 non-manifold edges. ", 0) == 0);  // the list's reason first
    CHECK(message.find(mesh_warning_advice(health[1])) != std::string::npos);
    CHECK(message.find("get_mesh_health {object_id: 1}") != std::string::npos);
    CHECK(message.find("Click") == std::string::npos);

    // load_model's form: only the objects it names, read on the spot -- the flagged ones among the
    // objects it describes.
    nlohmann::json described = nlohmann::json::array();
    for (int i = 0; i < 3; ++i)
        described.push_back(model_object_summary_json(*model.objects[std::size_t(i)], i));
    CHECK(flagged_object_indices(described) == std::vector<int>{1});
    CHECK(mesh_error_warnings(model, flagged_object_indices(described)) == entries);
    CHECK(mesh_error_warnings(model, std::vector<int>{0, 1}) == entries);
    CHECK(mesh_error_warnings(model, std::vector<int>{0, 2}).empty());
    CHECK(mesh_error_warnings(Model(), std::vector<MeshHealth>{}).empty());
}

TEST_CASE("Warnings added to an active_warnings section keep its count true", "[MeshHealth][orcamcp]")
{
    nlohmann::json section = {{"count", 1}, {"warnings", {{{"type", "ValidateWarning"}}}}};
    add_warnings(section, nlohmann::json::array({{{"type", "MeshErrors"}}, {{"type", "MeshErrors"}}}));
    CHECK(section["count"] == 3);
    CHECK(section["warnings"].size() == 3);
    CHECK(section["warnings"][2]["type"] == "MeshErrors");

    add_warnings(section, nlohmann::json::array());
    CHECK(section["count"] == 3);
}

TEST_CASE("get_mesh_health tells an agent what it can do about a flagged object", "[MeshHealth][orcamcp]")
{
    OnePartObject hole{TriangleMesh(cube_missing_facet())};
    const nlohmann::json r = mesh_health_report(*hole.object, 0).response;
    CHECK(r["advice"] == mesh_warning_advice(object_mesh_health(*hole.object)));
    CHECK(r["tooltip"] == k_hole_tooltip);  // the GUI's own text stays exact

    OnePartObject clean{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    CHECK_FALSE(mesh_health_report(*clean.object, 0).response.contains("advice"));
}

TEST_CASE("Every object description flags the warning icon, with the reason when it shows", "[MeshHealth][orcamcp]")
{
    OnePartObject clean{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    OnePartObject hole{TriangleMesh(cube_missing_facet())};

    nlohmann::json out;
    add_mesh_warning(out, object_mesh_health(*clean.object));
    CHECK(out == nlohmann::json{{"mesh_warning", false}});

    out = nlohmann::json::object();
    add_mesh_warning(out, object_mesh_health(*hole.object));
    CHECK(out == nlohmann::json{{"mesh_warning", true}, {"mesh_warning_reason", "Error: 3 non-manifold edges."}});

    // get_scene_info's objects and load_model's loaded_objects are this summary. Given the health
    // read once for the whole scene, it says the same as reading it itself.
    const nlohmann::json summary = model_object_summary_json(*hole.object, 0);
    CHECK(summary["mesh_warning"] == true);
    CHECK(summary["mesh_warning_reason"] == "Error: 3 non-manifold edges.");
    CHECK(model_object_summary_json(*hole.object, 0, object_mesh_health(*hole.object)) == summary);
    CHECK(model_object_summary_json(*clean.object, 0)["mesh_warning"] == false);
    CHECK_FALSE(model_object_summary_json(*clean.object, 0).contains("mesh_warning_reason"));
}

// An object on no plate (where deleting a plate leaves its objects) is described in
// get_scene_info's unplaced_objects: it carries the same mesh fields as a plate's object entry.
TEST_CASE("An unplaced object carries the mesh warning, and the features when they are asked for", "[MeshHealth][orcamcp]")
{
    OnePartObject hole{TriangleMesh(cube_missing_facet())};
    const MeshHealth       health  = object_mesh_health(*hole.object);
    const InstancesOnPlate nowhere = instances_on_plate(*hole.object, [](int) { return true; }); // every instance

    const nlohmann::json plain = GUI::OrcaMCPPlateUtils::UnplacedObjectJson(*hole.object, 3, nowhere, health, false);
    CHECK(plain["object_index"] == 3);
    CHECK(plain["name"] == "Test object");
    CHECK(plain["mesh_warning"] == true);
    CHECK(plain["mesh_warning_reason"] == "Error: 3 non-manifold edges.");
    CHECK_FALSE(plain.contains("features"));

    const nlohmann::json with_features = GUI::OrcaMCPPlateUtils::UnplacedObjectJson(*hole.object, 3, nowhere, health, true);
    CHECK(with_features["features"] == mesh_features_json(health));
}
