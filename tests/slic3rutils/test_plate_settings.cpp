#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateSettings.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "plate_list_fixtures.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

// A plate's own settings as set_plate_settings reads and get_scene_info reports them, checked as the
// plate settings dialog offers them (Plater::open_platesettings_dialog, PlateSettingsDialog.cpp), and
// the global bed type apply_config sets as the sidebar's list offers it.

using namespace Slic3r::GUI::OrcaMCP;
using Slic3r::BedType;
using Slic3r::PrintSequence;
using json = nlohmann::json;

namespace {

PlateSettingsOffer bambu_offer(int filaments = 3)
{
    PlateSettingsOffer offer;
    offer.filament_count = filaments;
    offer.plate_bed_type = true;
    offer.bed_types      = {Slic3r::btPC, Slic3r::btEP, Slic3r::btPEI, Slic3r::btPTE};
    return offer;
}

std::string refusal_of(const json& params, const PlateSettingsOffer& offer = bambu_offer())
{
    std::string error;
    const auto  request = read_plate_settings(params, offer, error);
    CHECK_FALSE(request);
    return error;
}

PlateSettingsRequest request_of(const json& params, const PlateSettingsOffer& offer = bambu_offer())
{
    std::string error;
    const auto  request = read_plate_settings(params, offer, error);
    INFO(error);
    REQUIRE(request);
    return *request;
}

struct OnePlate
{
    Slic3r::Model                                     model;
    std::unique_ptr<Slic3r::GUI::PartPlateList>       plates = plate_list_fixtures::plate_list_for(model, 1);
    Slic3r::GUI::PartPlate&                           plate  = *plates->get_plate(0);
};

} // namespace

TEST_CASE("A plate with no settings of its own follows the global ones, and sending them back changes nothing", "[PlateSettings][orcamcp]")
{
    OnePlate        one;
    const auto      settings = plate_settings_of(one.plate);
    const json      reported = plate_settings_json(settings);
    CHECK(reported["bed_type"] == "global");
    CHECK(reported["print_sequence"] == "global");
    CHECK(reported["first_layer_filament_order"] == "auto");
    CHECK(reported["other_layers_filament_order"] == "auto");
    CHECK(reported["spiral_vase"] == "global");
    CHECK(reported["locked"] == false);
    CHECK(plate_settings_changes(settings, request_of(reported)).empty());
}

TEST_CASE("A plate's own settings are read as the plate holds them, and round-trip", "[PlateSettings][orcamcp]")
{
    OnePlate one;
    one.plate.set_plate_name("Left");
    one.plate.lock(true);
    one.plate.config()->set_key_value("curr_bed_type", new Slic3r::ConfigOptionEnum<BedType>(Slic3r::btPTE));
    one.plate.config()->set_key_value("print_sequence", new Slic3r::ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
    one.plate.config()->set_key_value("spiral_mode", new Slic3r::ConfigOptionBool(false));
    one.plate.set_first_layer_print_sequence({2, 1, 3});
    one.plate.set_other_layers_print_sequence({{{2, 10}, {3, 1, 2}}, {{11, k_last_layer}, {1, 2, 3}}});

    const auto settings = plate_settings_of(one.plate);
    const json reported = plate_settings_json(settings);
    CHECK(reported["name"] == "Left");
    CHECK(reported["locked"] == true);
    CHECK(reported["bed_type"] == "Textured PEI Plate");
    CHECK(reported["print_sequence"] == "by object");
    CHECK(reported["spiral_vase"] == "off");
    CHECK(reported["first_layer_filament_order"] == json{2, 1, 3});
    CHECK(reported["other_layers_filament_order"][0] == json{{"from_layer", 2}, {"to_layer", 10}, {"order", {3, 1, 2}}});
    CHECK(reported["other_layers_filament_order"][1]["to_layer"].is_null());
    CHECK(plate_settings_changes(settings, request_of(reported)).empty());
}

TEST_CASE("Only what a call gives is a change, and a name or lock alone does not reach the slice", "[PlateSettings][orcamcp]")
{
    PlateSettings now;
    const auto    renamed = plate_settings_changes(now, request_of({{"name", "A"}, {"locked", true}}));
    CHECK(renamed == std::vector<std::string>{"name", "locked"});
    CHECK_FALSE(changes_slicing(renamed));
    const auto sequenced = plate_settings_changes(now, request_of({{"print_sequence", "by object"}, {"bed_type", "global"}}));
    CHECK(sequenced == std::vector<std::string>{"print_sequence"});
    CHECK(changes_slicing(sequenced));
}

TEST_CASE("A bed type must be one the printer offers", "[PlateSettings][orcamcp]")
{
    CHECK(request_of({{"bed_type", "Engineering Plate"}}).bed_type == Slic3r::btEP);
    CHECK(request_of({{"bed_type", "global"}}).bed_type == Slic3r::btDefault);
    const std::string refusal = refusal_of({{"bed_type", "Supertack Plate"}});
    CHECK(refusal.find("not one this printer offers") != std::string::npos);
    CHECK(refusal.find("\"Textured PEI Plate\"") != std::string::npos);
    CHECK(refusal_of({{"bed_type", "Glass"}}).find("not one this printer offers") != std::string::npos);
}

TEST_CASE("On a printer that is not Bambu Lab a plate follows the global bed type", "[PlateSettings][orcamcp]")
{
    PlateSettingsOffer offer = bambu_offer();
    offer.plate_bed_type     = false;
    CHECK(refusal_of({{"bed_type", "Cool Plate"}}, offer).find("curr_bed_type") != std::string::npos);
    CHECK(request_of({{"bed_type", "global"}}, offer).bed_type == Slic3r::btDefault);
}

TEST_CASE("A filament order lists every filament once, or is auto", "[PlateSettings][orcamcp]")
{
    CHECK(request_of({{"first_layer_filament_order", {3, 1, 2}}}).first_layer_order == std::vector<int>{3, 1, 2});
    CHECK(request_of({{"first_layer_filament_order", "auto"}}).first_layer_order == std::vector<int>{});
    const json bad = GENERATE(json{1, 2}, json{1, 2, 2}, json{1, 2, 4}, json{1, 2, 3, 4}, json("reverse"), json{1, "two", 3});
    CHECK(refusal_of({{"first_layer_filament_order", bad}}).find("each of filaments 1 to 3 exactly once") != std::string::npos);
}

TEST_CASE("A custom filament order is refused while the project has mixed filaments", "[PlateSettings][orcamcp]")
{
    PlateSettingsOffer offer = bambu_offer();
    offer.mixed_filaments    = true;
    CHECK(refusal_of({{"first_layer_filament_order", {2, 1, 3}}}, offer).find("mixed filaments") != std::string::npos);
    CHECK(refusal_of({{"other_layers_filament_order", {{{"from_layer", 2}, {"order", {2, 1, 3}}}}}}, offer).find("mixed") !=
          std::string::npos);
    CHECK(request_of({{"first_layer_filament_order", "auto"}}, offer).first_layer_order->empty());
}

TEST_CASE("The other layers' ranges start at layer 2, end at or after their start, and are kept in layer order", "[PlateSettings][orcamcp]")
{
    const auto ranges = request_of({{"other_layers_filament_order",
                                     {{{"from_layer", 20}, {"to_layer", nullptr}, {"order", {1, 2, 3}}},
                                      {{"from_layer", 2}, {"to_layer", 19}, {"order", {3, 2, 1}}}}}})
                            .other_layers_order;
    REQUIRE(ranges->size() == 2);
    CHECK((*ranges)[0].first == std::pair<int, int>{2, 19});
    CHECK((*ranges)[1].first == std::pair<int, int>{20, k_last_layer});
    // to_layer left out: to the last layer too.
    CHECK(request_of({{"other_layers_filament_order", {{{"from_layer", 5}, {"order", {1, 2, 3}}}}}})
              .other_layers_order->front()
              .first.second == k_last_layer);
    CHECK(refusal_of({{"other_layers_filament_order", {{{"from_layer", 1}, {"order", {1, 2, 3}}}}}}).find("2 or more") != std::string::npos);
    CHECK(refusal_of({{"other_layers_filament_order", {{{"from_layer", 9}, {"to_layer", 4}, {"order", {1, 2, 3}}}}}})
              .find("from_layer or more") != std::string::npos);
    CHECK(refusal_of({{"other_layers_filament_order", json::array()}}).find("\"auto\" or a list") != std::string::npos);
}

TEST_CASE("Overlapping ranges of the other layers are refused", "[PlateSettings][orcamcp]")
{
    const std::string refusal = refusal_of({{"other_layers_filament_order",
                                             {{{"from_layer", 2}, {"to_layer", 10}, {"order", {1, 2, 3}}},
                                              {{"from_layer", 10}, {"order", {3, 2, 1}}}}}});
    CHECK(refusal.find("2-10 and 10-the last layer overlap") != std::string::npos);
}

TEST_CASE("A plate name is at most 250 characters, counted as characters", "[PlateSettings][orcamcp]")
{
    CHECK(request_of({{"name", std::string(250, 'a')}}).name->size() == 250);
    CHECK(refusal_of({{"name", std::string(251, 'a')}}).find("at most 250") != std::string::npos);
    std::string accented;
    for (int i = 0; i < 250; ++i)
        accented += "\xC3\xA9"; // e acute: two bytes, one character
    CHECK(request_of({{"name", accented}}).name);
}

TEST_CASE("An argument of the wrong kind or an unknown choice is refused", "[PlateSettings][orcamcp]")
{
    CHECK(refusal_of({{"locked", "yes"}}).find("locked must be true or false") != std::string::npos);
    CHECK(refusal_of({{"print_sequence", "by plate"}}).find("\"by layer\"") != std::string::npos);
    CHECK(refusal_of({{"spiral_vase", true}}).find("spiral_vase must be text") != std::string::npos);
    CHECK(refusal_of({{"name", nullptr}}).find("name must be text") != std::string::npos);
}

TEST_CASE("What applies on a plate is its own setting, else the global one", "[PlateSettings][orcamcp]")
{
    PlateSettings settings;
    json          effective = plate_effective_json(settings, Slic3r::btPEI, PrintSequence::ByLayer, true);
    CHECK(effective == json{{"bed_type", "High Temp Plate"}, {"print_sequence", "by layer"}, {"spiral_vase", true}});
    settings.bed_type       = Slic3r::btPC;
    settings.print_sequence = PrintSequence::ByObject;
    settings.spiral_vase    = SpiralVase::off;
    effective               = plate_effective_json(settings, Slic3r::btPEI, PrintSequence::ByLayer, true);
    CHECK(effective == json{{"bed_type", "Cool Plate"}, {"print_sequence", "by object"}, {"spiral_vase", false}});
}

TEST_CASE("Bed types are spelt as the config spells them", "[PlateSettings][orcamcp]")
{
    for (int bed = Slic3r::btPC; bed < Slic3r::btCount; ++bed) {
        const std::string value = bed_type_value(BedType(bed));
        CHECK(bed_type_from_value(value) == BedType(bed));
    }
    CHECK(bed_type_value(Slic3r::btDefault) == "global");
    CHECK_FALSE(bed_type_from_value("global"));
    CHECK_FALSE(bed_type_from_value("Default Plate"));
}

TEST_CASE("The global bed type must be one the sidebar offers, and a printer with one bed type keeps it", "[PlateSettings][orcamcp]")
{
    const std::vector<BedType> offered = {Slic3r::btPC, Slic3r::btPEI};
    BedType                    parsed  = Slic3r::btDefault;
    CHECK_FALSE(global_bed_type_refusal("High Temp Plate", offered, true, Slic3r::btPC, parsed));
    CHECK(parsed == Slic3r::btPEI);
    CHECK(global_bed_type_refusal("Engineering Plate", offered, true, Slic3r::btPC, parsed)->find("not one this printer offers") !=
          std::string::npos);
    CHECK(global_bed_type_refusal("Glass", offered, true, Slic3r::btPC, parsed)->find("names no bed type") != std::string::npos);
    CHECK(global_bed_type_refusal("High Temp Plate", offered, false, Slic3r::btPC, parsed)->find("one bed type") != std::string::npos);
    // Its own bed type is no change, so it is not refused.
    CHECK_FALSE(global_bed_type_refusal("Cool Plate", offered, false, Slic3r::btPC, parsed));
}

TEST_CASE("The vase settings an object carries are read by value", "[PlateSettings][orcamcp]")
{
    Slic3r::DynamicPrintConfig object;
    CHECK(vase_settings_carried(object).empty());
    object.set_key_value("wall_loops", new Slic3r::ConfigOptionInt(1));
    object.set_key_value("sparse_infill_density", new Slic3r::ConfigOptionPercent(15));
    object.set_key_value("enable_support", new Slic3r::ConfigOptionBool(false));
    CHECK(vase_settings_carried(object) == std::vector<std::string>{"enable_support", "wall_loops"});
    CHECK(vase_object_settings().keys().size() == 8);
}

TEST_CASE("Changed keys are those whose value differs or that one config lacks", "[PlateSettings][orcamcp]")
{
    Slic3r::DynamicPrintConfig before, after;
    before.set_key_value("wall_loops", new Slic3r::ConfigOptionInt(3));
    before.set_key_value("brim_width", new Slic3r::ConfigOptionFloat(5));
    after.set_key_value("wall_loops", new Slic3r::ConfigOptionInt(1));
    after.set_key_value("brim_width", new Slic3r::ConfigOptionFloat(5));
    after.set_key_value("top_shell_layers", new Slic3r::ConfigOptionInt(0));
    CHECK(changed_config_keys(before, after) == std::vector<std::string>{"top_shell_layers", "wall_loops"});
}
