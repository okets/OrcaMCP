#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPExports.hpp"

// What export_gcode's sliced-file form and export_stl decide before the app writes anything. The files
// are the app's own File > Export actions; these decide which one a call asks for and refuse, before
// anything is written, what the GUI's Export items would be off for.

using namespace Slic3r::GUI::OrcaMCP;

namespace {
SlicedFilePlate sliced(int index) { return {index, /*has_objects=*/true, /*all_unprintable=*/false, /*sliced=*/true, std::nullopt}; }
SlicedFilePlate unsliced(int index) { return {index, true, false, false, std::nullopt}; }
SlicedFilePlate empty_plate(int index) { return {index, false, false, false, std::nullopt}; }
SlicedFilePlate unprintable(int index) { return {index, true, /*all_unprintable=*/true, false, std::nullopt}; }
SlicedFilePlate failed_check(int index) { return {index, true, false, true, std::string("plate_index " + std::to_string(index) + " failed the app's check")}; }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
} // namespace

// ---- export_gcode ------------------------------------------------------------------------------

TEST_CASE("a .gcode.3mf path asks for the sliced file, any other for plain G-code", "[McpExports][orcamcp]")
{
    CHECK(gcode_export_kind("/tmp/a.gcode.3mf") == GcodeExportKind::sliced_file);
    CHECK(gcode_export_kind("/tmp/A.GCODE.3MF") == GcodeExportKind::sliced_file);
    CHECK(gcode_export_kind("/tmp/a.gcode") == GcodeExportKind::gcode);
}

TEST_CASE("a sliced file is written where it was asked for, whatever the case of its .3mf", "[McpExports][orcamcp]")
{
    CHECK(sliced_file_path("/tmp/part.gcode.3mf") == "/tmp/part.gcode.3mf");
    CHECK(sliced_file_path("/tmp/PART.GCODE.3MF") == "/tmp/PART.GCODE.3MF");
    CHECK(sliced_file_path("/tmp/part.Gcode.3Mf") == "/tmp/part.Gcode.3Mf");
    // A name the file dialog gets without the extension has it added, as the app does.
    CHECK(sliced_file_path("/tmp/part") == "/tmp/part.3mf");
    CHECK(sliced_file_path("/tmp/part.gcode") == "/tmp/part.gcode.3mf");
}

TEST_CASE("export_gcode refuses a path it would write as something else, before looking at the plates", "[McpExports][orcamcp]")
{
    CHECK(contains(*gcode_export_path_refusal("", false), "output_path is required"));
    // A project 3MF is export_3mf's.
    CHECK(contains(*gcode_export_path_refusal("/tmp/project.3mf", false), "export_3mf"));
    // A plain G-code file holds one plate.
    CHECK(contains(*gcode_export_path_refusal("/tmp/a.gcode", true), ".gcode.3mf"));
    CHECK_FALSE(gcode_export_path_refusal("/tmp/a.gcode", false));
    CHECK_FALSE(gcode_export_path_refusal("/tmp/a.gcode.3mf", true));
}

TEST_CASE("the selected plate's sliced file needs that plate sliced, and its check passed", "[McpExports][orcamcp]")
{
    CHECK_FALSE(sliced_file_refusal({sliced(0), unsliced(1)}, /*all_plates=*/false, /*selected=*/0));
    CHECK(contains(*sliced_file_refusal({sliced(0), unsliced(1)}, false, 1), "plate_index 1 has no slice result: slice_all"));
    CHECK(contains(*sliced_file_refusal({failed_check(0)}, false, 0), "failed the app's check"));
    CHECK(contains(*sliced_file_refusal({empty_plate(0)}, false, 0), "nothing on it"));
}

TEST_CASE("every plate's sliced file needs every plate with a printable object sliced", "[McpExports][orcamcp]")
{
    // As the GUI's Export all plate sliced file: empty plates and plates of unprintable objects do not count.
    CHECK_FALSE(sliced_file_refusal({sliced(0), empty_plate(1), unprintable(2), sliced(3)}, /*all_plates=*/true, 0));
    CHECK(contains(*sliced_file_refusal({sliced(0), unsliced(1)}, true, 0), "plate_index 1 has no slice result"));
    CHECK(contains(*sliced_file_refusal({sliced(0), failed_check(1)}, true, 0), "plate_index 1 failed"));
    CHECK(contains(*sliced_file_refusal({empty_plate(0), unprintable(1)}, true, 0), "no plate has a printable object"));
}

TEST_CASE("the sliced file holds the selected plate, or every sliced plate", "[McpExports][orcamcp]")
{
    const std::vector<SlicedFilePlate> plates = {sliced(0), empty_plate(1), sliced(2)};
    CHECK(sliced_file_plates(plates, /*all_plates=*/false, 2) == std::vector<int>{2});
    CHECK(sliced_file_plates(plates, /*all_plates=*/true, 2) == std::vector<int>{0, 2});
}

// ---- export_stl --------------------------------------------------------------------------------

namespace {
bool no_folder(const std::string&) { return false; }
bool any_folder(const std::string&) { return true; }
MeshExportRequest request(std::string path) { MeshExportRequest r; r.output_path = std::move(path); return r; }
} // namespace

TEST_CASE("one file's format comes from its extension, and a format that disagrees is refused", "[McpExports][orcamcp]")
{
    const MeshExportDecision stl = plan_mesh_export(request("/tmp/a.stl"), 2, no_folder);
    REQUIRE(stl.plan);
    CHECK_FALSE(stl.plan->drc);
    CHECK_FALSE(stl.plan->one_file_per_object);
    CHECK_FALSE(stl.plan->selection_only);

    MeshExportRequest drc = request("/tmp/a.DRC");
    CHECK(plan_mesh_export(drc, 2, no_folder).plan->drc);
    drc.format = "stl";
    CHECK(contains(plan_mesh_export(drc, 2, no_folder).refusal, "make them match"));

    CHECK(contains(plan_mesh_export(request("/tmp/a.obj"), 2, no_folder).refusal, "must end in .stl or .drc"));
    MeshExportRequest bad = request("/tmp/a.stl");
    bad.format = "obj";
    CHECK(contains(plan_mesh_export(bad, 2, no_folder).refusal, "format must be stl or drc"));
}

TEST_CASE("one file per object writes into an existing folder, STL unless asked for DRC", "[McpExports][orcamcp]")
{
    MeshExportRequest folder = request("/tmp/out");
    folder.one_file_per_object = true;
    CHECK(contains(plan_mesh_export(folder, 2, no_folder).refusal, "not an existing folder"));
    const MeshExportDecision stl = plan_mesh_export(folder, 2, any_folder);
    REQUIRE(stl.plan);
    CHECK(stl.plan->one_file_per_object);
    CHECK_FALSE(stl.plan->drc);
    folder.format = "drc";
    CHECK(plan_mesh_export(folder, 2, any_folder).plan->drc);
}

TEST_CASE("object_ids export those objects as the object menu does, and bad ids are refused", "[McpExports][orcamcp]")
{
    MeshExportRequest some = request("/tmp/a.stl");
    some.object_ids = std::vector<int>{1, 0};
    const MeshExportDecision chosen = plan_mesh_export(some, 3, no_folder);
    REQUIRE(chosen.plan);
    CHECK(chosen.plan->selection_only);
    CHECK(chosen.plan->object_ids == std::vector<int>{1, 0});

    some.object_ids = std::vector<int>{3};
    CHECK(contains(plan_mesh_export(some, 3, no_folder).refusal, "Invalid object_id 3"));
    some.object_ids = std::vector<int>{1, 1};
    CHECK(contains(plan_mesh_export(some, 3, no_folder).refusal, "listed twice"));
    some.object_ids = std::vector<int>{};
    CHECK(contains(plan_mesh_export(some, 3, no_folder).refusal, "leave it out"));
}

TEST_CASE("an empty scene or a missing path has nothing to export", "[McpExports][orcamcp]")
{
    CHECK(contains(plan_mesh_export(request("/tmp/a.stl"), 0, no_folder).refusal, "no objects"));
    CHECK(contains(plan_mesh_export(request(""), 1, no_folder).refusal, "output_path is required"));
}
