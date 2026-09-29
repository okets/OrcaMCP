#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPLayerGcode.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "plate_list_fixtures.hpp"
#include "fff_print/test_helpers.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

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
    // As the slicer would count them, from the plate's filaments (tests with a feature's filament set their own).
    std::vector<int> distinct = rules.plate_filaments;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    rules.object_filaments      = distinct;
    rules.template_gcode_empty  = false;
    return rules;
}

LayerGcodeRequest a_pause() { return {LayerGcodeKind::pause, 0, {}}; }
LayerGcodeRequest change_to(int filament) { return {LayerGcodeKind::filament_change, filament, {}}; }
LayerGcodeRequest custom(std::string gcode) { return {LayerGcodeKind::custom, 0, std::move(gcode)}; }

bool mentions(const std::optional<std::string>& text, const std::string& part) { return text && text->find(part) != std::string::npos; }

} // namespace

TEST_CASE("a pause goes at the start of the layer, at its height, with the plate's filament", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, /*layer=*/4, a_pause(), rules(2, {2}), change));
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
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 0, a_pause(), rules(1, {1}), change));
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
    CHECK(mentions(add_layer_gcode(info, k_layers, 2, change_to(1), rules(1), change), "one filament"));
    CHECK(mentions(add_layer_gcode(info, k_layers, 2, change_to(2), rules(2, {1, 2}), change), "prints with several"));
    LayerGcodeRules vase = rules();
    vase.spiral_vase     = true;
    CHECK(mentions(add_layer_gcode(info, k_layers, 2, change_to(2), vase, change), "spiral vase"));
    // A slot the project lacks, as set_object_config refuses one.
    CHECK(mentions(add_layer_gcode(info, k_layers, 2, change_to(3), rules(), change), "filament 3 names no filament slot: the project has 2"));
    CHECK(mentions(add_layer_gcode(info, k_layers, 2, change_to(0), rules(), change), "filament 0 names no filament slot"));
    CHECK(info.gcodes.empty());
}

// The slider shows and offers filament changes by the slicer's rule (CustomGCode::tool_changes_off), with the
// filaments the plate's objects print as the slicer counts them: a feature's filament counts though the parts
// print with one, and a vase plate takes none. MCP decides by the same rule.
TEST_CASE("a filament change is offered only where the slicer would take it", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    LayerGcodeRules   walls_on_2 = rules(3, {1});
    walls_on_2.object_filaments  = std::vector<int>{1, 2}; // the parts print with filament 1, their walls with filament 2
    CHECK(mentions(add_layer_gcode(info, k_layers, 2, change_to(3), walls_on_2, change), "prints with several"));
    // The plate's own vase mode, which the tools read from the plate (PartPlate::get_spiral_vase_mode).
    LayerGcodeRules vase = rules(3, {1});
    vase.spiral_vase     = true;
    CHECK(mentions(add_layer_gcode(info, k_layers, 2, change_to(3), vase, change), "spiral vase"));
    CHECK(info.gcodes.empty());
}

// add_layer_gcode's checks that need no layer run before the tools make the plate current, so a refused call does
// not switch plates. The filaments a plate's objects print are known only once it is current: that one waits.
TEST_CASE("a request is refused without its layer, and one that needs the plate's filaments waits for them", "[LayerGcode][orcamcp]")
{
    LayerGcodeRules by_object = rules();
    by_object.by_object       = true;
    CHECK(mentions(layer_gcode_request_refusal(a_pause(), by_object), "by object"));
    LayerGcodeRules vase = rules();
    vase.spiral_vase     = true;
    CHECK(mentions(layer_gcode_request_refusal(change_to(2), vase), "spiral vase"));
    CHECK(mentions(layer_gcode_request_refusal(custom(""), rules()), "must not be empty"));

    LayerGcodeRules not_current = rules(2, {1, 2});
    not_current.object_filaments.reset();
    CHECK_FALSE(layer_gcode_request_refusal(change_to(2), not_current).has_value());
    CHECK(mentions(layer_gcode_request_refusal(change_to(2), rules(2, {1, 2})), "prints with several"));
}

// Whether the slicer takes a filament change goes with it, where the slider hides one it does not: on a vase plate,
// by object, on a plate whose objects print with several filaments, and in another mode (an older project's).
TEST_CASE("a filament change says whether the slicer takes it, and why not", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    info.mode   = CustomGCode::MultiAsSingle;
    info.gcodes = {{0.4, CustomGCode::PausePrint, 1, "", ""}, {1.0, CustomGCode::ToolChange, 2, "#00FF00", ""}};
    const auto change_of = [&info](const LayerGcodeRules& plate) { return layer_gcodes_json(info, &k_layers, &plate).at(1); };

    CHECK(change_of(rules()).at("active") == true);
    CHECK_FALSE(change_of(rules()).contains("inactive_reason"));
    CHECK_FALSE(layer_gcodes_json(info, &k_layers, nullptr).at(1).contains("active"));
    const LayerGcodeRules plain = rules();
    CHECK_FALSE(layer_gcodes_json(info, &k_layers, &plain).at(0).contains("active")); // a pause

    LayerGcodeRules vase = rules();
    vase.spiral_vase     = true;
    vase.object_filaments.reset(); // known or not, a vase plate takes none
    CHECK(change_of(vase).at("active") == false);
    CHECK(change_of(vase).at("inactive_reason").get<std::string>().find("spiral vase") != std::string::npos);

    CHECK(change_of(rules(2, {1, 2})).at("active") == false);
    CHECK(change_of(rules(2, {1, 2})).at("inactive_reason").get<std::string>().find("several filaments") != std::string::npos);

    LayerGcodeRules not_current = rules();
    not_current.object_filaments.reset();
    CHECK(change_of(not_current).at("active").is_null());

    info.mode = CustomGCode::MultiExtruder;
    CHECK(change_of(rules()).at("active") == false);
    CHECK(change_of(rules()).at("inactive_reason").get<std::string>().find("another filament mode") != std::string::npos);
}

// The slicer turns no filament change into a color change: ToolOrdering::assign_custom_gcodes skips them all, and
// GCode's custom G-code emitter asserts none arrives. So on a project of one filament -- after a slot was deleted,
// say -- a filament change writes nothing, and one to the filament already printing there switches nothing. Both
// were reported active.
TEST_CASE("a filament change that writes nothing in the G-code is reported inactive, with why", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    info.mode   = CustomGCode::MultiAsSingle;
    info.gcodes = {{1.0, CustomGCode::ToolChange, 1, "#FF0000", ""}};

    // Two slots, the object on slot 2 and a change to slot 1; slot 2 deleted: one slot left, the object on it.
    const LayerGcodeRules one_slot = rules(1, {1});
    const nlohmann::json  one      = layer_gcodes_json(info, &k_layers, &one_slot).at(0);
    CHECK(one.at("active") == false);
    CHECK(one.at("inactive_reason").get<std::string>().find("one filament") != std::string::npos);

    // Two slots, the objects on slot 1: a change to slot 1 switches nothing.
    const LayerGcodeRules on_1  = rules(2, {1});
    const nlohmann::json  same  = layer_gcodes_json(info, &k_layers, &on_1).at(0);
    CHECK(same.at("active") == false);
    CHECK(same.at("inactive_reason").get<std::string>().find("already prints") != std::string::npos);

    // To 2, then back to 1: both switch.
    info.gcodes = {{0.4, CustomGCode::ToolChange, 2, "#00FF00", ""}, {1.0, CustomGCode::ToolChange, 1, "#FF0000", ""}};
    const nlohmann::json both = layer_gcodes_json(info, &k_layers, &on_1);
    CHECK(both.at(0).at("active") == true);
    CHECK(both.at(1).at("active") == true);
    // To 2 twice: the second switches nothing.
    info.gcodes[1].extruder = 2;
    CHECK(layer_gcodes_json(info, &k_layers, &on_1).at(1).at("active") == false);

    // Above the plate's last layer the slicer never reaches it.
    info.gcodes = {{5.0, CustomGCode::ToolChange, 2, "#00FF00", ""}};
    const nlohmann::json above = layer_gcodes_json(info, &k_layers, &on_1).at(0);
    CHECK(above.at("active") == false);
    CHECK(above.at("inactive_reason").get<std::string>().find("last layer") != std::string::npos);
}

// Before a plate is sliced its layers are not known, and a change above its objects was reported active though the
// slicer never reaches it (a 3MF with a change at 15 mm on an object scaled to 10 mm). Every layer the slicer makes
// reaches the top of the highest object it prints, so a change at or under it is reached; above it, the last layer
// (a raft lifts it) is known only once the plate is sliced.
TEST_CASE("a filament change above the plate's objects is not reported active before the plate is sliced", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    info.mode   = CustomGCode::MultiAsSingle;
    info.gcodes = {{5.0, CustomGCode::ToolChange, 2, "#00FF00", ""}, {15.0, CustomGCode::ToolChange, 1, "#FF0000", ""}};
    LayerGcodeRules plate = rules(2, {1});
    plate.objects_top_mm  = 10.0;
    const nlohmann::json unsliced = layer_gcodes_json(info, nullptr, &plate);
    CHECK(unsliced.at(0).at("active") == true);
    CHECK(unsliced.at(1).at("active").is_null());
    plate.objects_top_mm.reset();
    CHECK(layer_gcodes_json(info, nullptr, &plate).at(0).at("active").is_null());
}

// The slicer's last layer can stop short of the top: generate_object_layers ends once the next layer's middle would pass
// the top, up to half a layer below it, and Z shrinkage compensation over 100 % slices the object shorter (a 50 mm
// object at 110 % ends at 45.45 mm). A change at the top was taken for reached, and ToolOrdering never applies it.
TEST_CASE("a filament change is reported reached only below where the slicer's last layer may stop", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    info.mode   = CustomGCode::MultiAsSingle;
    info.gcodes = {{9.95, CustomGCode::ToolChange, 2, "#00FF00", ""}, {10.05, CustomGCode::ToolChange, 1, "#FF0000", ""}};
    LayerGcodeRules plate  = rules(2, {1});
    plate.objects_top_mm   = 10.09; // 0.2 mm layers end at 10.0
    plate.largest_layer_mm = 0.2;
    const nlohmann::json half_layer = layer_gcodes_json(info, nullptr, &plate);
    CHECK(half_layer.at(0).at("active") == true); // 9.95 <= 10.09 - 0.1
    CHECK(half_layer.at(1).at("active").is_null());

    info.gcodes              = {{45.0, CustomGCode::ToolChange, 2, "#00FF00", ""}, {46.0, CustomGCode::ToolChange, 1, "#FF0000", ""}};
    plate.objects_top_mm     = 50.0;
    plate.z_shrinkage_percent = 110.0; // sliced to 45.45 mm
    const nlohmann::json shrunk = layer_gcodes_json(info, nullptr, &plate);
    CHECK(shrunk.at(0).at("active") == true);
    CHECK(shrunk.at(1).at("active").is_null());
}

// A variable or adaptive layer height can reach the largest layer the nozzle allows, which the slicer reads from
// max_layer_height, whose default 0 means 3/4 of the nozzle (Slicing::max_layer_height_from_nozzle). Read as 0 mm, an
// adaptive profile's 0.3 mm layers on a 0.4 mm nozzle put the reach of a 10 mm object at 9.9 mm, where the slicer may
// stop at 9.85, and a change at 9.88 was reported active.
TEST_CASE("an object with a variable layer height may print the largest layer its printer's nozzles allow", "[LayerGcode][orcamcp]")
{
    using Catch::Matchers::WithinAbs;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    config.set_key_value("max_layer_height", new ConfigOptionFloats({0.}));
    config.set_key_value("min_layer_height", new ConfigOptionFloats({0.07}));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    Model        model;
    ModelObject& object = *model.add_object();
    CHECK_THAT(object_largest_layer_mm(object, config), WithinAbs(0.2, 1e-9)); // no profile: its layer height
    object.layer_height_profile.set(std::vector<coordf_t>{0., 0.2, 10., 0.3});
    const double largest = object_largest_layer_mm(object, config);
    CHECK_THAT(largest, WithinAbs(0.3, 1e-9)); // 3/4 of 0.4

    CustomGCode::Info info;
    info.mode              = CustomGCode::MultiAsSingle;
    info.gcodes            = {{9.88, CustomGCode::ToolChange, 2, "#00FF00", ""}};
    LayerGcodeRules plate  = rules(2, {1});
    plate.objects_top_mm   = 10.0;
    plate.largest_layer_mm = largest;
    CHECK(layer_gcodes_json(info, nullptr, &plate).at(0).at("active").is_null()); // the slicer may stop at 9.85

    // Each extruder's limit: the largest of them, a max_layer_height set taken as it is.
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.8}));
    config.set_key_value("min_layer_height", new ConfigOptionFloats({0.07, 0.07}));
    config.set_key_value("max_layer_height", new ConfigOptionFloats({0., 0.5}));
    CHECK_THAT(object_largest_layer_mm(object, config), WithinAbs(0.5, 1e-9));
    config.set_key_value("max_layer_height", new ConfigOptionFloats({0., 0.}));
    CHECK_THAT(object_largest_layer_mm(object, config), WithinAbs(0.6, 1e-9)); // 3/4 of 0.8
}

// A change to a slot the project lacks is taken for filament 1 (custom_tool_changes); the reason named the slot it
// was stored with, "filament 5" on a project of two.
TEST_CASE("a filament change that switches nothing names the filament the slicer takes it for", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    info.mode                   = CustomGCode::MultiAsSingle;
    info.gcodes                 = {{1.0, CustomGCode::ToolChange, 5, "", ""}};
    const LayerGcodeRules on_1  = rules(2, {1});
    const std::string     why   = layer_gcodes_json(info, &k_layers, &on_1).at(0).at("inactive_reason").get<std::string>();
    CHECK(why.find("filament 1") != std::string::npos);
    CHECK(why.find("filament 5") == std::string::npos);
    CHECK(why.find("slot 5") != std::string::npos); // how it was mapped
    info.gcodes[0].extruder = 1;
    const std::string plain = layer_gcodes_json(info, &k_layers, &on_1).at(0).at("inactive_reason").get<std::string>();
    CHECK(plain.find("filament 1") != std::string::npos);
    CHECK(plain.find("slot") == std::string::npos);
}

TEST_CASE("nothing goes at a layer of a plate printed by object", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    LayerGcodeRules   by_object = rules();
    by_object.by_object         = true;
    CHECK(mentions(add_layer_gcode(info, k_layers, 2, a_pause(), by_object, change), "print_sequence \"by layer\""));
    CHECK(info.gcodes.empty());
}

TEST_CASE("custom G-code needs text of at most the slider's 1023 characters", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    LayerGcodeChange  change;
    CHECK(mentions(add_layer_gcode(info, k_layers, 1, custom(""), rules(), change), "must not be empty"));
    CHECK(mentions(add_layer_gcode(info, k_layers, 1, custom(std::string(1024, 'M')), rules(), change), "at most 1023"));
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
    CHECK(mentions(add_layer_gcode(info, k_layers, 1, {LayerGcodeKind::template_gcode, 0, {}}, none, change), "no template G-code"));
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
    CHECK(mentions(add_layer_gcode(info, k_layers, 3, a_pause(), rules(), change), "already has a custom"));
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
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 7, a_pause(), rules(), change));
    const CustomGCode::Info before = info;
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 7, a_pause(), rules(), change));
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
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 8, a_pause(), rules(), change));
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
    REQUIRE_FALSE(add_layer_gcode(info, k_layers, 4, a_pause(), rules(), change));
    CHECK(mentions(delete_layer_gcode(info, k_layers, 5, change), "layer 6 has no G-code"));
    REQUIRE_FALSE(delete_layer_gcode(info, k_layers, 4, change));
    CHECK(change.changed);
    CHECK(change.item.type == CustomGCode::PausePrint);
    CHECK(info.gcodes.empty());
    CHECK(mentions(delete_layer_gcode(info, k_layers, 10, change), "does not exist"));
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

// The heights add_layer_gcode puts G-code at are read from the plate's G-code, and kept for the edits made
// before the next slice, while the Print's layers are still that slice's. "Its slicing steps are done" also
// held for another slice's: a changed layer height sliced again answered the same. Each object's steps
// carry the stamp they took when done, and a step done again takes a new one.
TEST_CASE("layers read from a slice are that slice's until an object's layers are sliced again", "[LayerGcode][orcamcp]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2}, {"enable_support", 0}});
    Print print;
    Model model;
    Test::init_print({make_cube(20, 20, 10)}, print, model, config);
    print.process();
    const std::optional<SliceLayersStamp> sliced = slice_layers_stamp(print);
    REQUIRE(sliced.has_value());

    // A change of the G-code only (what a layer G-code edit is): the layers are the same.
    config.set_deserialize_strict({{"machine_start_gcode", "G28 ; another start"}});
    print.apply(model, config);
    CHECK(slice_layers_stamp(print) == sliced);

    // Another layer height: not sliced until the next slice, then another slice's layers.
    config.set_deserialize_strict({{"layer_height", 0.3}});
    print.apply(model, config);
    CHECK_FALSE(slice_layers_stamp(print).has_value());
    print.process();
    const std::optional<SliceLayersStamp> resliced = slice_layers_stamp(print);
    REQUIRE(resliced.has_value());
    CHECK(*resliced != *sliced);
}

// ---- The Preview's layer slider (IMSlider): what it keeps of a plate's layer G-code --------------------
//
// The slider takes the plate's layer G-code when the Preview shows it (SetTicksValues) and gives it back
// (GetTicksValues), which the plater writes into the project on the slider's change. It erased every filament
// change of the plate shown once it printed with several filaments, and wrote that to the project; and a
// static "last vase mode" cleared the ticks of whichever plate was shown next after any vase toggle.

namespace {

// A slider over k_layers for a plate that prints with one filament (`several`: with several), in vase mode or not.
std::unique_ptr<GUI::IMSlider> slider_for(bool several = false, bool vase = false)
{
    auto slider = std::make_unique<GUI::IMSlider>(0, int(k_layers.size()) - 1, 0, int(k_layers.size()) - 1);
    slider->SetSliderValues(k_layers);
    slider->SetMaxValue(int(k_layers.size()) - 1);
    slider->SetModeAndOnlyExtruder(/*is_one_extruder_printed_model=*/true, /*only_extruder=*/1, /*can_change_color=*/!several, vase);
    slider->SetDrawMode(/*is_sequential_print=*/false);
    slider->SetGcodeOnly(false);
    return slider;
}

CustomGCode::Info pause_and_filament_change()
{
    CustomGCode::Info info;
    info.mode   = CustomGCode::MultiAsSingle;
    info.gcodes = {{0.4, CustomGCode::PausePrint, 1, "", ""}, {1.0, CustomGCode::ToolChange, 2, "#00FF00", ""}};
    return info;
}

// Whether `got` holds `expected`'s items: their type, filament and height, in order.
bool same_items(const std::vector<CustomGCode::Item>& got, const std::vector<CustomGCode::Item>& expected)
{
    if (got.size() != expected.size())
        return false;
    for (std::size_t i = 0; i < got.size(); ++i)
        if (got[i].type != expected[i].type || got[i].extruder != expected[i].extruder || std::abs(got[i].print_z - expected[i].print_z) > 1e-9)
            return false;
    return true;
}

} // namespace

TEST_CASE("the slider keeps a filament change it does not show while the plate prints with several filaments", "[LayerGcode][orcamcp]")
{
    const CustomGCode::Info info   = pause_and_filament_change();
    auto                    slider = slider_for(/*several=*/true);
    slider->SetTicksValues(info);
    // What a tick edit writes back into the project: the change is still there.
    CHECK(same_items(slider->GetTicksValues().gcodes, info.gcodes));

    // Printed with one filament again, the slider shows it as a tick, at its layer.
    auto one = slider_for(/*several=*/false);
    one->SetTicksValues(info);
    CHECK(same_items(one->GetTicksValues().gcodes, info.gcodes));
}

TEST_CASE("two items at one layer both stay, though the slider shows one tick there", "[LayerGcode][orcamcp]")
{
    CustomGCode::Info info;
    info.mode   = CustomGCode::MultiAsSingle;
    info.gcodes = {{0.6, CustomGCode::PausePrint, 1, "", ""}, {0.6, CustomGCode::Custom, 1, "", "M117 hi"}};
    auto slider = slider_for();
    slider->SetTicksValues(info);
    CHECK(slider->GetTicksValues().gcodes.size() == 2);
}

// STUDIO-2621 clears a plate's layer G-code once it no longer works: printed by object, or after a vase toggle.
// The Preview makes the clear on the plate's items at once (the slider's deferred change event lost it when the
// slider was updated twice before it), and a static "last vase mode" cleared whichever plate was shown next.
TEST_CASE("a vase toggle clears the layer G-code of the plate it was made on, not of the next plate shown", "[LayerGcode][orcamcp]")
{
    std::map<int, bool> vase_by_plate;
    const auto          shown = [&vase_by_plate](int plate, bool vase) {
        return GUI::clears_plate_layer_gcode(vase_by_plate, plate, vase, /*by_object=*/false);
    };
    CHECK_FALSE(shown(/*plate=*/1, /*vase=*/false));
    // Another plate, first shown in vase mode: its ticks are its own.
    CHECK_FALSE(shown(/*plate=*/2, /*vase=*/true));
    // Back to the first plate, whose mode did not change: they stay, as often as it is shown.
    CHECK_FALSE(shown(1, false));
    CHECK_FALSE(shown(1, false));
    // The first plate switched to vase mode: its ticks no longer work, and go -- once.
    CHECK(shown(1, true));
    CHECK_FALSE(shown(1, true));
    CHECK_FALSE(shown(2, true));
}

TEST_CASE("a plate printed by object has its layer G-code cleared, as upstream means to", "[LayerGcode][orcamcp]")
{
    std::map<int, bool> vase_by_plate;
    CHECK(GUI::clears_plate_layer_gcode(vase_by_plate, 1, false, /*by_object=*/true));
}

// The slider no longer clears anything itself: shown by object, it keeps the plate's items (the Preview has
// cleared them before when it means to).
TEST_CASE("the slider keeps every item of the plate it is given, whatever it shows", "[LayerGcode][orcamcp]")
{
    auto slider = slider_for();
    slider->SetDrawMode(/*is_sequential_print=*/true);
    const CustomGCode::Info info = pause_and_filament_change();
    slider->SetTicksValues(info);
    CHECK(same_items(slider->GetTicksValues().gcodes, info.gcodes));
}

// ---- A plate's filaments with its filament changes (PartPlate::get_extruders) ----------------------

// The plate's filaments, as the Print counts them (Print::extruders): a filament change counts only where
// the slicer applies it -- a by-layer plate whose objects print with one filament.
TEST_CASE("a plate's filament change counts as a filament it uses only where the slicer applies it", "[LayerGcode][orcamcp]")
{
    Model model;
    auto  plates = plate_list_fixtures::plate_list_for(model, 1);
    plate_list_fixtures::add_cube(model, *plates, {plate_list_fixtures::centre_of(*plates, 0)});
    model.plates_custom_gcodes[0] = {CustomGCode::MultiAsSingle, {{6.0, CustomGCode::ToolChange, 3, "#0000FF", ""}}};
    DynamicPrintConfig project;
    project.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF"}));
    DynamicPrintConfig by_layer = DynamicPrintConfig::full_print_config();
    GUI::PartPlate&    plate    = *plates->get_plate(0);

    CHECK(plate.get_extruders(true, by_layer, project) == std::vector<int>{1, 3});

    DynamicPrintConfig by_object = by_layer;
    by_object.set_deserialize_strict({{"print_sequence", "by object"}});
    CHECK(plate.get_extruders(true, by_object, project) == std::vector<int>{1});

    // In vase mode, the plate's own or the print preset's, the slicer takes no filament change.
    DynamicPrintConfig vase = by_layer;
    vase.set_deserialize_strict({{"spiral_mode", 1}});
    CHECK(plate.get_extruders(true, vase, project) == std::vector<int>{1});
    plate.config()->set_key_value("spiral_mode", new ConfigOptionBool(true));
    CHECK(plate.get_extruders(true, by_layer, project) == std::vector<int>{1});
    plate.config()->erase("spiral_mode");

    Vec3d beside = plate_list_fixtures::centre_of(*plates, 0);
    beside.x() += 40.;
    plate_list_fixtures::add_cube(model, *plates, {beside}).config.set_key_value("extruder", new ConfigOptionInt(2));
    CHECK(plate.get_extruders(true, by_layer, project) == std::vector<int>{1, 2});
}

// The slicer leaves out every instance that is not printable -- the object's flag, the instance's, or not inside the
// plate's build volume (ModelInstance::is_printable, PrintApply's print_objects_from_model_object) -- and so do the
// filaments such an object prints. The plate counted them among its objects' filaments, took a filament change for
// one the slicer does not take, and left out the filament it switches to.
TEST_CASE("a plate's objects the slicer does not print do not keep a filament change from being listed", "[LayerGcode][orcamcp]")
{
    Model model;
    auto  plates = plate_list_fixtures::plate_list_for(model, 1);
    Vec3d at     = plate_list_fixtures::centre_of(*plates, 0);
    plate_list_fixtures::add_cube(model, *plates, {at});
    at.x() += 40.;
    ModelObject& other = plate_list_fixtures::add_cube(model, *plates, {at});
    other.config.set_key_value("extruder", new ConfigOptionInt(2));
    model.plates_custom_gcodes[0] = {CustomGCode::MultiAsSingle, {{10.0, CustomGCode::ToolChange, 3, "#0000FF", ""}}};
    DynamicPrintConfig project;
    project.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF"}));
    const DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    GUI::PartPlate&          plate  = *plates->get_plate(0);
    const auto               lists  = [&](int filament) {
        const std::vector<int> listed = plate.get_extruders(true, preset, project);
        return std::find(listed.begin(), listed.end(), filament) != listed.end();
    };
    REQUIRE_FALSE(lists(3)); // two objects on two filaments: the slicer takes no change

    other.printable = false; // the object list's Printable off
    CHECK(lists(3));
    CHECK(lists(1));
    other.printable                = true;
    other.instances[0]->printable = false; // one instance's
    CHECK(lists(3));
    other.instances[0]->printable = true;
    // Standing across the plate's edge: on the plate, but not inside its build volume (and the plate cannot be sliced).
    other.instances[0]->set_offset(Vec3d(plate_list_fixtures::k_plate_size - 5., at.y(), at.z()));
    plates->notify_instance_update(1, 0);
    CHECK(lists(3));
}

// What the slicer leaves out by the build volume alone: on a round bed, an object in a corner of the plate's square is on
// the plate (its box is inside the plate's box, which is all the plate's own containment asks), yet outside the bed's
// circle, so the slicer does not print it. On plate 1, whose shape is moved by the plate's offset.
TEST_CASE("an object outside a round bed's circle does not keep a plate's filament change from being listed", "[LayerGcode][orcamcp]")
{
    Model model;
    auto  plates = plate_list_fixtures::plate_list_for(model, 2, plate_list_fixtures::round_bed());
    GUI::PartPlate& plate = *plates->get_plate(1);
    plate_list_fixtures::add_cube(model, *plates, {plate_list_fixtures::centre_of(*plates, 1)});
    const Vec3d corner = plate.get_plate_box().min + Vec3d(20., 20., plate_list_fixtures::k_cube_size / 2.0);
    plate_list_fixtures::add_cube(model, *plates, {Vec3d(corner.x(), corner.y(), plate_list_fixtures::k_cube_size / 2.0)})
        .config.set_key_value("extruder", new ConfigOptionInt(2));
    REQUIRE(plate.contain_any_instance_totally(1)); // the plate holds it
    model.plates_custom_gcodes[1] = {CustomGCode::MultiAsSingle, {{10.0, CustomGCode::ToolChange, 3, "#0000FF", ""}}};
    DynamicPrintConfig project;
    project.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF"}));
    const std::vector<int> listed = plate.get_extruders(true, DynamicPrintConfig::full_print_config(), project);
    CHECK(std::find(listed.begin(), listed.end(), 3) != listed.end());
}

TEST_CASE("an object on another plate does not keep a plate's filament change from being listed", "[LayerGcode][orcamcp]")
{
    Model model;
    auto  plates = plate_list_fixtures::plate_list_for(model, 2);
    plate_list_fixtures::add_cube(model, *plates, {plate_list_fixtures::centre_of(*plates, 0)});
    plate_list_fixtures::add_cube(model, *plates, {plate_list_fixtures::centre_of(*plates, 1)})
        .config.set_key_value("extruder", new ConfigOptionInt(2));
    model.plates_custom_gcodes[0] = {CustomGCode::MultiAsSingle, {{10.0, CustomGCode::ToolChange, 3, "#0000FF", ""}}};
    DynamicPrintConfig project;
    project.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF"}));
    CHECK(plates->get_plate(0)->get_extruders(true, DynamicPrintConfig::full_print_config(), project) == std::vector<int>{1, 3});
}

// A feature's filament (sparse infill, walls, top and bottom surfaces) prints only where the feature does -- no sparse
// infill at 0 % --, which the slicer knows from each region (Print::object_extruders) and the plate cannot. The plate
// counted the preset's sparse infill filament as one its objects print, dropped the filament change the slicer takes,
// and listed a filament that does not print instead. It now decides by the filaments its objects print whatever their
// features do (their parts' and layer ranges'), so it lists a change the slicer may take rather than miss one it does.
TEST_CASE("a plate lists a filament change the slicer may take, though a feature names another filament", "[LayerGcode][orcamcp]")
{
    Model model;
    auto  plates = plate_list_fixtures::plate_list_for(model, 1);
    plate_list_fixtures::add_cube(model, *plates, {plate_list_fixtures::centre_of(*plates, 0)})
        .config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    model.plates_custom_gcodes[0] = {CustomGCode::MultiAsSingle, {{6.0, CustomGCode::ToolChange, 3, "#0000FF", ""}}};
    DynamicPrintConfig project;
    project.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF"}));
    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_deserialize_strict({{"sparse_infill_filament_id", 2}});

    const std::vector<int> listed = plates->get_plate(0)->get_extruders(true, preset, project);
    CHECK(std::find(listed.begin(), listed.end(), 3) != listed.end()); // the slicer prints filament 3 from 6 mm
}
