// src/slic3r/GUI/OrcaMCP/OrcaMCPExports.hpp
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

// What export_gcode's sliced-file form and export_stl decide before the app writes anything: which
// file a path asks for, and why a call is refused. No wx and no Plater: the tests drive it with plain
// values (tests/slic3rutils/test_mcp_exports.cpp). The files themselves are written by the app's own
// exports -- Plater::export_gcode_3mf (File > Export > Export plate sliced file) and Plater::export_stl
// (File > Export > Export all objects as one STL / as STLs, and the object menu's Export as one STL) --
// whose file dialogs the call's path answers (mcp_answer_path_dialog).

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// ---- export_gcode ------------------------------------------------------------------------------

enum class GcodeExportKind
{
    gcode,       // a plain .gcode of the selected plate, written by the background process
    sliced_file, // a .gcode.3mf: the plate's, or every plate's, G-code inside a 3MF (the plate sliced file)
};

// What `output_path` asks for: a path ending in .gcode.3mf (any case) is a sliced file.
GcodeExportKind gcode_export_kind(const std::string& output_path);

// Why export_gcode refuses `output_path` with `all_plates` before looking at the plates, or nullopt: no
// path; a .3mf that is not a .gcode.3mf (a project, which export_3mf writes); all_plates for a plain
// .gcode, which holds one plate.
std::optional<std::string> gcode_export_path_refusal(const std::string& output_path, bool all_plates);

// One plate as the app's Export plate sliced file reads it (PartPlate).
struct SlicedFilePlate
{
    int                        index           = -1;
    bool                       has_objects     = false; // !PartPlate::empty()
    bool                       all_unprintable = false; // PartPlate::is_all_instances_unprintable
    bool                       sliced          = false; // a valid slice result
    std::optional<std::string> check_refusal;           // what the check its slice ran on its G-code found
};

// Why the sliced file cannot be written, or nullopt, as the menu's own enabling decides it: the
// selected plate (MainFrame::can_export_gcode), or every plate with a printable object on it, of which
// at least one (MainFrame::can_export_all_gcode, PartPlateList::is_all_slice_results_ready_for_print).
// A plate without a slice result names slice_all; one whose G-code check failed, what it found.
std::optional<std::string> sliced_file_refusal(const std::vector<SlicedFilePlate>& plates, bool all_plates, int selected_index);

// The plates whose G-code the file holds: the selected one, or every plate with a slice result.
std::vector<int> sliced_file_plates(const std::vector<SlicedFilePlate>& plates, bool all_plates, int selected_index);

// ---- export_stl --------------------------------------------------------------------------------

// export_stl's arguments, as the handler read them.
struct MeshExportRequest
{
    std::string                     output_path;
    std::optional<std::string>      format;             // "stl" or "drc"; nullopt: from output_path's extension
    std::optional<std::vector<int>> object_ids;         // nullopt: every object
    bool                            one_file_per_object = false;
};

// What the app is asked to write.
struct MeshExportPlan
{
    bool             drc                 = false; // Draco, else STL
    bool             one_file_per_object = false; // output_path is a folder, one file per object (or instance)
    bool             selection_only      = false; // the object menu's export of the objects named, else the File menu's of all
    std::vector<int> object_ids;                  // the objects selected for a selection_only export
};

// The plan for `request` over a scene of `object_count` objects, or the refusal: no objects; an
// object_id out of range or listed twice; an empty object_ids; a format other than stl or drc; a file
// whose extension is not the format's; a folder that does not exist (`is_folder`) for one file per object.
struct MeshExportDecision
{
    std::optional<MeshExportPlan> plan;
    std::string                   refusal; // when there is no plan
};
MeshExportDecision plan_mesh_export(const MeshExportRequest& request, int object_count, const std::function<bool(const std::string&)>& is_folder);

}}} // namespace Slic3r::GUI::OrcaMCP
