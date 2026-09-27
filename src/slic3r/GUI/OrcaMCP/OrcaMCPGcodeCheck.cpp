// src/slic3r/GUI/OrcaMCP/OrcaMCPGcodeCheck.cpp
#include "OrcaMCPGcodeCheck.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/PartPlate.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

constexpr int above_printable_height_bit = 3;

struct CheckBit
{
    int         bit;
    const char* code;
    const char* words;
};

// gcode_check_result.error_code's bits, as GCodeProcessor::check_multi_extruder_gcode_valid and
// GCode::do_export (the printed mass) set them.
constexpr CheckBit known_bits[] = {
    {0, "outside_extruder_area", "a toolpath is outside the area its extruder can reach"},
    {1, "above_extruder_height", "a toolpath is above its extruder's printable height"},
    {2, "outside_bed", "a toolpath is outside the bed's printable area"},
    {3, "above_printable_height", "a toolpath is above the printer's printable height"},
    {4, "in_wrapping_area", "a toolpath is inside the wrapping detection area"},
    {11, "over_printed_mass", "the print is heavier than the printer's maximum printed mass"},
};

GcodeCheckProblem problem_of_bit(int bit)
{
    const auto known = std::find_if(std::begin(known_bits), std::end(known_bits), [bit](const CheckBit& b) { return b.bit == bit; });
    if (known != std::end(known_bits))
        return {known->code, known->words};
    return {"check_bit_" + std::to_string(bit), "the check failed with error bit " + std::to_string(bit)};
}

double rounded_mm(double value) { return std::round(value * 1000.) / 1000.; }

std::string mm(double value)
{
    std::ostringstream out;
    out << rounded_mm(value) << " mm";
    return out.str();
}

// The problem, with the heights that make it when the result had them. The likeliest cause is a raft:
// the check before slicing (Print::validate) measures each object without its raft and refuses one
// whose own top layer is above the printable height, so what reaches this check is lifted by a raft.
GcodeCheckProblem above_printable_height_problem(const GcodeCheckInput& check)
{
    GcodeCheckProblem problem = problem_of_bit(above_printable_height_bit);
    if (check.highest_layer_z) {
        problem.words += " (the highest layer prints at " + mm(*check.highest_layer_z) + "; the printable height is " +
                         mm(check.printable_height) + ")";
        problem.facts = {{"highest_layer_z_mm", rounded_mm(*check.highest_layer_z)}, {"printable_height_mm", rounded_mm(check.printable_height)}};
    }
    problem.hint = "The app's check before slicing measures each object without its raft, so a raft lifts an object that fits "
                   "by the raft's thickness; fewer raft_layers, or a lower object, keeps its top layer within the printable height.";
    return problem;
}

// The highest layer an extrusion prints at, custom G-code's (start and end G-code) aside, as the check
// measures it; nullopt without one.
std::optional<double> highest_extrusion_layer_z(const GCodeProcessorResult& result)
{
    std::optional<double> highest;
    for (const GCodeProcessorResult::MoveVertex& move : result.moves)
        if (move.type == EMoveType::Extrude && move.extrusion_role != erCustom && (!highest || move.print_z > *highest))
            highest = move.print_z;
    return highest;
}

GcodeCheckProblem bed_conflict_problem(const std::vector<int>& filaments)
{
    std::string ids;
    for (int filament : filaments)
        ids += (ids.empty() ? "" : ", ") + std::to_string(filament + 1);
    const bool several = filaments.size() > 1;
    return {"filament_bed_conflict", std::string(several ? "filaments " : "filament ") + ids +
                                         " cannot be printed directly on this plate's surface: the plate type has no "
                                         "first-layer bed temperature for " + (several ? "them" : "it")};
}

std::string joined_words(const std::vector<GcodeCheckProblem>& problems)
{
    std::string words;
    for (const GcodeCheckProblem& problem : problems)
        words += (words.empty() ? "" : "; ") + problem.words;
    return words;
}

std::string joined_hints(const std::vector<GcodeCheckProblem>& problems)
{
    std::string hints;
    for (const GcodeCheckProblem& problem : problems)
        if (!problem.hint.empty())
            hints += (hints.empty() ? "" : " ") + problem.hint;
    return hints;
}

} // namespace

GcodeCheckInput gcode_check_input(const GCodeProcessorResult& result)
{
    GcodeCheckInput input{result.toolpath_outside, result.gcode_check_result.error_code, result.filament_printable_reuslt.conflict_filament};
    // Walking the moves costs, so only for a plate the height check failed.
    if (input.error_code & (1 << above_printable_height_bit)) {
        input.highest_layer_z  = highest_extrusion_layer_z(result);
        input.printable_height = result.printable_height;
    }
    return input;
}

std::vector<GcodeCheckProblem> gcode_check_problems(const GcodeCheckInput& check)
{
    std::vector<GcodeCheckProblem> problems;
    for (int bit = 0; bit < 31; ++bit)
        if (check.error_code & (1 << bit))
            problems.push_back(bit == above_printable_height_bit ? above_printable_height_problem(check) : problem_of_bit(bit));
    if (check.toolpath_outside)
        problems.push_back({"toolpath_outside", "a toolpath is outside the printable volume"});
    if (!check.bed_conflict_filaments.empty())
        problems.push_back(bed_conflict_problem(check.bed_conflict_filaments));
    return problems;
}

nlohmann::json gcode_check_json(const std::vector<GcodeCheckProblem>& problems)
{
    if (problems.empty())
        return {{"ok", true}};
    nlohmann::json summary = {{"ok", false}, {"problems", nlohmann::json::array()}, {"message", joined_words(problems)}};
    for (const GcodeCheckProblem& problem : problems) {
        summary["problems"].push_back(problem.code);
        summary.update(problem.facts);
    }
    if (const std::string hints = joined_hints(problems); !hints.empty())
        summary["hint"] = hints;
    return summary;
}

std::optional<std::string> gcode_check_refusal(int plate_index, const std::vector<GcodeCheckProblem>& problems)
{
    if (problems.empty())
        return std::nullopt;
    const std::string hints = joined_hints(problems);
    return "plate_index " + std::to_string(plate_index) +
           " failed the app's check of its sliced G-code, which keeps its Print and Export buttons off too: " +
           joined_words(problems) + "." + (hints.empty() ? "" : " " + hints) +
           " Fix that and slice again; get_slicing_status's gcode_check says what is left.";
}

bool plate_gcode_checked(PartPlate& plate) { return plate.is_slice_result_valid() && plate.get_slice_result() != nullptr; }

nlohmann::json plate_gcode_check_json(PartPlate& plate)
{
    if (!plate_gcode_checked(plate))
        return nullptr;
    return gcode_check_json(gcode_check_problems(gcode_check_input(*plate.get_slice_result())));
}

std::optional<std::string> plate_gcode_check_refusal(PartPlate& plate, int plate_index)
{
    if (!plate_gcode_checked(plate))
        return std::nullopt;
    return gcode_check_refusal(plate_index, gcode_check_problems(gcode_check_input(*plate.get_slice_result())));
}

}}} // namespace Slic3r::GUI::OrcaMCP
