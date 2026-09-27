// src/slic3r/GUI/OrcaMCP/OrcaMCPGcodeCheck.hpp
#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The check a slice runs on its own G-code (GCodeProcessor::check_multi_extruder_gcode_valid and the
// printed-mass check in GCode::do_export), read the way the GUI reads it before it lets a plate be
// printed, sent or exported: PartPlate::is_slice_result_ready_for_print, which keeps the Print, Send
// and Export buttons off. MCP's export_gcode and send_to_printer refuse on the same answer, and
// get_slicing_status reports it per plate. The decision takes plain values (tests/slic3rutils/
// test_gcode_check.cpp); only plate_gcode_check_* read a plate.

namespace Slic3r {
struct GCodeProcessorResult;
namespace GUI {
class PartPlate;
namespace OrcaMCP {

// The three things is_slice_result_ready_for_print reads from a plate's slice result, and the facts
// behind a toolpath above the printable height.
struct GcodeCheckInput
{
    bool             toolpath_outside = false;  // GCodeProcessorResult::toolpath_outside
    int              error_code       = 0;      // gcode_check_result.error_code, a bit per problem
    std::vector<int> bed_conflict_filaments;    // filament_printable_reuslt.conflict_filament, 0-based
    // Read only when the check found a toolpath above the printable height: the highest layer an
    // extrusion prints at, as the check measured it (its print_z), and the printable height, in mm.
    std::optional<double> highest_layer_z;
    double                printable_height = 0.;
};

GcodeCheckInput gcode_check_input(const GCodeProcessorResult& result);

// One thing the check found: a stable code for agents, the words for a refusal, and, where the
// result has them, the numbers behind it (JSON fields, in mm) and a hint at the usual cause.
struct GcodeCheckProblem
{
    std::string    code;
    std::string    words;
    nlohmann::json facts = nlohmann::json::object();
    std::string    hint;
};

// Everything the check found, in the order of error_code's bits, then the other two. Empty means the
// plate may be printed, sent and exported, exactly when is_slice_result_ready_for_print says so (for
// a plate with a valid result).
std::vector<GcodeCheckProblem> gcode_check_problems(const GcodeCheckInput& check);

// get_slicing_status's per-plate summary: {"ok": true}, or {"ok": false, "problems": [codes],
// "message": the words}, with each problem's facts and a "hint" when it has them.
nlohmann::json gcode_check_json(const std::vector<GcodeCheckProblem>& problems);

// Why export_gcode and send_to_printer refuse the plate, or nullopt when the check found nothing.
std::optional<std::string> gcode_check_refusal(int plate_index, const std::vector<GcodeCheckProblem>& problems);

// Whether the plate has a valid slice result, whose G-code its slice checked.
bool plate_gcode_checked(PartPlate& plate);

// The same for a plate: null / nullopt when it has no valid slice result, which has not been checked.
nlohmann::json             plate_gcode_check_json(PartPlate& plate);
std::optional<std::string> plate_gcode_check_refusal(PartPlate& plate, int plate_index);

} // namespace OrcaMCP
} // namespace GUI
} // namespace Slic3r
