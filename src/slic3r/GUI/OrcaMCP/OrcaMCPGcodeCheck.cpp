// src/slic3r/GUI/OrcaMCP/OrcaMCPGcodeCheck.cpp
#include "OrcaMCPGcodeCheck.hpp"

#include <algorithm>
#include <iterator>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/PartPlate.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

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

} // namespace

GcodeCheckInput gcode_check_input(const GCodeProcessorResult& result)
{
    return {result.toolpath_outside, result.gcode_check_result.error_code, result.filament_printable_reuslt.conflict_filament};
}

std::vector<GcodeCheckProblem> gcode_check_problems(const GcodeCheckInput& check)
{
    std::vector<GcodeCheckProblem> problems;
    for (int bit = 0; bit < 31; ++bit)
        if (check.error_code & (1 << bit))
            problems.push_back(problem_of_bit(bit));
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
    nlohmann::json codes = nlohmann::json::array();
    for (const GcodeCheckProblem& problem : problems)
        codes.push_back(problem.code);
    return {{"ok", false}, {"problems", codes}, {"message", joined_words(problems)}};
}

std::optional<std::string> gcode_check_refusal(int plate_index, const std::vector<GcodeCheckProblem>& problems)
{
    if (problems.empty())
        return std::nullopt;
    return "plate_index " + std::to_string(plate_index) +
           " failed the app's check of its sliced G-code, which keeps its Print and Export buttons off too: " +
           joined_words(problems) + ". Fix that and slice again; get_slicing_status's gcode_check says what is left.";
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
