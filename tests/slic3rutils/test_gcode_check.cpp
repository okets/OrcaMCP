#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
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

TEST_CASE("a toolpath above the printable height is refused with the heights that make it, and the usual cause", "[orcamcp][GcodeCheck]")
{
    GcodeCheckInput check;
    check.error_code       = 1 << 3;
    check.highest_layer_z  = 20.5000004; // print_z is a float
    check.printable_height = 20.;
    const std::vector<GcodeCheckProblem> problems = gcode_check_problems(check);
    REQUIRE(codes(problems) == std::vector<std::string>{"above_printable_height"});
    CHECK(problems.front().words.find("the highest layer prints at 20.5 mm") != std::string::npos);
    CHECK(problems.front().words.find("the printable height is 20 mm") != std::string::npos);

    const nlohmann::json summary = gcode_check_json(problems);
    CHECK(summary["highest_layer_z_mm"] == 20.5);
    CHECK(summary["printable_height_mm"] == 20.);
    REQUIRE(summary.contains("hint"));
    CHECK(summary["hint"].get<std::string>().find("raft_layers") != std::string::npos);

    const std::optional<std::string> refusal = gcode_check_refusal(0, problems);
    REQUIRE(refusal.has_value());
    CHECK(refusal->find("20.5 mm") != std::string::npos);
    CHECK(refusal->find(summary["hint"].get<std::string>()) != std::string::npos);
}

TEST_CASE("without the heights, a toolpath above the printable height is refused with the cause alone", "[orcamcp][GcodeCheck]")
{
    GcodeCheckInput check;
    check.error_code = 1 << 3;
    const std::vector<GcodeCheckProblem> problems = gcode_check_problems(check);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().words.find(" mm") == std::string::npos);

    const nlohmann::json summary = gcode_check_json(problems);
    CHECK_FALSE(summary.contains("highest_layer_z_mm"));
    CHECK(summary.contains("hint"));
}

TEST_CASE("the highest layer is read from the result's extrusions, and only when the height check failed", "[orcamcp][GcodeCheck]")
{
    Slic3r::GCodeProcessorResult result;
    result.reset();
    result.printable_height = 20.f;
    const auto add_move = [&result](Slic3r::EMoveType type, Slic3r::ExtrusionRole role, float print_z) {
        Slic3r::GCodeProcessorResult::MoveVertex move;
        move.type           = type;
        move.extrusion_role = role;
        move.print_z        = print_z;
        result.moves.push_back(move);
    };
    add_move(Slic3r::EMoveType::Extrude, Slic3r::erExternalPerimeter, 20.5f);
    add_move(Slic3r::EMoveType::Extrude, Slic3r::erCustom, 30.f);    // the end G-code's extrusion is not a layer
    add_move(Slic3r::EMoveType::Travel, Slic3r::erExternalPerimeter, 40.f); // nor is a travel

    CHECK_FALSE(gcode_check_input(result).highest_layer_z.has_value());

    result.gcode_check_result.error_code = 1 << 3;
    const GcodeCheckInput check = gcode_check_input(result);
    REQUIRE(check.highest_layer_z.has_value());
    CHECK(std::abs(*check.highest_layer_z - 20.5) < 1e-6);
    CHECK(check.printable_height == 20.);
}
