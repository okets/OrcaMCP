// src/slic3r/GUI/OrcaMCP/OrcaMCPExportTools.hpp
#pragma once

#include <string>

#include <nlohmann/json.hpp>

// The exports that write a file straight away, through the app's own File > Export actions: the plate
// sliced file (export_gcode with a .gcode.3mf path) and the mesh export (export_stl, registered by
// OrcaMCPServer::register_export_tools). Their decisions are OrcaMCPExports.hpp.

namespace Slic3r { namespace GUI {
class Plater;
namespace OrcaMCP {
struct McpDialogSuppressionGuard;

// export_gcode with an output_path ending in .gcode.3mf: the selected plate's G-code, or with `all_plates`
// every plate's, in a 3MF, as Export plate sliced file and Export all plate sliced file write it
// (Plater::export_gcode_3mf, whose file dialog the path answers). Main thread, under the caller's guard.
nlohmann::json export_sliced_file(Plater& plater, McpDialogSuppressionGuard& guard, const std::string& output_path, bool all_plates);

}}} // namespace Slic3r::GUI::OrcaMCP
