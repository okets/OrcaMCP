#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp"

#include <string>
#include <vector>

// Auto material mapping: the pure half. Nothing here touches a printer, a preset bundle or wx -
// which slot feeds which tool is decided from plain data, and that is the whole reason it lives in
// its own function rather than inside auto_material_mappings' JSON plumbing.

using Slic3r::FlashforgeApi::MaterialSlot;
using Slic3r::GUI::OrcaMCP::auto_map_tools_to_slots;
using Slic3r::GUI::OrcaMCP::material_color_delta_e;
using Slic3r::GUI::OrcaMCP::ProjectFilamentTool;

namespace {

MaterialSlot loaded(int id, const char* material, const char* color)
{
    MaterialSlot slot;
    slot.slot_id        = id;
    slot.has_filament   = true;
    slot.material_name  = material;
    slot.material_color = color;
    return slot;
}

MaterialSlot empty_slot(int id)
{
    MaterialSlot slot;
    slot.slot_id      = id;
    slot.has_filament = false;
    return slot;
}

// The station that produced the bug: four PLA slots, tool 1 blue, mapped to slot 1 magenta.
std::vector<MaterialSlot> four_pla_slots()
{
    return {loaded(1, "PLA", "#FF00FF"),  // magenta
            loaded(2, "PLA", "#0000FF"),  // blue
            loaded(3, "PLA", "#FFFFFF"),  // white
            loaded(4, "PLA", "#FF0000")}; // red
}

} // namespace

TEST_CASE("auto_map_tools_to_slots picks the nearest colour in the family", "[orcamcp][material-mapping]")
{
    const std::vector<ProjectFilamentTool> tools = {{0, "PLA", "#FF0000"},  // red   -> slot 4
                                                    {1, "PLA", "#0000FF"}}; // blue  -> slot 2

    const auto mapped = auto_map_tools_to_slots(tools, four_pla_slots());
    REQUIRE(mapped.size() == 2);
    CHECK(mapped[0].tool_id == 0);
    CHECK(mapped[0].slot_id == 4);
    CHECK(mapped[1].tool_id == 1);
    CHECK(mapped[1].slot_id == 2);

    // Exact colour matches, so the reported distance is zero rather than merely small.
    REQUIRE(mapped[0].color_delta_e.has_value());
    REQUIRE(mapped[1].color_delta_e.has_value());
    CHECK_THAT(*mapped[0].color_delta_e, Catch::Matchers::WithinAbs(0.0, 1e-9));
    CHECK_THAT(*mapped[1].color_delta_e, Catch::Matchers::WithinAbs(0.0, 1e-9));
}

TEST_CASE("auto_map_tools_to_slots never hands two tools the same slot", "[orcamcp][material-mapping]")
{
    // Both tools want slot 2; the second must take its next best rather than double-book.
    const std::vector<ProjectFilamentTool> tools = {{0, "PLA", "#0000FF"}, {1, "PLA", "#0000EE"}};

    const auto mapped = auto_map_tools_to_slots(tools, four_pla_slots());
    REQUIRE(mapped.size() == 2);
    CHECK(mapped[0].slot_id == 2);
    CHECK(mapped[1].slot_id != 2);
}

TEST_CASE("auto_map_tools_to_slots breaks a colour tie on the lower slot id", "[orcamcp][material-mapping]")
{
    // Two slots hold the identical colour, and the station lists the higher id first: the result
    // must not depend on that order.
    const std::vector<MaterialSlot> slots = {loaded(4, "PLA", "#00FF00"), loaded(2, "PLA", "#00FF00"),
                                             loaded(3, "PLA", "#FF0000")};

    const auto mapped = auto_map_tools_to_slots({{0, "PLA", "#00FF00"}}, slots);
    REQUIRE(mapped.size() == 1);
    CHECK(mapped[0].slot_id == 2);
}

TEST_CASE("auto_map_tools_to_slots falls back to first-free when a colour is unknown", "[orcamcp][material-mapping]")
{
    SECTION("the project filament has no colour")
    {
        const auto mapped = auto_map_tools_to_slots({{0, "PLA", ""}}, four_pla_slots());
        REQUIRE(mapped.size() == 1);
        CHECK(mapped[0].slot_id == 1); // first free slot of the family, exactly as before item P
        CHECK_FALSE(mapped[0].color_delta_e.has_value());
    }

    SECTION("the project filament's colour is not a colour")
    {
        // "#GGGGGG" is the case upstream's parser reads as black; is_hex_color rejects it, so the
        // pair is reported as unscored rather than scored against black.
        const auto mapped = auto_map_tools_to_slots({{0, "PLA", "#GGGGGG"}}, four_pla_slots());
        REQUIRE(mapped.size() == 1);
        CHECK(mapped[0].slot_id == 1);
        CHECK_FALSE(mapped[0].color_delta_e.has_value());
    }

    SECTION("no slot reports a colour")
    {
        const std::vector<MaterialSlot> slots = {loaded(1, "PLA", ""), loaded(2, "PLA", "")};
        const auto                      mapped = auto_map_tools_to_slots({{0, "PLA", "#0000FF"}}, slots);
        REQUIRE(mapped.size() == 1);
        CHECK(mapped[0].slot_id == 1);
        CHECK_FALSE(mapped[0].color_delta_e.has_value());
    }

    SECTION("a scorable candidate beats an unscorable one whatever the order")
    {
        const std::vector<MaterialSlot> slots  = {loaded(1, "PLA", ""), loaded(2, "PLA", "#FFFFFF")};
        const auto                      mapped = auto_map_tools_to_slots({{0, "PLA", "#0000FF"}}, slots);
        REQUIRE(mapped.size() == 1);
        CHECK(mapped[0].slot_id == 2);
        CHECK(mapped[0].color_delta_e.has_value());
    }
}

TEST_CASE("auto_map_tools_to_slots only considers loaded slots of the same family", "[orcamcp][material-mapping]")
{
    const std::vector<MaterialSlot> slots = {empty_slot(1),                     // free but not loaded
                                             loaded(2, "PETG", "#0000FF"),      // wrong family
                                             loaded(3, "PLA Basic", "#FF0000"), // PLA Basic normalizes to PLA
                                             loaded(4, "PLA", "#0000FF")};

    SECTION("a PLA tool skips the empty and the PETG slot")
    {
        const auto mapped = auto_map_tools_to_slots({{0, "PLA", "#0000FF"}}, slots);
        REQUIRE(mapped.size() == 1);
        CHECK(mapped[0].slot_id == 4);
    }

    SECTION("a tool with no candidate is left out entirely")
    {
        const auto mapped = auto_map_tools_to_slots({{0, "ABS", "#0000FF"}}, slots);
        CHECK(mapped.empty());
    }

    SECTION("a tool with no material type is left out entirely")
    {
        const auto mapped = auto_map_tools_to_slots({{0, "", "#0000FF"}}, slots);
        CHECK(mapped.empty());
    }
}

TEST_CASE("material_color_delta_e reports nothing rather than a wrong number", "[orcamcp][material-mapping]")
{
    CHECK_FALSE(material_color_delta_e("", "#FF0000").has_value());
    CHECK_FALSE(material_color_delta_e("#FF0000", "").has_value());
    CHECK_FALSE(material_color_delta_e("#GGGGGG", "#FF0000").has_value());       // decodes as black upstream
    CHECK_FALSE(material_color_delta_e("#FF0000 (red)", "#FF0000").has_value()); // trailing garbage
    CHECK_FALSE(material_color_delta_e("FF0000", "#FF0000").has_value());        // no '#'

    REQUIRE(material_color_delta_e("#FF0000", "#FF0000").has_value());
    CHECK_THAT(*material_color_delta_e("#FF0000", "#FF0000"), Catch::Matchers::WithinAbs(0.0, 1e-9));

    // The printer may report an alpha byte; the RGB half of it is still a usable colour.
    REQUIRE(material_color_delta_e("#FF0000", "#FF0000FF").has_value());
    CHECK_THAT(*material_color_delta_e("#FF0000", "#FF0000FF"), Catch::Matchers::WithinAbs(0.0, 1e-9));

    // Perceptually distinct colours are far apart; a "just noticeable difference" is ~2.3.
    REQUIRE(material_color_delta_e("#0000FF", "#FF00FF").has_value());
    CHECK(*material_color_delta_e("#0000FF", "#FF00FF") > 10.0);
}
