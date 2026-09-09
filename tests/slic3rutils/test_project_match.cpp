#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPProjectMatch.hpp"

#include <algorithm>
#include <string>
#include <vector>

// "Match project to printer": the pure half. Nothing here touches a preset bundle, a printer or wx -
// the ranking, the per-slot plan and the summary line are decided from plain data, which is the whole
// reason they live in their own functions.

using namespace Slic3r::GUI::OrcaMCP;

namespace {

// The Creator 5 Pro's filament collection, trimmed to the entries that decide something. The real
// one has ~318 compatible presets; the ordering rules only ever look at these fields.
std::vector<MatchCandidate> c5p_candidates()
{
    return {
        {"Flashforge PETG Pro @FF C5P", "PETG", "Flashforge", true},
        {"Flashforge PETG Transparent @FF C5P", "PETG", "Flashforge", true},
        {"Flashforge HS PETG @FF C5P", "PETG", "Flashforge", true},
        {"Flashforge PETG-CF @FF C5P", "PETG-CF", "Flashforge", true},
        {"Flashforge PLA Basic @FF C5P", "PLA", "Flashforge", true},
        {"Flashforge PLA Pro @FF C5P", "PLA", "Flashforge", true},
        {"Flashforge HS PLA @FF C5P", "PLA", "Flashforge", true},
        {"Flashforge ABS Basic @FF C5P", "ABS", "Flashforge", true},
        {"Flashforge ASA Basic @FF C5P", "ASA", "Flashforge", true},
        {"Generic PLA @FF C5P", "PLA", "Generic", true},
        {"Generic PETG @System", "PETG", "Generic", false},
        {"Generic PLA @System", "PLA", "Generic", false},
        {"Bambu PETG Basic @System", "PETG", "Bambu Lab", false},
        {"AliZ PETG @System", "PETG", "Aliz", false},
    };
}

std::vector<MatchCandidate> without(std::vector<MatchCandidate> candidates, const std::string& fragment)
{
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&fragment](const MatchCandidate& c) {
                                        return c.name.find(fragment) != std::string::npos;
                                    }),
                     candidates.end());
    return candidates;
}

const SlotPlan* slot(const std::vector<SlotPlan>& plans, int id)
{
    const auto found = std::find_if(plans.begin(), plans.end(), [id](const SlotPlan& p) { return p.slot == id; });
    return found == plans.end() ? nullptr : &*found;
}

// Station: slot 1 loaded with PETG in the printer's own brown, slots 2-4 empty. The machine the
// feature was written against, on the evening it was written.
std::vector<StationSlot> station_petg_then_empty()
{
    return {{1, true, "PETG", "#B17C38"}, {2, false, "", ""}, {3, false, "", ""}, {4, false, "", ""}};
}

// Project: four slots on a stale magenta PLA, which is exactly the trap - send_to_printer refuses on
// the material mismatch and the plate preview paints the part the wrong colour.
std::vector<ProjectSlot> project_all_pla()
{
    return {{1, "Panchroma PLA Translucent @System", "PLA", "#F435F6", false},
            {2, "Panchroma PLA Translucent @System", "PLA", "#4CAAF8", false},
            {3, "Panchroma PLA Translucent @System", "PLA", "#FFF245", false},
            {4, "Panchroma PLA Translucent @System", "PLA", "#BEBEBE", false}};
}

} // namespace

TEST_CASE("A slot takes the vendor profile built for this printer model", "[ProjectMatch]")
{
    const auto candidates = c5p_candidates();

    SECTION("a model-specific vendor profile wins, and the plainest of those")
    {
        // "PETG Pro" and "PETG Transparent" both lead with the material; "HS PETG" does not, so it
        // loses before the alphabetical tie-break ever runs.
        CHECK(choose_filament_preset("PETG", candidates, "Flashforge") == "Flashforge PETG Pro @FF C5P");
        CHECK(choose_filament_preset("PLA", candidates, "Flashforge") == "Flashforge PLA Basic @FF C5P");
    }

    SECTION("a same-vendor profile that is not model-specific loses to one that is")
    {
        auto with_generic_machine = candidates;
        with_generic_machine.push_back({"Flashforge PETG Basic @Flashforge", "PETG", "Flashforge", false});
        CHECK(choose_filament_preset("PETG", with_generic_machine, "Flashforge") == "Flashforge PETG Pro @FF C5P");
    }

    SECTION("with no model-specific profile, any same-vendor one beats the generic")
    {
        auto no_model_specific = without(candidates, "@FF C5P");
        no_model_specific.push_back({"Flashforge PETG Basic @Flashforge", "PETG", "Flashforge", false});
        CHECK(choose_filament_preset("PETG", no_model_specific, "Flashforge") == "Flashforge PETG Basic @Flashforge");
    }

    SECTION("an exact material beats one that merely normalizes to the same family")
    {
        // ASA and ABS share a family, so both are viable for an ABS spool; only one of them is ABS.
        CHECK(choose_filament_preset("ABS", candidates, "Flashforge") == "Flashforge ABS Basic @FF C5P");
        CHECK(choose_filament_preset("ASA", candidates, "Flashforge") == "Flashforge ASA Basic @FF C5P");
    }

    SECTION("a CF grade is its own family, never the plain one")
    {
        CHECK(choose_filament_preset("PETG-CF", candidates, "Flashforge") == "Flashforge PETG-CF @FF C5P");
        // ... and the plain material never picks up the CF profile.
        CHECK(choose_filament_preset("PETG", without(candidates, "PETG Pro"), "Flashforge") ==
              "Flashforge PETG Transparent @FF C5P");
    }
}

TEST_CASE("A material with no vendor profile falls back to a generic one", "[ProjectMatch]")
{
    SECTION("no vendor profile of that material falls to Generic <TYPE> @System")
    {
        const auto candidates = without(c5p_candidates(), "Flashforge PETG");
        CHECK(choose_filament_preset("PETG", without(candidates, "HS PETG"), "Flashforge") ==
              "Generic PETG @System");
    }

    SECTION("no vendor and no generic still beats leaving the slot on the wrong material")
    {
        std::vector<MatchCandidate> third_party_only = {{"Bambu PETG Basic @System", "PETG", "Bambu Lab", false},
                                                        {"AliZ PETG @System", "PETG", "Aliz", false}};
        CHECK(choose_filament_preset("PETG", third_party_only, "Flashforge") == "AliZ PETG @System");
    }

    SECTION("an unknown printer vendor cannot reach the vendor tiers, so Generic wins")
    {
        CHECK(choose_filament_preset("PETG", c5p_candidates(), "") == "Generic PETG @System");
    }
}

TEST_CASE("A slot nothing can be matched to keeps its preset and says why", "[ProjectMatch]")
{
    SECTION("no compatible preset of that family")
    {
        const std::vector<MatchCandidate> pla_only = {{"Flashforge PLA Basic @FF C5P", "PLA", "Flashforge", true}};
        CHECK(choose_filament_preset("PETG", pla_only, "Flashforge").empty());

        const auto plan = plan_project_match(station_petg_then_empty(), project_all_pla(), pla_only, "Flashforge", {});
        REQUIRE(plan.size() == 1);
        const SlotPlan& one = plan.front();
        CHECK(one.matched == false);
        CHECK(one.disagrees == true);
        CHECK(one.preset_after == "Panchroma PLA Translucent @System");   // left exactly where it was
        CHECK(one.preset_changes == false);
        CHECK(one.reason.find("PETG") != std::string::npos);
        // The colour the printer does report is still worth having.
        CHECK(one.color_changes == true);
        CHECK(one.color_after == "#B17C38");
    }

    SECTION("a loaded slot the printer cannot name keeps its preset and takes the colour")
    {
        std::vector<StationSlot> station = station_petg_then_empty();
        station[0].material              = "";
        const auto plan = plan_project_match(station, project_all_pla(), c5p_candidates(), "Flashforge", {});
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].matched == false);
        CHECK(plan[0].preset_changes == false);
        CHECK(plan[0].color_changes == true);
        CHECK(plan[0].color_after == "#B17C38");
        CHECK(plan[0].reason.find("no material name") != std::string::npos);
    }

    SECTION("a mixed slot is never rewritten by this")
    {
        auto project        = project_all_pla();
        project[0].is_mixed = true;
        const auto plan = plan_project_match(station_petg_then_empty(), project, c5p_candidates(), "Flashforge", {});
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].matched == false);
        CHECK(plan[0].changes() == false);
        CHECK(plan[0].reason.find("set_mixed_filament") != std::string::npos);
    }
}

TEST_CASE("A slot the printer reports as empty is left alone", "[ProjectMatch]")
{
    const auto plan = plan_project_match(station_petg_then_empty(), project_all_pla(), c5p_candidates(), "Flashforge", {});

    SECTION("by default only the loaded slot is planned at all")
    {
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].slot == 1);
        CHECK(slot(plan, 2) == nullptr);
        CHECK(slot(plan, 3) == nullptr);
        CHECK(slot(plan, 4) == nullptr);
    }

    SECTION("naming an empty slot answers about it without touching it")
    {
        const auto asked = plan_project_match(station_petg_then_empty(), project_all_pla(), c5p_candidates(),
                                              "Flashforge", {2, 3});
        REQUIRE(asked.size() == 2);
        for (const SlotPlan& entry : asked) {
            CHECK(entry.changes() == false);
            CHECK(entry.disagrees == false);
            CHECK(entry.matched == true);
            CHECK(entry.preset_after == entry.preset_before);
            CHECK(entry.color_after == entry.color_before);
            CHECK(entry.reason.find("empty") != std::string::npos);
        }
    }

    SECTION("an empty slot whose colour differs is still not recoloured")
    {
        // The station reports nothing for an empty slot, so there is no colour to copy over - and a
        // project colour is not something to clear just because no spool is in the bay.
        const auto asked = plan_project_match(station_petg_then_empty(), project_all_pla(), c5p_candidates(),
                                              "Flashforge", {4});
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].color_after == "#BEBEBE");
        CHECK(asked[0].color_changes == false);
    }
}

TEST_CASE("A loaded slot takes the printer's material and colour", "[ProjectMatch]")
{
    SECTION("both halves of the trap in one entry")
    {
        const auto plan = plan_project_match(station_petg_then_empty(), project_all_pla(), c5p_candidates(),
                                             "Flashforge", {});
        REQUIRE(plan.size() == 1);
        const SlotPlan& one = plan.front();
        CHECK(one.slot == 1);
        CHECK(one.material == "PETG");
        CHECK(one.color == "#B17C38");
        CHECK(one.preset_before == "Panchroma PLA Translucent @System");
        CHECK(one.preset_after == "Flashforge PETG Pro @FF C5P");
        CHECK(one.type_before == "PLA");
        CHECK(one.color_before == "#F435F6");
        CHECK(one.color_after == "#B17C38");
        CHECK(one.preset_changes == true);
        CHECK(one.color_changes == true);
        CHECK(one.changes() == true);
        CHECK(one.disagrees == true);
    }

    SECTION("the right material already: the preset stays, only the colour follows")
    {
        auto project      = project_all_pla();
        project[0].preset = "Flashforge PETG Transparent @FF C5P";
        project[0].type   = "PETG";
        const auto plan   = plan_project_match(station_petg_then_empty(), project, c5p_candidates(), "Flashforge", {});
        REQUIRE(plan.size() == 1);
        // Matching is not homogenizing: a deliberate PETG Transparent is not swapped for PETG Pro.
        CHECK(plan[0].preset_after == "Flashforge PETG Transparent @FF C5P");
        CHECK(plan[0].preset_changes == false);
        CHECK(plan[0].color_changes == true);
        CHECK(plan[0].disagrees == true);
    }

    SECTION("nothing to do at all")
    {
        auto project      = project_all_pla();
        project[0].preset = "Flashforge PETG Pro @FF C5P";
        project[0].type   = "PETG";
        project[0].color  = "#b17c38";   // the same colour, spelled differently
        const auto plan   = plan_project_match(station_petg_then_empty(), project, c5p_candidates(), "Flashforge", {});
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].changes() == false);
        CHECK(plan[0].disagrees == false);
        CHECK(plan[0].matched == true);
        CHECK(plan[0].reason.find("already matches") != std::string::npos);
    }

    SECTION("a station with more slots than the project has")
    {
        std::vector<StationSlot> station = {{1, true, "PETG", "#B17C38"}, {2, true, "PLA", "#4CAAF8"}};
        const std::vector<ProjectSlot> project = {{1, "Generic PLA @System", "PLA", "#F435F6", false}};
        const auto plan = plan_project_match(station, project, c5p_candidates(), "Flashforge", {});
        REQUIRE(plan.size() == 2);
        CHECK(slot(plan, 2)->matched == false);
        CHECK(slot(plan, 2)->reason.find("no filament slot 2") != std::string::npos);
        // Reported, but not something to nag about: a one-filament project on a four-spool machine
        // is ordinary, and no button can give the project another slot.
        CHECK(slot(plan, 2)->disagrees == false);
    }
}

TEST_CASE("A colour is compared however it is spelled", "[ProjectMatch]")
{
    CHECK(normalize_hex_color("#b17c38") == "#B17C38");
    CHECK(normalize_hex_color("B17C38") == "#B17C38");
    CHECK(normalize_hex_color("0xb17c38") == "#B17C38");
    CHECK(normalize_hex_color("#ABC") == "#AABBCC");
    CHECK(normalize_hex_color("#B17C38FF") == "#B17C38");   // an alpha byte is dropped
    CHECK(normalize_hex_color("").empty());
    CHECK(normalize_hex_color("brown").empty());
    CHECK(normalize_hex_color("#12345").empty());
}

TEST_CASE("The summary names what disagrees, and says nothing when nothing does", "[ProjectMatch]")
{
    SECTION("one slot names both sides")
    {
        const auto plan = plan_project_match(station_petg_then_empty(), project_all_pla(), c5p_candidates(),
                                             "Flashforge", {});
        const std::string line = describe_plan_summary(plan);
        CHECK(line.find("Slot 1") != std::string::npos);
        CHECK(line.find("PETG #B17C38") != std::string::npos);
        CHECK(line.find("PLA #F435F6") != std::string::npos);
    }

    SECTION("several slots are counted, not recited")
    {
        const std::vector<StationSlot> station = {{1, true, "PETG", "#B17C38"},
                                                  {2, true, "ABS", "#101010"},
                                                  {3, false, "", ""},
                                                  {4, true, "PLA", "#BEBEBE"}};
        auto                           project = project_all_pla();
        project[3].color                       = "#BEBEBE";   // slot 4 already agrees entirely
        const auto        plan = plan_project_match(station, project, c5p_candidates(), "Flashforge", {});
        const std::string line = describe_plan_summary(plan);
        CHECK(line == "Slots 1 and 2 are loaded with different filament than the project has.");
    }

    SECTION("agreement says nothing")
    {
        auto project      = project_all_pla();
        project[0].preset = "Flashforge PETG Pro @FF C5P";
        project[0].type   = "PETG";
        project[0].color  = "#B17C38";
        CHECK(describe_plan_summary(
                  plan_project_match(station_petg_then_empty(), project, c5p_candidates(), "Flashforge", {}))
                  .empty());
    }
}
