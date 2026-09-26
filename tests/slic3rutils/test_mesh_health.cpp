#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>

// The object list's warning icon beside an object, and the tooltip under it, are what a user sees
// when a mesh has errors. Everything here reads them from the model alone -- the list's own
// mesh_errors_info -- so a headless caller reports exactly what the list shows. The meshes are
// built with known defects, and the tooltip text is spelled out in full: it is the list's wording,
// and a change to it should fail here before an agent quotes a sentence the GUI no longer shows.

using namespace Slic3r;

namespace {

// A closed 10 mm cube with its first facet removed: a triangular hole whose three edges each
// leave one neighbour facet with an open edge.
indexed_triangle_set cube_missing_facet()
{
    indexed_triangle_set its = its_make_cube(10.0, 10.0, 10.0);
    its.indices.erase(its.indices.begin());
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
    CHECK(utf8(tooltip.tooltip) == "Remaining errors:\n\t3 non-manifold edges\n\nClick the icon to repair model object");
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
