#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPMeshHealth.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <test_utils.hpp>

#include <string>

// The object list's warning icon beside an object, and the tooltip under it, are what a user sees
// when a mesh has errors. Everything here reads them from the model alone -- the list's own
// mesh_errors_info -- so a headless caller reports exactly what the list shows. The meshes are
// built with known defects, and the tooltip text is spelled out in full: it is the list's wording,
// and a change to it should fail here before an agent quotes a sentence the GUI no longer shows.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

namespace {

const char* const k_hole_tooltip = "Remaining errors:\n\t3 non-manifold edges\n\nClick the icon to repair model object";

// A closed 10 mm cube with its first facet removed: a triangular hole whose three edges each
// leave one neighbour facet with an open edge.
indexed_triangle_set cube_missing_facet()
{
    indexed_triangle_set its = its_make_cube(10.0, 10.0, 10.0);
    its.indices.erase(its.indices.begin());
    return its;
}

indexed_triangle_set translated(indexed_triangle_set its, const Vec3f& offset)
{
    for (Vec3f& v : its.vertices)
        v += offset;
    return its;
}

// `count` separate cubes in one mesh, each 1 mm and 3 mm from the last: `count` shells.
indexed_triangle_set separate_cubes(int count)
{
    indexed_triangle_set its;
    for (int i = 0; i < count; ++i)
        its_merge(its, translated(its_make_cube(1.0, 1.0, 1.0), Vec3f(3.f * i, 0.f, 0.f)));
    return its;
}

RepairedMeshErrors reversed_facets(int count)
{
    RepairedMeshErrors errors;
    errors.facets_reversed = count;
    return errors;
}

std::string utf8(const wxString& text) { return text.ToUTF8().data(); }

// One object holding `mesh` as its only part, with one instance, as a load leaves it.
struct OnePartObject
{
    Model        model;
    ModelObject* object = nullptr;

    explicit OnePartObject(TriangleMesh&& mesh)
    {
        object       = model.add_object();
        object->name = "Test object";
        object->add_volume(std::move(mesh));
        object->add_instance();
    }
};

} // namespace

TEST_CASE("A hole shows the warning icon, with the list's remaining-errors tooltip", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};

    wxString   sidebar;
    int        open_edges = -1;
    const auto info       = GUI::mesh_errors_info(*f.object, -1, &sidebar, &open_edges);
    const auto tooltip    = GUI::mesh_errors_info(*f.object);

    CHECK(tooltip.warning_icon_name == "obj_warning");
    CHECK(utf8(tooltip.tooltip) == k_hole_tooltip);
    // With sidebar_info asked for, the list's sidebar line comes back and the tooltip loses its
    // "click the icon" line, as ObjectList::get_mesh_errors_info always did.
    CHECK(utf8(sidebar) == "Error: 3 non-manifold edges.");
    CHECK(utf8(info.tooltip) == "Remaining errors:\n\t3 non-manifold edges\n");
    CHECK(open_edges == 3);

    // The same text for the object's only volume, from the volume's own stats.
    CHECK(utf8(GUI::mesh_errors_info(*f.object, 0).tooltip) == utf8(tooltip.tooltip));
}

TEST_CASE("Recorded repairs show the warning icon and say how many were repaired", "[MeshHealth][orcamcp]")
{
    // A 3MF's mesh_stat element reaches the mesh through this constructor (bbs_3mf.cpp); it is the
    // only way a repair count gets into a loaded mesh (from_stl records none).
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};

    wxString   sidebar;
    const auto tooltip = GUI::mesh_errors_info(*f.object);
    GUI::mesh_errors_info(*f.object, -1, &sidebar);

    CHECK(tooltip.warning_icon_name == "obj_warning");
    CHECK(utf8(tooltip.tooltip) == "1 error repaired\n\nClick the icon to repair model object");
    CHECK(utf8(sidebar) == "1 error repaired");
}

TEST_CASE("Repairs and a hole together are both named in the tooltip", "[MeshHealth][orcamcp]")
{
    RepairedMeshErrors errors;
    errors.edges_fixed = 2;
    OnePartObject f{TriangleMesh(cube_missing_facet(), errors)};

    wxString   sidebar;
    const auto tooltip = GUI::mesh_errors_info(*f.object);
    GUI::mesh_errors_info(*f.object, -1, &sidebar);

    CHECK(utf8(tooltip.tooltip) ==
          "2 errors repaired\nRemaining errors:\n\t3 non-manifold edges\n\nClick the icon to repair model object");
    CHECK(utf8(sidebar) == "Error: 3 non-manifold edges.\n2 errors repaired");
}

TEST_CASE("A clean mesh has no warning icon and no tooltip", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};

    const auto info = GUI::mesh_errors_info(*f.object);
    CHECK(info.warning_icon_name.empty());
    CHECK(info.tooltip.empty());
}

TEST_CASE("The object row counts a modifier's open edges, as the list does", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    f.object->add_volume(TriangleMesh(cube_missing_facet()), ModelVolumeType::PARAMETER_MODIFIER);

    CHECK(GUI::mesh_errors_info(*f.object).warning_icon_name == "obj_warning");
    CHECK(GUI::mesh_errors_info(*f.object, 0).warning_icon_name.empty());
    CHECK(GUI::mesh_errors_info(*f.object, 1).warning_icon_name == "obj_warning");
}

TEST_CASE("Mesh health reports a hole's numbers with the list's icon and tooltip", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};

    const MeshHealth h = object_mesh_health(*f.object);
    CHECK(h.facets == 11);
    CHECK(h.shells == 1);
    CHECK(h.open_edges == 3);
    CHECK_FALSE(h.manifold());
    CHECK_FALSE(h.repaired());
    CHECK(h.errors_repaired == 0);
    CHECK(h.warning);
    // The list's own text, word for word, from the same function the list calls.
    CHECK(h.tooltip == utf8(GUI::mesh_errors_info(*f.object).tooltip));
    CHECK(h.tooltip == k_hole_tooltip);
    CHECK(h.reason == "Error: 3 non-manifold edges.");
}

TEST_CASE("Mesh health of a clean mesh has no warning, tooltip or reason", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};

    const MeshHealth h = object_mesh_health(*f.object);
    CHECK(h.facets == 12);
    CHECK(h.shells == 1);
    CHECK(h.manifold());
    CHECK_FALSE(h.repaired());
    CHECK_FALSE(h.warning);
    CHECK(h.tooltip.empty());
    CHECK(h.reason.empty());
}

TEST_CASE("Two disjoint cubes in one part are two shells, and no warning", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(separate_cubes(2))};

    const MeshHealth h = object_mesh_health(*f.object);
    CHECK(h.shells == 2);
    CHECK(h.facets == 24);
    CHECK(h.manifold());
    CHECK_FALSE(h.warning);
}

TEST_CASE("Recorded repairs are reported field by field, with the list's count", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};

    const MeshHealth h = object_mesh_health(*f.object);
    CHECK(h.repaired_errors.facets_reversed == 1);
    CHECK(h.errors_repaired == 1);
    CHECK(h.repaired());
    CHECK(h.manifold());
    CHECK(h.warning);
    CHECK(h.tooltip == "1 error repaired\n\nClick the icon to repair model object");
    CHECK(h.reason == "1 error repaired");
}

TEST_CASE("A reason naming a hole and repairs is one line", "[MeshHealth][orcamcp]")
{
    RepairedMeshErrors errors;
    errors.edges_fixed = 2;
    OnePartObject f{TriangleMesh(cube_missing_facet(), errors)};

    CHECK(object_mesh_health(*f.object).reason == "Error: 3 non-manifold edges. 2 errors repaired");
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
    CHECK(h.facets == 12);
    CHECK(h.shells == 1);
    CHECK(h.open_edges == 0);
    CHECK_FALSE(h.repaired());
    CHECK_FALSE(h.warning);
}

TEST_CASE("An object's health counts every volume's errors and only the parts' facets and shells", "[MeshHealth][orcamcp]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    f.object->add_volume(TriangleMesh(separate_cubes(2)), ModelVolumeType::PARAMETER_MODIFIER);
    f.object->add_volume(TriangleMesh(cube_missing_facet()), ModelVolumeType::PARAMETER_MODIFIER);

    const MeshHealth object = object_mesh_health(*f.object);
    CHECK(object.facets == 12);
    CHECK(object.shells == 1);
    CHECK(object.open_edges == 3);
    CHECK(object.warning);

    CHECK_FALSE(volume_mesh_health(*f.object, 0).warning);
    CHECK(volume_mesh_health(*f.object, 1).shells == 2);
    CHECK(volume_mesh_health(*f.object, 2).open_edges == 3);
    CHECK(volume_mesh_health(*f.object, 2).tooltip == k_hole_tooltip);
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

    nlohmann::json features = mesh_features_json(*f.object);
    CHECK_THAT(features["volume_mm3"].get<double>(), WithinAbs(1000.0, 1e-3));
    features.erase("volume_mm3");
    CHECK(features == numbers);

    // The object's volume is its first instance's, scaled as the list's sidebar measures it.
    f.object->instances.front()->set_scaling_factor(Vec3d(2.0, 2.0, 2.0));
    CHECK_THAT(mesh_features_json(*f.object)["volume_mm3"].get<double>(), WithinAbs(8000.0, 1e-3));
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
    CHECK(r["mesh_warning"] == false);
    CHECK(r["tooltip"] == "");
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
    CHECK(r["volumes"][0]["open_edges"] == 3);
}

TEST_CASE("active_warnings lists every object that shows the warning icon, with its tooltip", "[MeshHealth][orcamcp]")
{
    Model model;
    for (int i = 0; i < 3; ++i) {
        ModelObject* object = model.add_object();
        object->name        = "Object " + std::to_string(i);
        object->add_volume(i == 1 ? TriangleMesh(cube_missing_facet()) : TriangleMesh(its_make_cube(10.0, 10.0, 10.0)));
        object->add_instance();
    }

    const nlohmann::json entries = mesh_warning_entries(model);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0] == nlohmann::json{{"level", "warning"},
                                       {"type", "MeshErrors"},
                                       {"object_id", 1},
                                       {"object_name", "Object 1"},
                                       {"message", k_hole_tooltip}});

    CHECK(mesh_warning_entries(Model()).empty());
}

TEST_CASE("Every object description flags the warning icon, with the reason when it shows", "[MeshHealth][orcamcp]")
{
    OnePartObject clean{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    OnePartObject hole{TriangleMesh(cube_missing_facet())};

    nlohmann::json out;
    add_mesh_warning(out, *clean.object);
    CHECK(out == nlohmann::json{{"mesh_warning", false}});

    out = nlohmann::json::object();
    add_mesh_warning(out, *hole.object);
    CHECK(out == nlohmann::json{{"mesh_warning", true}, {"mesh_warning_reason", "Error: 3 non-manifold edges."}});

    // get_scene_info's objects and load_model's loaded_objects are this summary.
    const nlohmann::json summary = model_object_summary_json(*hole.object, 0);
    CHECK(summary["mesh_warning"] == true);
    CHECK(summary["mesh_warning_reason"] == "Error: 3 non-manifold edges.");
    CHECK(model_object_summary_json(*clean.object, 0)["mesh_warning"] == false);
    CHECK_FALSE(model_object_summary_json(*clean.object, 0).contains("mesh_warning_reason"));
}
