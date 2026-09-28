#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
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
    GUI::renumber_filament_settings_after_delete(*object, 1);

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

    GUI::renumber_filament_settings_after_delete(*object, 3);

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

    GUI::renumber_filament_settings_after_delete(*object, 1);

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
    GUI::renumber_filament_settings_after_delete(preset, 1);

    CHECK(preset.opt_int("support_filament") == 3);
    CHECK(preset.opt_int("support_interface_filament") == 0);
    CHECK(preset.opt_int("wipe_tower_filament") == 1);
    CHECK(preset.opt_int("top_surface_filament_id") == 2);
    CHECK(preset.opt_int("outer_wall_filament_id") == 0); // the default stays the default

    CHECK_FALSE(GUI::filament_number_after_delete(2, 1).has_value());
    CHECK(GUI::filament_number_after_delete(0, 1) == 0);
}

TEST_CASE("a filament a mixed filament lists is one whose delete breaks it", "[FilamentSlots]")
{
    PresetBundle bundle;
    // Three physical slots and a mixed slot 4 of slots 1 and 3.
    bundle.project_config.option<ConfigOptionBools>("filament_is_mixed", true)->values           = {false, false, false, true};
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components", true)->values = {"", "", "", "1,3"};

    CHECK(bundle.merge_breaks_mixed_filament(0, 3));
    CHECK(bundle.merge_breaks_mixed_filament(2, 3));
    CHECK_FALSE(bundle.merge_breaks_mixed_filament(1, 3)); // not a component
    CHECK_FALSE(bundle.merge_breaks_mixed_filament(3, 0)); // a mixed filament merged away breaks nothing
    CHECK_FALSE(bundle.merge_breaks_mixed_filament(0, 1)); // into a physical one
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

TEST_CASE("merging a slot into a mix made of it waits for allow_breaking_mix", "[FilamentSlots]")
{
    const FilamentSlotsState project = multi_material_project(3, 1);
    DeleteSlotRequest        request;
    request.slot           = 1;
    request.merge_into     = 4;
    request.breaks_mix     = true;
    request.mix_components = {1, 3};

    const std::string refusal = refusal_text(delete_slot_refusal(project, request, idle_pipeline, false));
    CHECK(mentions(refusal, "mixed slot 4 is made of (slots 1 and 3)"));
    CHECK(mentions(refusal, "allow_breaking_mix: true"));

    request.allow_breaking_mix = true;
    CHECK_FALSE(delete_slot_refusal(project, request, idle_pipeline, false).has_value());
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

    CHECK(objects_changed({{1}, {1, 3}, {2}}, {{1}, {1, 2}, {1}}) ==
          json::array({{{"object_id", 1}, {"filaments_before", {1, 3}}, {"filaments_after", {1, 2}}},
                       {{"object_id", 2}, {"filaments_before", {2}}, {"filaments_after", {1}}}}));
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
