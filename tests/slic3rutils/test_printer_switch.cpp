#include <catch2/catch_test_macros.hpp>

#include <string>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp"

// select_preset {type: printer} says where the plate's filament colours came from, because a printer
// switch replacing them is upstream behaviour an agent cannot see otherwise: on 2026-09-26 one
// concluded "switching printers reset the slot colours" and set all four again.

using Slic3r::GUI::printer_switch_colors_source;

TEST_CASE("A switch to a printer with saved colours takes them", "[orcamcp][presets]")
{
    CHECK(std::string(printer_switch_colors_source(/*remember=*/true, /*changes=*/true, /*saved=*/true)) == "remembered");
}

TEST_CASE("A switch to a printer with no saved colours takes upstream's default", "[orcamcp][presets]")
{
    CHECK(std::string(printer_switch_colors_source(/*remember=*/true, /*changes=*/true, /*saved=*/false)) == "default");
}

TEST_CASE("The colours are kept when the printer does not change or its configuration is not remembered", "[orcamcp][presets]")
{
    CHECK(std::string(printer_switch_colors_source(/*remember=*/true, /*changes=*/false, /*saved=*/true)) == "kept");
    CHECK(std::string(printer_switch_colors_source(/*remember=*/false, /*changes=*/true, /*saved=*/true)) == "kept");
    CHECK(std::string(printer_switch_colors_source(/*remember=*/false, /*changes=*/true, /*saved=*/false)) == "kept");
}
