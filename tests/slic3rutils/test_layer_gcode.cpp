#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <optional>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPLayerGcode.hpp"

// G-code at a layer as the Preview's layer slider adds, edits and deletes it (IMSlider's menus,
// TickCodeInfo): add_layer_gcode and delete_layer_gcode edit the plate's CustomGCode::Info by the
// slider's rules, which these hold them to, without the app.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// Ten layers 0.2 mm apart: layer 1 at 0.2 mm, layer 10 at 2.0 mm.
const std::vector<double> k_layers = {0.2, 0.4, 0.6, 0.8, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0};

// A project of `slots` physical filaments whose plate prints with `plate_filaments`.
LayerGcodeRules rules(std::size_t slots = 2, std::vector<int> plate_filaments = {1})
{
    LayerGcodeRules rules;
    rules.slots.is_mixed        = std::vector<bool>(slots, false);
    rules.slots.slot_presets    = std::vector<std::string>(slots, "PLA");
    rules.filament_colors       = {"#FF0000", "#00FF00", "#0000FF", "#FFFFFF"};
    rules.filament_colors.resize(slots);
    rules.plate_filaments       = std::move(plate_filaments);
    rules.template_gcode_empty  = false;
    return rules;
}

LayerGcodeRequest pause() { return {LayerGcodeKind::pause, 0, {}}; }
LayerGcodeRequest change_to(int filament) { return {LayerGcodeKind::filament_change, filament, {}}; }
LayerGcodeRequest custom(std::string gcode) { return {LayerGcodeKind::custom, 0, std::move(gcode)}; }

bool contains(const std::optional<std::string>& text, const std::string& part) { return text && text->find(part) != std::string::npos; }

} // namespace

TEST_CASE("a pause goes at the start of the layer, at its height, with the plate's filament", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, /*layer=*/4, pause(), rules(2, {2}), change));
    CHECK(change.changed);
    REQUIRE(info.gcodes.size() == 1);
    CHECK(info.gcodes[0].type == CustomGCode::PausePrint);
    CHECK_THAT(info.gcodes[0].print_z, Catch::Matchers::WithinAbs(1.0, 1e-9));
    // IMSlider's max(1, m_only_extruder): the one filament the plate prints with.
    CHECK(info.gcodes[0].extruder == 2);
    CHECK(info.gcodes[0].color.empty());
    CHECK(info.mode == CustomGCode::MultiAsSingle);
}

TEST_CASE("on a project of one filament the slider's mode is single extruder, and its filament 1", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 0, pause(), rules(1, {1}), change));
    CHECK(info.mode == CustomGCode::SingleExtruder);
    CHECK(info.gcodes[0].extruder == 1);
}

TEST_CASE("a filament change records the slot and its colour", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 2, change_to(2), rules(), change));
    CHECK(info.gcodes[0].type == CustomGCode::ToolChange);
    CHECK(info.gcodes[0].extruder == 2);
    CHECK(info.gcodes[0].color == "#00FF00");
}

TEST_CASE("the slider offers no filament change where it greys the menu", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    CHECK(contains(add_layer_gcode(info, k_layers, 2, change_to(1), rules(1), change), "one filament"));
    CHECK(contains(add_layer_gcode(info, k_layers, 2, change_to(2), rules(2, {1, 2}), change), "prints with several"));
    LayerGcodeRules vase = rules();
    vase.spiral_vase     = true;
    CHECK(contains(add_layer_gcode(info, k_layers, 2, change_to(2), vase, change), "spiral vase"));
    // A slot the project lacks, as set_object_config refuses one.
    CHECK(contains(add_layer_gcode(info, k_layers, 2, change_to(3), rules(), change), "filament 3 names no filament slot"));
    CHECK(contains(add_layer_gcode(info, k_layers, 2, change_to(0), rules(), change), "numbered from 1"));
    CHECK(info.gcodes.empty());
}

TEST_CASE("nothing goes at a layer of a plate printed by object", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    LayerGcodeRules   by_object = rules();
    by_object.by_object         = true;
    CHECK(contains(add_layer_gcode(info, k_layers, 2, pause(), by_object, change), "print_sequence \"by layer\""));
    CHECK(info.gcodes.empty());
}

TEST_CASE("custom G-code needs text of at most the slider's 1023 characters", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    CHECK(contains(add_layer_gcode(info, k_layers, 1, custom(""), rules(), change), "must not be empty"));
    CHECK(contains(add_layer_gcode(info, k_layers, 1, custom(std::string(1024, 'M')), rules(), change), "at most 1023"));
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 1, custom("M117 hello"), rules(), change));
    CHECK(info.gcodes[0].extra == "M117 hello");
    CHECK(info.gcodes[0].type == CustomGCode::Custom);
}

TEST_CASE("the template is offered only when the printer has one", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    LayerGcodeRules   none = rules();
    none.template_gcode_empty = true;
    CHECK(contains(add_layer_gcode(info, k_layers, 1, {LayerGcodeKind::template_gcode, 0, {}}, none, change), "no template G-code"));
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 1, {LayerGcodeKind::template_gcode, 0, {}}, rules(), change));
    CHECK(info.gcodes[0].type == CustomGCode::Template);
}

TEST_CASE("a layer that has G-code is edited only where the slider's Edit would", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 3, custom("M117 a"), rules(), change));

    // Custom G-code's text is edited in place, and the old one reported.
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 3, custom("M117 b"), rules(), change));
    CHECK(change.changed);
    REQUIRE(change.replaced);
    CHECK(change.replaced->extra == "M117 a");
    REQUIRE(info.gcodes.size() == 1);
    CHECK(info.gcodes[0].extra == "M117 b");

    // A pause there: the slider offers only Delete on a custom G-code's layer, besides Edit.
    CHECK(contains(add_layer_gcode(info, k_layers, 3, pause(), rules(), change), "already has a custom"));
    CHECK(info.gcodes.size() == 1);
}

TEST_CASE("a filament change is switched to another filament, not added twice", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 5, change_to(2), rules(3), change));
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 5, change_to(3), rules(3), change));
    REQUIRE(info.gcodes.size() == 1);
    CHECK(info.gcodes[0].extruder == 3);
    CHECK(info.gcodes[0].color == "#0000FF");
    CHECK(change.replaced->extruder == 2);
}

TEST_CASE("asking for what a layer already has changes nothing", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 7, pause(), rules(), change));
    const CustomGCode::Info before = info;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 7, pause(), rules(), change));
    CHECK_FALSE(change.changed);
    CHECK(info == before);
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 2, custom("G4 S1"), rules(), change));
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 2, custom("G4 S1"), rules(), change));
    CHECK_FALSE(change.changed);
}

TEST_CASE("items stay in height order, and each is found at its layer", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 8, pause(), rules(), change));
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 1, custom("M117 x"), rules(), change));
    REQUIRE(info.gcodes.size() == 2);
    CHECK(info.gcodes[0].print_z < info.gcodes[1].print_z);
    // Within the slider's tolerance: a height the G-code rounds still lands on its layer.
    CHECK(layer_of(k_layers, 0.4005) == std::optional<std::size_t>(1));
    CHECK(layer_of(k_layers, 5.0) == std::nullopt);
}

TEST_CASE("the slider's Delete removes a layer's item, and a layer with none says so", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 4, pause(), rules(), change));
    CHECK(contains(delete_layer_gcode(info, k_layers, 5, change), "layer 6 has no G-code"));
    REQUIRE_FALSE(delete_layer_gcode(info, k_layers, 4, change));
    CHECK(change.changed);
    CHECK(change.item.type == CustomGCode::PausePrint);
    CHECK(info.gcodes.empty());
    CHECK(contains(delete_layer_gcode(info, k_layers, 10, change), "does not exist"));
}

TEST_CASE("an item is described by layer number, height, type and what it carries", "[LayerGcode][orcamcp]")
{
    const CustomGCode::Item change{0.6, CustomGCode::ToolChange, 2, "#00FF00", ""};
    const nlohmann::json    json = layer_gcode_json(change, &k_layers);
    CHECK(json.at("layer") == 3);
    CHECK_THAT(json.at("z_mm").get<double>(), Catch::Matchers::WithinAbs(0.6, 1e-9));
    CHECK(json.at("type") == "filament_change");
    CHECK(json.at("filament") == 2);
    CHECK(layer_gcode_json(change, nullptr).at("layer").is_null());
    const CustomGCode::Item text{0.2, CustomGCode::Custom, 1, "", "M117 hi"};
    CHECK(layer_gcode_json(text, &k_layers).at("gcode") == "M117 hi");
    CHECK_FALSE(layer_gcode_json(text, &k_layers).contains("filament"));
}

TEST_CASE("every kind has the name the tools take and report", "[LayerGcode][orcamcp]")
{
    for (const char* name : {"pause", "filament_change", "custom", "template"}) {
        const auto kind = layer_gcode_kind_named(name);
        REQUIRE(kind);
    }
    CHECK_FALSE(layer_gcode_kind_named("color_change"));
    CHECK(layer_gcode_type_name(CustomGCode::PausePrint) == "pause");
    CHECK(layer_gcode_type_name(CustomGCode::Template) == "template");
    CHECK(layer_gcode_type_name(CustomGCode::ColorChange) == "color_change");
}
