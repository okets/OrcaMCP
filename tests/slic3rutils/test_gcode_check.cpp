#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPGcodeCheck.hpp"

// The check a slice runs on its own G-code, as export_gcode and send_to_printer refuse on it and
// get_slicing_status reports it. The GUI keeps a plate's Print, Send and Export buttons off on the
// same answer (PartPlate::is_slice_result_ready_for_print); MCP used to export and send such a plate
// anyway, and on Flashforge a send starts the print.

using namespace Slic3r::GUI::OrcaMCP;

namespace {
std::vector<std::string> codes(const std::vector<GcodeCheckProblem>& problems)
{
    std::vector<std::string> out;
    for (const GcodeCheckProblem& problem : problems)
        out.push_back(problem.code);
    return out;
}
} // namespace

TEST_CASE("a plate whose G-code check found nothing may be exported and sent", "[orcamcp][GcodeCheck]")
{
    const std::vector<GcodeCheckProblem> problems = gcode_check_problems(GcodeCheckInput{});
    CHECK(problems.empty());
    CHECK(gcode_check_json(problems) == nlohmann::json{{"ok", true}});
    CHECK_FALSE(gcode_check_refusal(0, problems).has_value());
}

TEST_CASE("each error the check sets refuses the plate, by name", "[orcamcp][GcodeCheck]")
{
    const auto [bit, code] = GENERATE(table<int, std::string>({
        {0, "outside_extruder_area"},
        {1, "above_extruder_height"},
        {2, "outside_bed"},
        {3, "above_printable_height"},
        {4, "in_wrapping_area"},
        {11, "over_printed_mass"},
        {7, "check_bit_7"}, // a bit this code does not know yet refuses too
    }));
    DYNAMIC_SECTION("error bit " << bit) {
        GcodeCheckInput check;
        check.error_code = 1 << bit;
        const std::vector<GcodeCheckProblem> problems = gcode_check_problems(check);
        REQUIRE(codes(problems) == std::vector<std::string>{code});

        const nlohmann::json summary = gcode_check_json(problems);
        CHECK(summary["ok"] == false);
        CHECK(summary["problems"] == nlohmann::json::array({code}));
        CHECK(summary["message"] == problems.front().words);

        const std::optional<std::string> refusal = gcode_check_refusal(2, problems);
        REQUIRE(refusal.has_value());
        CHECK(refusal->find("plate_index 2") != std::string::npos);
        CHECK(refusal->find(problems.front().words) != std::string::npos);
    }
}

TEST_CASE("the volume and bed-surface checks refuse the plate as the GUI does", "[orcamcp][GcodeCheck]")
{
    GcodeCheckInput outside;
    outside.toolpath_outside = true;
    CHECK(codes(gcode_check_problems(outside)) == std::vector<std::string>{"toolpath_outside"});

    GcodeCheckInput bed;
    bed.bed_conflict_filaments = {1, 2}; // 0-based: filaments 2 and 3
    const std::vector<GcodeCheckProblem> problems = gcode_check_problems(bed);
    REQUIRE(codes(problems) == std::vector<std::string>{"filament_bed_conflict"});
    CHECK(problems.front().words.find("filaments 2, 3") != std::string::npos);
}

TEST_CASE("every problem the check found is named, in the order of its bits", "[orcamcp][GcodeCheck]")
{
    GcodeCheckInput check;
    check.error_code             = (1 << 3) | (1 << 1);
    check.toolpath_outside       = true;
    check.bed_conflict_filaments = {0};
    const std::vector<GcodeCheckProblem> problems = gcode_check_problems(check);
    CHECK(codes(problems) ==
          std::vector<std::string>{"above_extruder_height", "above_printable_height", "toolpath_outside", "filament_bed_conflict"});

    const std::optional<std::string> refusal = gcode_check_refusal(0, problems);
    REQUIRE(refusal.has_value());
    for (const GcodeCheckProblem& problem : problems)
        CHECK(refusal->find(problem.words) != std::string::npos);
}

TEST_CASE("the check is read from the slice result's own fields", "[orcamcp][GcodeCheck]")
{
    Slic3r::GCodeProcessorResult result;
    result.reset();
    CHECK(gcode_check_problems(gcode_check_input(result)).empty());

    result.toolpath_outside                            = true;
    result.gcode_check_result.error_code               = 1 << 3;
    result.filament_printable_reuslt.conflict_filament = {4};
    const GcodeCheckInput check = gcode_check_input(result);
    CHECK(check.toolpath_outside);
    CHECK(check.error_code == (1 << 3));
    CHECK(check.bed_conflict_filaments == std::vector<int>{4});
}
