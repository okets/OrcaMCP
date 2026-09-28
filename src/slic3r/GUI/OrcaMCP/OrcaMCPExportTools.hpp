// src/slic3r/GUI/OrcaMCP/OrcaMCPExportTools.hpp
#pragma once

#include <string>

#include <nlohmann/json.hpp>

// The exports through the app's own File > Export actions: the plate sliced file (export_gcode with a .gcode.3mf
// path) and the mesh export (export_stl, registered by OrcaMCPServer::register_export_tools), written straight away,
// and the wait for a plain .gcode, which the app writes in the background. Their decisions are OrcaMCPExports.hpp.

namespace Slic3r { namespace GUI {
class Plater;
namespace OrcaMCP {
struct McpDialogSuppressionGuard;

// export_gcode with an output_path ending in .gcode.3mf: the selected plate's G-code, or with `all_plates`
// every plate's, in a 3MF, as Export plate sliced file and Export all plate sliced file write it
// (Plater::export_gcode_3mf, whose file dialog the path answers). Main thread, under the caller's guard.
nlohmann::json export_sliced_file(Plater& plater, McpDialogSuppressionGuard& guard, const std::string& output_path, bool all_plates);

class GcodeExportOutcome;
// export_gcode with an output_path ending in .gcode, once the app has begun writing it in the background (`started`,
// the call's export_started answer): waits for the export on the HTTP thread -- never inside main-thread work, which
// the export's completion needs -- until `outcome` has ended, the app has taken the export off, the call's wait cap
// (tool_wait_cap) has passed, or the app quits (this_thread_cancelled), then answers gcode_export_answer's, with
// `started`'s info_messages and the active warnings. HTTP thread.
nlohmann::json wait_for_gcode_export(GcodeExportOutcome& outcome, const std::string& output_path, const nlohmann::json& started);

}}} // namespace Slic3r::GUI::OrcaMCP
