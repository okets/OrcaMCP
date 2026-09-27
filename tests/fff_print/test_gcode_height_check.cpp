#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

// The G-code check after a slice (GCodeProcessor::check_multi_extruder_gcode_valid) compares each
// extrusion's print_z -- the height of the layer it belongs to -- with the printer's printable
// heights. GCode::process_layer writes that height as "; Z_HEIGHT:" for Bambu printers and ";Z:"
// for every other; the processor used to read only the first, so on every other printer print_z
// stayed 0 and the check never tripped.
namespace {

// gcode_check_result.error_code bits the processor sets (GCodeProcessor.cpp).
constexpr int OVER_EXTRUDER_HEIGHT  = 1 << 1;
constexpr int OVER_PRINTABLE_HEIGHT = 1 << 3;

// Two layers of outer wall, the second at top_z, tagged the way GCode::process_layer tags a layer
// for a Bambu printer or for any other.
std::string two_layer_gcode(bool bambu, double top_z)
{
    GCodeProcessor::s_IsBBLPrinter = bambu;
    const std::string role = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) + "Outer wall\n";
    const auto layer = [bambu](double z) {
        std::ostringstream tags;
        tags << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) << "\n"
             << (bambu ? "; Z_HEIGHT: " : ";Z:") << z << "\n";
        return tags.str();
    };
    std::ostringstream gcode;
    gcode << "M83\n"
          << layer(0.2) << role << "G1 X10 Y10 Z0.2 F600\nG1 X50 Y10 E2 F1800\n"
          << layer(top_z) << role << "G1 Z" << top_z << "\nG1 X50 Y50 E2\n";
    return gcode.str();
}

// Runs `gcode` through a processor configured for a printer of this kind. s_IsBBLPrinter picks the
// tag table and other tests in this binary change it, so it is pinned here.
void process_gcode(GCodeProcessor& processor, bool bambu, const std::string& gcode)
{
    GCodeProcessor::s_IsBBLPrinter = bambu;
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream os(temp.string());
        os << gcode;
    }
    processor.apply_config(config);
    processor.process_file(temp.string());
}

// The check's error code for one filament, printed on extruder 1, on a 200 mm bed.
int check_error_code(GCodeProcessor& processor, double printable_height, const std::vector<double>& extruder_heights)
{
    const size_t  extruders = std::max<size_t>(1, extruder_heights.size());
    const Pointfs bed       = {{0, 0}, {200, 0}, {200, 200}, {0, 200}};
    processor.check_multi_extruder_gcode_valid(int(extruders), bed, printable_height, {}, std::vector<Polygons>(extruders),
                                               extruder_heights, {1}, std::vector<std::set<int>>(extruders));
    return processor.get_result().gcode_check_result.error_code;
}

// Every extrusion but custom G-code's (the start and end G-code), in print order.
std::vector<const GCodeProcessorResult::MoveVertex*> extrusions(const GCodeProcessorResult& result)
{
    std::vector<const GCodeProcessorResult::MoveVertex*> out;
    for (const GCodeProcessorResult::MoveVertex& move : result.moves)
        if (move.type == EMoveType::Extrude && move.extrusion_role != erCustom)
            out.push_back(&move);
    return out;
}

// Slices and exports `print`, filling `result` with the processor result the export produced.
void export_result(Print& print, GCodeProcessorResult& result)
{
    ScopedTemporaryFile temp(".gcode");
    print.process();
    print.export_gcode(temp.string(), &result, nullptr);
}

// Two nozzles, one filament each, and no extruder_printable_height of their own: the config of
// every non-Bambu multi-extruder printer profile.
DynamicPrintConfig two_nozzle_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "nozzle_diameter",                "0.4,0.4" },
        { "printer_extruder_id",            "1,2" },
        { "printer_extruder_variant",       "Direct Drive Standard,Direct Drive Standard" },
        { "extruder_printable_height",      "0,0" },
        { "single_extruder_multi_material", 0 },
    });
    config.set_deserialize_strict(extra);
    return config;
}

} // namespace

TEST_CASE("A layer's Z tag sets the print height of the moves that follow it", "[GCodeHeightCheck]")
{
    // Each layer of the G-code prints at its own Z, so every extrusion's print height is its Z.
    const bool bambu = GENERATE(false, true);
    DYNAMIC_SECTION((bambu ? "Bambu printer" : "other printer")) {
        const double   top_z = 3.2;
        GCodeProcessor processor;
        process_gcode(processor, bambu, two_layer_gcode(bambu, top_z));

        const auto moves = extrusions(processor.get_result());
        REQUIRE_FALSE(moves.empty());
        CHECK_THAT(moves.back()->position.z(), WithinAbs(top_z, 1e-6));
        for (const GCodeProcessorResult::MoveVertex* move : moves)
            CHECK_THAT(move->print_z, WithinAbs(move->position.z(), 1e-6));
    }
}

TEST_CASE("A toolpath above the printable height fails the G-code check on any printer", "[GCodeHeightCheck]")
{
    const bool bambu = GENERATE(false, true);
    const auto [top_z, over] = GENERATE(table<double, bool>({ {20.4, true}, {19.8, false} }));
    DYNAMIC_SECTION((bambu ? "Bambu printer" : "other printer") << ", top layer at " << top_z) {
        const double   printable_height = 20.;
        GCodeProcessor processor;
        process_gcode(processor, bambu, two_layer_gcode(bambu, top_z));

        const int error_code = check_error_code(processor, printable_height, {});
        CHECK(((error_code & OVER_PRINTABLE_HEIGHT) != 0) == over);
    }
}

TEST_CASE("An extruder with no printable height of its own sets no height limit", "[GCodeHeightCheck]")
{
    // The filament prints on extruder 1 at 10 mm, well inside the 250 mm bed. Only a height set on
    // extruder 1 that it exceeds is an extruder height error; 0 is the default every profile
    // without extruder heights carries, and means unset.
    const auto [extruder_heights, over] = GENERATE(table<std::vector<double>, bool>({
        { {0., 0.}, false },
        { {5., 0.}, true  },
        { {0., 5.}, false },
        { {320., 325.}, false },
    }));
    DYNAMIC_SECTION("extruder heights " << extruder_heights[0] << ", " << extruder_heights[1]) {
        GCodeProcessor processor;
        process_gcode(processor, false, two_layer_gcode(false, 10.));

        const int error_code = check_error_code(processor, 250., extruder_heights);
        CHECK(((error_code & OVER_EXTRUDER_HEIGHT) != 0) == over);
        CHECK((error_code & OVER_PRINTABLE_HEIGHT) == 0);
    }
}

TEST_CASE("A raft that lifts a fitting object above the printable height fails the G-code check", "[GCodeHeightCheck]")
{
    // The pre-slice check measures the object without its raft, so a 20 mm cube under a 20.5 mm
    // printable height passes it; three raft layers then lift its top layer past 20.5 mm.
    const double printable_height = 20.5;
    const int    raft_layers      = GENERATE(0, 3);
    DYNAMIC_SECTION("raft layers " << raft_layers) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({ { "printable_height", printable_height }, { "raft_layers", raft_layers } });
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, config);
        GCodeProcessorResult result;
        export_result(print, result);

        const double top_z = print.objects().front()->layers().back()->print_z;
        const bool   over  = top_z > printable_height;
        REQUIRE(over == (raft_layers > 0));
        CHECK(((result.gcode_check_result.error_code & OVER_PRINTABLE_HEIGHT) != 0) == over);
    }
}

TEST_CASE("A normal slice on a multi-extruder printer without extruder heights passes the G-code check", "[GCodeHeightCheck]")
{
    // One cube per extruder, so each extruder's height check runs.
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> per_object = { { { "extruder", 1 } }, { { "extruder", 2 } } };
    init_print(std::vector<TriangleMesh>{ cube(20), cube(20) }, print, model, two_nozzle_config({ { "printable_height", 250 } }),
               &per_object);
    GCodeProcessorResult result;
    export_result(print, result);

    CHECK((result.gcode_check_result.error_code & OVER_EXTRUDER_HEIGHT) == 0);
    CHECK((result.gcode_check_result.error_code & OVER_PRINTABLE_HEIGHT) == 0);

    // print_z is each extrusion's real layer height: above the bed, and at most the top layer's.
    const double top_z = print.objects().front()->layers().back()->print_z;
    const auto   moves = extrusions(result);
    REQUIRE_FALSE(moves.empty());
    const auto [lowest, highest] = std::minmax_element(moves.begin(), moves.end(),
        [](const auto* a, const auto* b) { return a->print_z < b->print_z; });
    CHECK((*lowest)->print_z > 0.f);
    CHECK_THAT((*highest)->print_z, WithinAbs(top_z, 1e-4));
}
