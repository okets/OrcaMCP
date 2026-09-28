#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPFilamentSlots.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"

#include <nlohmann/json.hpp>
#include <string>

// Adding, deleting and merging filament slots: what the app's own code does to the model and the
// project when a slot goes, and add_filament_slot / delete_filament_slot's decisions apart from the app.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using json = nlohmann::json;

namespace {

// An AD5X-like project: one extruder that changes filaments, `physical` slots of "Generic PLA", then
// `mixed` mixed slots.
FilamentSlotsState multi_material_project(size_t physical, size_t mixed = 0)
{
    FilamentSlotsState state;
    for (size_t i = 0; i < physical; ++i) {
        state.is_mixed.push_back(false);
        state.slot_presets.push_back("Generic PLA");
    }
    for (size_t i = 0; i < mixed; ++i) {
        state.is_mixed.push_back(true);
        state.slot_presets.push_back("Generic PLA");
    }
    state.multi_material = true;
    state.extruders      = 1;
    state.edited_preset  = "Generic PLA";
    return state;
}

const PipelineState idle_pipeline{};

std::string refusal_text(const std::optional<std::string>& refusal) { return refusal ? *refusal : std::string("none"); }

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The tool's own answer to a tools/call, decoded.
json call_tool(const std::string& tool, const json& arguments)
{
    const json result = GUI::OrcaMCPServer::handle_tools_call({{"name", tool}, {"arguments", arguments}});
    return json::parse(result.at("content").at(0).at("text").get<std::string>());
}

ModelObject* object_with_two_parts(Model& model)
{
    ModelObject* object = model.add_object();
    object->add_volume(TriangleMesh(its_make_cube(10, 10, 10)));
    object->add_volume(TriangleMesh(its_make_cube(5, 5, 5)));
    object->add_instance();
    return object;
}

int support_filament(const ModelConfigObject& config, const char* key) { return config.has(key) ? config.opt_int(key) : -1; }

} // namespace

TEST_CASE("deleting a filament renumbers each support filament in its own config", "[FilamentSlots]")
{
    Model        model;
    ModelObject* object = object_with_two_parts(model);
    object->config.set_key_value("support_filament", new ConfigOptionInt(3));
    object->volumes[0]->config.set_key_value("support_interface_filament", new ConfigOptionInt(4));
    object->volumes[1]->config.set_key_value("support_filament", new ConfigOptionInt(2));

    // Slot 2 (index 1) goes: 3 and 4 move down one, a reference to 2 falls back to the default.
    GUI::renumber_filament_settings(*object, GUI::FilamentRenumbering::deletion(1));

    CHECK(support_filament(object->config, "support_filament") == 2);
    CHECK(support_filament(object->volumes[0]->config, "support_interface_filament") == 3);
    CHECK_FALSE(object->volumes[1]->config.has("support_filament"));
    // Upstream wrote a volume's renumbered value into the object: the object gained the volume's key.
    CHECK_FALSE(object->config.has("support_interface_filament"));
}

TEST_CASE("deleting a filament leaves the support filaments before it as they are", "[FilamentSlots]")
{
    Model        model;
    ModelObject* object = object_with_two_parts(model);
    object->config.set_key_value("support_filament", new ConfigOptionInt(1));
    object->volumes[1]->config.set_key_value("support_interface_filament", new ConfigOptionInt(2));

    GUI::renumber_filament_settings(*object, GUI::FilamentRenumbering::deletion(3));

    CHECK(support_filament(object->config, "support_filament") == 1);
    CHECK(support_filament(object->volumes[1]->config, "support_interface_filament") == 2);
    CHECK_FALSE(object->volumes[0]->config.has("support_filament"));
}

TEST_CASE("deleting a filament renumbers a part's per-feature filaments too", "[FilamentSlots]")
{
    Model        model;
    ModelObject* object = object_with_two_parts(model);
    object->volumes[1]->config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(3));
    object->config.set_key_value("sparse_infill_filament_id", new ConfigOptionInt(2));

    GUI::renumber_filament_settings(*object, GUI::FilamentRenumbering::deletion(1));

    CHECK(object->volumes[1]->config.opt_int("outer_wall_filament_id") == 2);
    CHECK_FALSE(object->config.has("sparse_infill_filament_id")); // it named the deleted slot: the default takes over
}

TEST_CASE("deleting a filament renumbers the print preset's filament settings, keeping every key", "[FilamentSlots]")
{
    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("support_filament", new ConfigOptionInt(4));
    preset.set_key_value("support_interface_filament", new ConfigOptionInt(2));
    preset.set_key_value("wipe_tower_filament", new ConfigOptionInt(1));
    preset.set_key_value("top_surface_filament_id", new ConfigOptionInt(3));

    // Slot 2 goes: 4 and 3 move down one, 2 is the default (0), 1 stays.
    GUI::renumber_filament_settings(preset, GUI::FilamentRenumbering::deletion(1));

    CHECK(preset.opt_int("support_filament") == 3);
    CHECK(preset.opt_int("support_interface_filament") == 0);
    CHECK(preset.opt_int("wipe_tower_filament") == 1);
    CHECK(preset.opt_int("top_surface_filament_id") == 2);
    CHECK(preset.opt_int("outer_wall_filament_id") == 0); // the default stays the default

    CHECK_FALSE(GUI::FilamentRenumbering::deletion(1).number(2, false).has_value());
    CHECK(GUI::FilamentRenumbering::deletion(1).number(0, false) == 0);
}

TEST_CASE("merging a filament moves the settings that named it to the slot it merged into", "[FilamentSlots]")
{
    // Slot 2 (index 1) merged into slot 4, which is slot 3 (index 2) once slot 2 has gone.
    const auto merge = GUI::FilamentRenumbering::deletion(1, 2, false);

    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("support_filament", new ConfigOptionInt(2));
    preset.set_key_value("sparse_infill_filament_id", new ConfigOptionInt(2));
    preset.set_key_value("wipe_tower_filament", new ConfigOptionInt(4));
    GUI::renumber_filament_settings(preset, merge);
    CHECK(preset.opt_int("support_filament") == 3);
    CHECK(preset.opt_int("sparse_infill_filament_id") == 3);
    CHECK(preset.opt_int("wipe_tower_filament") == 3);

    Model        model;
    ModelObject* object = object_with_two_parts(model);
    object->volumes[1]->config.set_key_value("support_interface_filament", new ConfigOptionInt(2));
    object->config.set_key_value("top_surface_filament_id", new ConfigOptionInt(2));
    GUI::renumber_filament_settings(*object, merge);
    CHECK(object->volumes[1]->config.opt_int("support_interface_filament") == 3);
    CHECK(object->config.opt_int("top_surface_filament_id") == 3);
}

TEST_CASE("merging a filament into a mixed one leaves support and the wipe tower on the default", "[FilamentSlots]")
{
    // Support and the wipe tower print from a physical filament only (ConfigManipulation's
    // physical_only_keys); a feature may print from a mix.
    const auto into_mix = GUI::FilamentRenumbering::deletion(1, 2, true);
    CHECK(into_mix.number(2, true) == std::nullopt);
    CHECK(into_mix.number(2, false) == 3);

    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("support_filament", new ConfigOptionInt(2));
    preset.set_key_value("outer_wall_filament_id", new ConfigOptionInt(2));
    GUI::renumber_filament_settings(preset, into_mix);
    CHECK(preset.opt_int("support_filament") == 0);
    CHECK(preset.opt_int("outer_wall_filament_id") == 3);
}

TEST_CASE("adding a filament before the mixed ones moves every number that names a mixed slot", "[FilamentSlots]")
{
    // Three physical slots and mixed slots 4 and 5; the new physical slot goes in at 4 (index 3).
    Model        model;
    ModelObject* object = object_with_two_parts(model);
    object->config.set_key_value("extruder", new ConfigOptionInt(4));
    object->config.set_key_value("support_filament", new ConfigOptionInt(2));
    object->volumes[0]->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->volumes[1]->config.set_key_value("extruder", new ConfigOptionInt(5));
    object->volumes[1]->config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(4));
    ModelConfig& range = object->layer_config_ranges[{2.0, 5.0}];
    range.set_key_value("extruder", new ConfigOptionInt(5));
    range.set_key_value("sparse_infill_filament_id", new ConfigOptionInt(4));
    CustomGCode::Info& plate = model.plates_custom_gcodes[0];
    plate.gcodes.push_back({3.0, CustomGCode::ToolChange, 4, "", ""});
    plate.gcodes.push_back({4.0, CustomGCode::ToolChange, 2, "", ""});

    GUI::renumber_filaments_after_insert(model, 3);

    CHECK(object->config.opt_int("extruder") == 5);
    CHECK(object->config.opt_int("support_filament") == 2);
    CHECK(object->volumes[0]->config.opt_int("extruder") == 2);
    CHECK(object->volumes[1]->config.opt_int("extruder") == 6);
    CHECK(object->volumes[1]->config.opt_int("outer_wall_filament_id") == 5);
    CHECK(range.opt_int("extruder") == 6);
    CHECK(range.opt_int("sparse_infill_filament_id") == 5);
    CHECK(plate.gcodes[0].extruder == 5);
    CHECK(plate.gcodes[1].extruder == 2);

    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("top_surface_filament_id", new ConfigOptionInt(5));
    preset.set_key_value("support_filament", new ConfigOptionInt(3));
    GUI::renumber_filament_settings(preset, GUI::FilamentRenumbering::insertion(3));
    CHECK(preset.opt_int("top_surface_filament_id") == 6);
    CHECK(preset.opt_int("support_filament") == 3);
}

TEST_CASE("a slot is added only where the sidebar offers its + button", "[FilamentSlots]")
{
    CHECK_FALSE(add_slot_refusal(multi_material_project(1), idle_pipeline, false).has_value());

    // A Creator 5 Pro: four extruders, one filament each.
    FilamentSlotsState toolchanger = multi_material_project(4);
    toolchanger.multi_material     = false;
    toolchanger.extruders          = 4;
    CHECK(mentions(refusal_text(add_slot_refusal(toolchanger, idle_pipeline, false)), "4 extruders, each holding one filament"));

    FilamentSlotsState single = multi_material_project(1);
    single.multi_material     = false;
    CHECK(mentions(refusal_text(add_slot_refusal(single, idle_pipeline, false)), "single_extruder_multi_material"));

    FilamentSlotsState preview = multi_material_project(2);
    preview.gcode_preview      = true;
    CHECK(mentions(refusal_text(add_slot_refusal(preview, idle_pipeline, false)), "new_project"));

    CHECK(mentions(refusal_text(add_slot_refusal(multi_material_project(64), idle_pipeline, false)), "the most the app takes"));
}

TEST_CASE("a slot is neither added nor deleted while slicing or while a job runs", "[FilamentSlots]")
{
    PipelineState slicing;
    slicing.is_slicing = true;
    CHECK(mentions(refusal_text(add_slot_refusal(multi_material_project(2), slicing, false)), "wait_for_slice"));
    CHECK(mentions(refusal_text(add_slot_refusal(multi_material_project(2), idle_pipeline, true)), "add_filament_slot again"));

    DeleteSlotRequest request;
    request.slot = 2;
    CHECK(mentions(refusal_text(delete_slot_refusal(multi_material_project(2), request, slicing, false)), "nothing was deleted"));
    CHECK(mentions(refusal_text(delete_slot_refusal(multi_material_project(2), request, idle_pipeline, true)), "delete_filament_slot again"));
}

TEST_CASE("a slot is deleted as its Delete and Merge with allow", "[FilamentSlots]")
{
    const FilamentSlotsState project = multi_material_project(3, 1);
    DeleteSlotRequest        request;

    request.slot = 3;
    CHECK_FALSE(delete_slot_refusal(project, request, idle_pipeline, false).has_value());
    request.slot = 4; // the mixed slot
    CHECK_FALSE(delete_slot_refusal(project, request, idle_pipeline, false).has_value());

    request.slot = 5;
    CHECK(mentions(refusal_text(delete_slot_refusal(project, request, idle_pipeline, false)), "the project has 4 filament slots, 1 to 4"));
    request.slot       = 2;
    request.merge_into = 2;
    CHECK(mentions(refusal_text(delete_slot_refusal(project, request, idle_pipeline, false)), "merge_into is the slot being deleted"));
    request.merge_into = 0;
    CHECK(mentions(refusal_text(delete_slot_refusal(project, request, idle_pipeline, false)), "merge_into 0 is out of range"));

    DeleteSlotRequest last;
    last.slot = 1;
    CHECK(mentions(refusal_text(delete_slot_refusal(multi_material_project(1, 0), last, idle_pipeline, false)), "only physical filament slot"));

    FilamentSlotsState toolchanger = multi_material_project(4);
    toolchanger.multi_material     = false;
    toolchanger.extruders          = 4;
    CHECK(mentions(refusal_text(delete_slot_refusal(toolchanger, last, idle_pipeline, false)), "follow its extruders"));
}

TEST_CASE("deleting or merging a slot a mix is made of waits for allow_breaking_mix", "[FilamentSlots]")
{
    const FilamentSlotsState project = multi_material_project(3, 1);
    DeleteSlotRequest        request;
    request.slot           = 1;
    request.merge_into     = 4;
    request.breaks_mix     = 4;
    request.mix_components = {1, 3};

    const std::string refusal = refusal_text(delete_slot_refusal(project, request, idle_pipeline, false));
    CHECK(mentions(refusal, "mixed slot 4 is made of (slots 1 and 3): merging it breaks the mix"));
    CHECK(mentions(refusal, "allow_breaking_mix: true"));
    CHECK(mentions(refusal, "delete_mixed_filament {slot: 4}"));

    request.allow_breaking_mix = true;
    CHECK_FALSE(delete_slot_refusal(project, request, idle_pipeline, false).has_value());

    // A plain delete of a slot a mix is made of breaks it too, which the app does not ask about.
    DeleteSlotRequest plain;
    plain.slot           = 3;
    plain.breaks_mix     = 4;
    plain.mix_components = {1, 3};
    CHECK(mentions(refusal_text(delete_slot_refusal(project, plain, idle_pipeline, false)), "deleting it breaks the mix"));
}

TEST_CASE("a delete that re-selects the Filament settings waits for their unsaved changes", "[FilamentSlots]")
{
    FilamentSlotsState project = multi_material_project(3);
    project.slot_presets       = {"Generic PLA", "Generic PETG", "Generic ABS"};
    project.edited_preset      = "Generic PETG";

    // Slot 1 always re-selects; the only slot using the edited preset does too; another slot does not.
    CHECK(delete_reselects_edited_preset(project, 0));
    CHECK(delete_reselects_edited_preset(project, 1));
    CHECK_FALSE(delete_reselects_edited_preset(project, 2));

    DeleteSlotRequest request;
    request.slot = 2;
    CHECK_FALSE(delete_slot_refusal(project, request, idle_pipeline, false).has_value());

    project.edited_dirty = true;
    project.dirty_keys   = {"nozzle_temperature"};
    const std::string refusal = refusal_text(delete_slot_refusal(project, request, idle_pipeline, false));
    CHECK(mentions(refusal, "unsaved changes to 'Generic PETG' (nozzle_temperature)"));
    CHECK(mentions(refusal, "save_preset"));
    CHECK(mentions(refusal, "reset_preset"));
    request.slot = 3;
    CHECK_FALSE(delete_slot_refusal(project, request, idle_pipeline, false).has_value());
}

TEST_CASE("an add or a delete says which slots it renumbered", "[FilamentSlots]")
{
    // A physical slot goes before the mixed ones: slots 3 and 4 become 4 and 5.
    CHECK(renumbered_after_add(multi_material_project(2, 2)) == json::array({{{"from", 3}, {"to", 4}}, {{"from", 4}, {"to", 5}}}));
    CHECK(renumbered_after_add(multi_material_project(2)) == json::array());

    CHECK(renumbered_after_delete(4, 2) == json::array({{{"from", 3}, {"to", 2}}, {{"from", 4}, {"to", 3}}}));
    CHECK(renumbered_after_delete(4, 4) == json::array());

    // Slot 2 goes: object 2 printed with it and moved to slot 1; object 1's slot 3 is renumbered, not changed.
    CHECK(objects_changed({{1}, {1, 3}, {2}}, {{1}, {1, 2}, {1}}, 2) ==
          json::array({{{"object_id", 2}, {"filaments_before", {2}}, {"filaments_after", {1}}}}));
    // Merged into slot 3, which is 2 now: the same number, another filament, and still a change.
    CHECK(objects_changed({{2}}, {{2}}, 2) == json::array({{{"object_id", 0}, {"filaments_before", {2}}, {"filaments_after", {2}}}}));
}

TEST_CASE("a slot's filament settings are edited in that slot's preset", "[FilamentSlots]")
{
    FilamentSlotsState project = multi_material_project(3, 1);
    project.slot_presets       = {"Generic PLA", "Generic PETG", "Generic PLA", "Generic PLA"};
    project.edited_preset      = "Generic PLA";

    SlotEdit plan;
    REQUIRE_FALSE(slot_edit_refusal(project, 2, false, plan).has_value());
    CHECK(plan.index == 1);
    CHECK(plan.preset == "Generic PETG");
    CHECK(plan.select_needed);
    CHECK(plan.slots == std::vector<int>{2});

    // Slots 1 and 3 share a preset: a change reaches both, so it is asked for.
    const std::string shared = refusal_text(slot_edit_refusal(project, 1, false, plan));
    CHECK(mentions(shared, "slot 3's too"));
    CHECK(mentions(shared, "clone_preset"));
    CHECK(mentions(shared, "select_preset"));
    REQUIRE_FALSE(slot_edit_refusal(project, 1, true, plan).has_value());
    CHECK(plan.slots == std::vector<int>{1, 3});
    CHECK_FALSE(plan.select_needed);

    CHECK(mentions(refusal_text(slot_edit_refusal(project, 4, false, plan)), "set_mixed_filament"));
    CHECK(mentions(refusal_text(slot_edit_refusal(project, 5, false, plan)), "out of range"));

    // Switching to another slot's preset would discard unsaved changes; staying on it would not.
    project.edited_dirty = true;
    CHECK(mentions(refusal_text(slot_edit_refusal(project, 2, false, plan)), "unsaved changes to 'Generic PLA'"));
    CHECK_FALSE(slot_edit_refusal(project, 1, true, plan).has_value());
}

TEST_CASE("filament settings name their slot when there are several", "[FilamentSlots]")
{
    CHECK_FALSE(slot_needed_refusal(multi_material_project(1, 2), "filament_slot").has_value());

    FilamentSlotsState project = multi_material_project(3);
    project.slot_presets       = {"Generic PLA", "Generic PETG", "Generic PLA"};
    project.edited_preset      = "Generic PETG";
    const std::string refusal  = refusal_text(slot_needed_refusal(project, "filament_slot"));
    CHECK(mentions(refusal, "slot 2's"));
    CHECK(mentions(refusal, "pass filament_slot (1 to 3)"));
}

TEST_CASE("add_filament_slot and delete_filament_slot refuse a malformed call before the app", "[FilamentSlots][orcamcp]")
{
    CHECK(call_tool("add_filament_slot", {{"color", "red"}})["message"] == "color must be \"#RRGGBB\"");
    CHECK(call_tool("add_filament_slot", {{"color", "#FF000080"}})["message"] == "color must be \"#RRGGBB\"");
    CHECK(mentions(call_tool("add_filament_slot", {{"preset", ""}})["message"], "preset must be a filament preset's name"));
    CHECK(call_tool("delete_filament_slot", {{"slot", "two"}})["message"] == "slot must be an integer, 1-based");
    CHECK(mentions(call_tool("delete_filament_slot", {{"slot", 2}, {"merge_into", "one"}})["message"], "merge_into must be an integer"));
    CHECK(call_tool("delete_filament_slot", {{"slot", 2}, {"allow_breaking_mix", "yes"}})["message"] == "allow_breaking_mix must be a boolean");
}

TEST_CASE("apply_config refuses a filament_slot it could not act on, before the app", "[FilamentSlots][orcamcp]")
{
    const json print_only = json::array({{{"type", "print"}, {"key", "layer_height"}, {"value", "0.2"}}});
    const json filament   = json::array({{{"type", "filament"}, {"key", "nozzle_temperature"}, {"value", "230"}}});

    CHECK(call_tool("apply_config", {{"settings", print_only}, {"filament_slot", 2}})["message"] ==
          "filament_slot goes with filament settings (type filament), and this call has none");
    CHECK(call_tool("apply_config", {{"settings", filament}, {"include_sharing_slots", true}})["message"] ==
          "include_sharing_slots goes with filament_slot: name the slot whose preset to change");
    CHECK(call_tool("apply_config", {{"settings", filament}, {"filament_slot", "two"}})["message"] ==
          "filament_slot must be an integer, 1-based");
    CHECK(call_tool("apply_config", {{"settings", filament}, {"filament_slot", 2}, {"include_sharing_slots", "yes"}})["message"] ==
          "include_sharing_slots must be a boolean");
}

TEST_CASE("a slot change that renumbered anything warns that an undo would put objects on the wrong slots", "[FilamentSlots]")
{
    const json moved = json::array({{{"from", 3}, {"to", 2}}});
    const json object = json::array({{{"object_id", 0}, {"filaments_before", {2}}, {"filaments_after", {1}}}});

    const auto warning = slot_change_undo_warning(moved, json::array());
    REQUIRE(warning.has_value());
    CHECK(mentions(*warning, "undo"));
    CHECK(mentions(*warning, "wrong"));
    CHECK(slot_change_undo_warning(json::array(), object).has_value());
    // Nothing renumbered and nothing moved: an undo restores the same numbers.
    CHECK_FALSE(slot_change_undo_warning(json::array(), json::array()).has_value());
}

TEST_CASE("pointing the Filament settings at a slot leaves an open settings window editing that slot", "[FilamentSlots]")
{
    // Settings closed (no slot edited): they stay closed to the slots, as after the settings' own close.
    CHECK(editing_slot_after_pointing(-1, 2) == -1);
    // Settings open on a slot: they now show slot 3's preset, so a pick there reaches slot 3, as the slot's
    // Edit would leave it -- never slot 1, whose preset they no longer show.
    CHECK(editing_slot_after_pointing(0, 2) == 2);
    CHECK(editing_slot_after_pointing(2, 2) == 2);
}

TEST_CASE("a setting that names a filament slot takes only a slot the project has", "[FilamentSlots]")
{
    const FilamentSlotsState project = multi_material_project(4, 1); // slots 1-4 physical, 5 mixed

    // 0 is the default (the object's own filament), and every slot the project has is a value.
    for (const char* key : {"support_filament", "sparse_infill_filament_id", "extruder"}) {
        CHECK_FALSE(filament_number_refusal(project, key, 0).has_value());
        CHECK_FALSE(filament_number_refusal(project, key, 4).has_value());
    }
    // A slot past the last one would reach the slicer as a filament that does not exist: the app's own
    // lists never offer it.
    const auto past = filament_number_refusal(project, "sparse_infill_filament_id", 6);
    REQUIRE(past.has_value());
    CHECK(mentions(*past, "5 filament slots"));
    CHECK(filament_number_refusal(project, "extruder", 9).has_value());
    CHECK(filament_number_refusal(project, "support_filament", -1).has_value());
    // Settings that name no filament are not this rule's.
    CHECK_FALSE(filament_number_refusal(project, "wall_loops", 9).has_value());
}

TEST_CASE("support and the wipe tower take a physical slot, the per-feature filaments a mixed one too", "[FilamentSlots]")
{
    const FilamentSlotsState project = multi_material_project(4, 1); // slot 5 is mixed

    for (const char* key : {"support_filament", "support_interface_filament", "wipe_tower_filament"}) {
        const auto refusal = filament_number_refusal(project, key, 5);
        REQUIRE(refusal.has_value());
        CHECK(mentions(*refusal, "mixed"));
        CHECK(mentions(*refusal, "1-4"));
    }
    // The per-feature filaments are resolved layer by layer, and an object prints with a mix as its filament.
    CHECK_FALSE(filament_number_refusal(project, "top_surface_filament_id", 5).has_value());
    CHECK_FALSE(filament_number_refusal(project, "extruder", 5).has_value());
}

TEST_CASE("the filament numbers a batch of settings may not take are named with why, the rest left alone", "[FilamentSlots]")
{
    const FilamentSlotsState project = multi_material_project(2, 1); // slot 3 is mixed
    DynamicPrintConfig       settings;
    settings.set_key_value("support_filament", new ConfigOptionInt(3));          // mixed: refused
    settings.set_key_value("sparse_infill_filament_id", new ConfigOptionInt(3)); // mixed: fine
    settings.set_key_value("outer_wall_filament_id", new ConfigOptionInt(4));    // no slot 4: refused
    settings.set_key_value("wall_loops", new ConfigOptionInt(40));               // names no filament

    const auto refused = refused_filament_numbers(project, settings);
    REQUIRE(refused.size() == 2);
    CHECK(refused[0].first == "outer_wall_filament_id");
    CHECK(refused[1].first == "support_filament");
    CHECK(mentions(refused[1].second, "mixed"));
}

namespace {

using GcodeItems = std::vector<CustomGCode::Item>;

// The plate's items after `change`, as "<type> <filament>" in print_z order.
std::vector<std::string> gcodes_after(GcodeItems items, const GUI::FilamentRenumbering& change)
{
    Model model;
    model.plates_custom_gcodes[0].gcodes = std::move(items);
    GUI::renumber_custom_gcodes(model, change);
    std::vector<std::string> after;
    for (const CustomGCode::Item& item : model.plates_custom_gcodes[0].gcodes) {
        const char* type = item.type == CustomGCode::ToolChange  ? "tool" :
                           item.type == CustomGCode::ColorChange ? "colour" :
                           item.type == CustomGCode::PausePrint  ? "pause" : "custom";
        after.push_back(std::string(type) + " " + std::to_string(item.extruder));
    }
    return after;
}

using Items = std::vector<std::string>;

} // namespace

TEST_CASE("merging a slot moves the tool changes to it onto the target, numbered as after the merge", "[FilamentSlots]")
{
    // Slots 1-3, a tool change at z 5 to slot 2; slot 2 merged into 3, which is slot 2 after the merge (index 1).
    CHECK(gcodes_after({{5.0, CustomGCode::ToolChange, 2, "", ""}}, GUI::FilamentRenumbering::deletion(1, 1)) ==
          Items{"tool 2"});

    // Every tool change to the merged slot moves, not only the first, and one to a later slot moves down one.
    const GcodeItems items{{5.0, CustomGCode::ToolChange, 2, "", ""},
                           {8.0, CustomGCode::ToolChange, 2, "", ""},
                           {9.0, CustomGCode::ToolChange, 3, "", ""},
                           {10.0, CustomGCode::ToolChange, 1, "", ""}};
    CHECK(gcodes_after(items, GUI::FilamentRenumbering::deletion(1, 0)) ==
          Items{"tool 1", "tool 1", "tool 2", "tool 1"});
}

TEST_CASE("deleting a slot drops the tool and colour changes to it, and keeps a pause or custom G-code", "[FilamentSlots]")
{
    // As upstream drops a tool change to a deleted slot; a colour change for it goes too, its filament gone. A
    // pause or custom G-code only records the filament printing there, and stays, on slot 1.
    const GcodeItems items{{2.0, CustomGCode::ColorChange, 2, "#FF0000", ""},
                           {3.0, CustomGCode::ToolChange, 2, "", ""},
                           {4.0, CustomGCode::PausePrint, 2, "", ""},
                           {5.0, CustomGCode::Custom, 2, "", "M117 hi"},
                           {6.0, CustomGCode::ColorChange, 3, "#00FF00", ""},
                           {7.0, CustomGCode::PausePrint, 3, "", ""}};
    CHECK(gcodes_after(items, GUI::FilamentRenumbering::deletion(1)) == Items{"pause 1",
                                                                               "custom 1",
                                                                               "colour 2",
                                                                               "pause 2"});
}

TEST_CASE("a merge moves colour changes and every other item to the target too, as an add renumbers them all", "[FilamentSlots]")
{
    const GcodeItems items{{2.0, CustomGCode::ColorChange, 3, "#FF0000", ""},
                           {4.0, CustomGCode::PausePrint, 3, "", ""},
                           {6.0, CustomGCode::ColorChange, 4, "#00FF00", ""}};
    // Slot 3 merged into 1.
    CHECK(gcodes_after(items, GUI::FilamentRenumbering::deletion(2, 0)) ==
          Items{"colour 1", "pause 1", "colour 3"});
    // And an add before slot 3 moves every one of them up, whatever its type.
    CHECK(gcodes_after(items, GUI::FilamentRenumbering::insertion(2)) ==
          Items{"colour 4", "pause 4", "colour 5"});
}
