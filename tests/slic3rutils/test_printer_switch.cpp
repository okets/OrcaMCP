#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp"

// select_preset {type: printer} says where each slot's colour came from, observed by comparing the
// colours before and after the switch: a printer switch replacing them is upstream behaviour an agent
// cannot see otherwise. On 2026-09-26 one concluded "switching printers reset the slot colours" and
// set all four again.

using Slic3r::GUI::printer_switch_color_sources;
using Slic3r::GUI::summarize_color_sources;
using Strings = std::vector<std::string>;

TEST_CASE("A slot that took the colour saved for the new printer is remembered", "[orcamcp][presets]")
{
    CHECK(printer_switch_color_sources({"#111111", "#222222"}, {"#AAAAAA", "#BBBBBB"}, {"#AAAAAA", "#BBBBBB"}) ==
          Strings{"remembered", "remembered"});
}

TEST_CASE("A slot the saved colours do not cover is upstream's padded default", "[orcamcp][presets]")
{
    // A four-slot printer with one saved colour: update_selections pads the rest with #26A69A.
    CHECK(printer_switch_color_sources({"#111111"}, {"#AAAAAA", "#26A69A", "#26A69A", "#26A69A"}, {"#AAAAAA"}) ==
          Strings{"remembered", "default", "default", "default"});
    CHECK(printer_switch_color_sources({"#111111"}, {"#26a69a"}, {}) == Strings{"default"}); // case does not matter
}

TEST_CASE("A slot whose colour did not change is unchanged, even if it is also the saved one", "[orcamcp][presets]")
{
    CHECK(printer_switch_color_sources({"#111111", "#222222"}, {"#111111", "#222222"}, {}) == Strings{"unchanged", "unchanged"});
    CHECK(printer_switch_color_sources({"#AAAAAA"}, {"#aaaaaa"}, {"#AAAAAA"}) == Strings{"unchanged"});
}

TEST_CASE("A colour from nowhere the switch explains is other", "[orcamcp][presets]")
{
    CHECK(printer_switch_color_sources({"#111111"}, {"#111111", "#123456"}, {}) == Strings{"unchanged", "other"});
}

TEST_CASE("summarize_color_sources names the one source every changed slot shares", "[orcamcp][presets]")
{
    CHECK(summarize_color_sources({"unchanged", "unchanged"}) == "unchanged");
    CHECK(summarize_color_sources({}) == "unchanged");
    CHECK(summarize_color_sources({"remembered", "unchanged"}) == "remembered");
    CHECK(summarize_color_sources({"default", "default"}) == "default");
    CHECK(summarize_color_sources({"remembered", "default"}) == "mixed");
    CHECK(summarize_color_sources({"other"}) == "other");
}
