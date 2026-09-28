// src/slic3r/GUI/OrcaMCP/OrcaMCPPlateTools.cpp
//
// set_plate_settings: a plate's name, lock, bed type, print sequence, filament orders and spiral vase, as
// the plate settings dialog, the plate's name editor and its lock icon change them. The decisions are
// OrcaMCPPlateSettings.hpp.
#include "OrcaMCPPlateTools.hpp"
#include "OrcaMCPPlateSettings.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPServer.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// What the dialog offers on this printer and project.
PlateSettingsOffer plate_settings_offer()
{
    PresetBundle&             bundle  = *wxGetApp().preset_bundle;
    const DynamicPrintConfig& project = bundle.project_config;
    PlateSettingsOffer        offer;
    if (const auto* colours = project.option<ConfigOptionStrings>("filament_colour"))
        offer.filament_count = int(colours->values.size());
    if (const auto* mixed = project.option<ConfigOptionBools>("filament_is_mixed"))
        offer.mixed_filaments = has_any_mixed_filament(mixed->values);
    // The dialog greys the bed type out on every printer but Bambu Lab's (PlateSettingsDialog).
    offer.plate_bed_type = bundle.is_bbl_vendor();
    offer.bed_types      = wxGetApp().sidebar().get_cur_combox_bed_types();
    return offer;
}

bool global_spiral_vase()
{
    const DynamicPrintConfig& print = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    return print.has("spiral_mode") && print.opt_bool("spiral_mode");
}

nlohmann::json effective_json(const PlateSettings& settings)
{
    const DynamicPrintConfig& project = wxGetApp().preset_bundle->project_config;
    const BedType global_bed_type     = project.has("curr_bed_type") ? project.opt_enum<BedType>("curr_bed_type") : btDefault;
    return plate_effective_json(settings, global_bed_type, wxGetApp().global_print_sequence(), global_spiral_vase());
}

bool effective_vase(const PlateSettings& settings) { return effective_json(settings).at("spiral_vase").get<bool>(); }

// The objects standing on the plate, once each and in object order, with their settings now.
using ObjectConfigs = std::vector<std::pair<const ModelObject*, DynamicPrintConfig>>;
ObjectConfigs object_configs_on(PartPlate& plate)
{
    ObjectConfigs configs;
    for (const ModelObject* object : objects_on_plate(plate))
        configs.emplace_back(object, object->config.get());
    return configs;
}

// The plates other than `plate_index` an instance of the object stands on.
std::vector<int> other_plates_of(const ModelObject& object, int object_index, int plate_index)
{
    std::set<int> plates;
    for (const InstancePlacement& placement : instance_placements(object, object_index, wxGetApp().plater()->get_partplate_list()))
        if (placement.plate_index >= 0 && placement.plate_index != plate_index)
            plates.insert(placement.plate_index);
    return std::vector<int>(plates.begin(), plates.end());
}

// What the dialog's Enable, answered Yes, gave the plate's objects: each one whose settings changed,
// with the keys and the other plates its copies stand on (the settings are the object's, so they print
// with them there too).
nlohmann::json vase_settings_applied(const ObjectConfigs& before, int plate_index)
{
    nlohmann::json applied = nlohmann::json::array();
    for (const auto& [object, config] : before) {
        const std::vector<std::string> changed = changed_config_keys(config, object->config.get());
        if (changed.empty())
            continue;
        const int index = model_object_index(object);
        applied.push_back({{"object_id", index},
                           {"name", object->name},
                           {"settings", changed},
                           {"also_on_plates", other_plates_of(*object, index, plate_index)}});
    }
    return applied;
}

// The objects on the plate that still carry the vase's object settings, and the first one's keys.
std::vector<NextStep> vase_left_behind_steps(PartPlate& plate)
{
    std::vector<int>         object_ids;
    std::vector<std::string> first_keys;
    for (const auto& [object, config] : object_configs_on(plate)) {
        const std::vector<std::string> carried = vase_settings_carried(config);
        if (carried.empty())
            continue;
        object_ids.push_back(model_object_index(object));
        if (first_keys.empty())
            first_keys = carried;
    }
    return vase_settings_next_steps(object_ids, first_keys);
}

// Makes `plate_index` the current plate, as the plate's settings icon does first; not an undo step.
void make_current(Plater& plater, int plate_index)
{
    if (plater.get_partplate_list().get_curr_plate_index() == plate_index)
        return;
    Plater::SuppressSnapshots not_an_edit(&plater);
    plater.select_plate(plate_index);
}

// The dialog's OK (Plater::open_platesettings_dialog's EVT_SET_BED_TYPE_CONFIRM handler), for the settings
// the request changes, in its order. A plate follows the global spiral vase when it is on.
void apply_dialog_settings(Plater& plater, PartPlate& plate, const PlateSettingsRequest& request, const std::vector<std::string>& changes)
{
    const auto changed = [&changes](const char* name) { return std::find(changes.begin(), changes.end(), name) != changes.end(); };
    if (changed("bed_type")) {
        plate.set_bed_type(*request.bed_type);
        plater.update_project_dirty_from_presets();
        plater.set_plater_dirty(true);
    }
    if (changed("first_layer_filament_order"))
        plate.set_first_layer_print_sequence(*request.first_layer_order);
    if (changed("other_layers_filament_order"))
        plate.set_other_layers_print_sequence(*request.other_layers_order);
    if (changed("print_sequence"))
        plate.set_print_seq(*request.print_sequence);
    if (changed("spiral_vase")) {
        // Enable asks its own question -- change the objects' settings and turn the vase on? -- which the
        // call's "on" answers Yes (MsgDialog under suppression); No would cancel the vase.
        const SpiralVase vase = *request.spiral_vase;
        plate.set_spiral_vase_mode(vase == SpiralVase::on, vase == SpiralVase::global);
    }
    plater.update_project_dirty_from_presets();
    plater.set_plater_dirty(true);
    plater.config_change_notification(*plate.config(), "print_sequence");
    plater.update();
    wxGetApp().obj_list()->update_selections();
    plater.schedule_background_process();
}

nlohmann::json set_plate_settings_on_main_thread(const nlohmann::json& params)
{
    Plater*        plater = wxGetApp().plater();
    GUI::PartPlateList& plates = plater->get_partplate_list();
    std::string    error;
    const auto     plate_arg = read_plate_index(params, error);
    if (!error.empty())
        return error_response(error);
    const int plate_index = plate_arg.value_or(plates.get_curr_plate_index());
    if (const auto refusal = plate_index_error(plate_index, plates.get_plate_count()))
        return error_response(*refusal);
    auto request = read_plate_settings(params, plate_settings_offer(), plate_settings_of(*plates.get_plate(plate_index)), error);
    if (!request)
        return error_response(error);
    // An arrange of every plate locks the plates whose print sequence differs while it runs, and unlocks
    // them after; a plate's arrange applies its result to the current plate. Neither is read or changed
    // under one.
    if (asks_anything(*request))
        if (const auto refusal = edit_job_refusal(!plater->get_ui_job_worker().is_idle(), "set_plate_settings"))
            return error_response(*refusal);

    PartPlate&          plate  = *plates.get_plate(plate_index);
    const PlateSettings before = plate_settings_of(plate);
    nlohmann::json      answer = {{"status", "success"}, {"plate_index", plate_index}};
    // The dialog's Enable on a plate that follows a global vase that is on changes nothing
    // (PartPlate::set_spiral_vase_mode): the plate keeps following it.
    if (request->spiral_vase == SpiralVase::on && before.spiral_vase == SpiralVase::global && global_spiral_vase()) {
        request->spiral_vase.reset();
        answer["spiral_vase_note"] = "spiral vase is on in the print settings, which this plate follows, so it stays \"global\" (on), as "
                                     "the dialog's Enable leaves it";
    }
    const std::vector<std::string> changes = plate_settings_changes(before, *request);
    answer["changed"]                     = !changes.empty();
    if (!changes.empty()) {
        McpDialogSuppressionGuard                              guard;
        const ObjectConfigs       configs = object_configs_on(plate);
        {
            Plater::TakeSnapshot snapshot(plater, "Plate Settings");
            if (changes_slicing(changes)) {
                make_current(*plater, plate_index);
                apply_dialog_settings(*plater, plate, *request, changes);
                mark_plate_unsliced(plates, plate_index);
            }
            if (request->name && *request->name != before.name)
                plate.set_plate_name(*request->name);
            if (request->locked && *request->locked != before.locked)
                plates.lock_plate(plate_index, *request->locked);
            if (!changes_slicing(changes))
                plater->update();
        }
        const PlateSettings after = plate_settings_of(plate);
        answer["changed_settings"] = plate_settings_differences(before, after);
        const nlohmann::json vase  = vase_settings_applied(configs, plate_index);
        if (!vase.empty())
            answer["vase_settings_applied"] = vase;
        std::vector<NextStep> steps;
        if (after.print_sequence != before.print_sequence && effective_json(after).at("print_sequence") == "by object")
            steps = print_by_object_next_steps(plate_index);
        if (effective_vase(before) && !effective_vase(after))
            for (NextStep& step : vase_left_behind_steps(plate))
                steps.push_back(std::move(step));
        add_next_steps(answer, steps);
        answer = guard.report(std::move(answer));
    } else {
        answer["changed_settings"] = nlohmann::json::array();
    }
    const PlateSettings now        = plate_settings_of(plate);
    answer["settings"]             = plate_settings_json(now);
    answer["effective"]            = effective_json(now);
    answer["current_plate_index"]  = plates.get_curr_plate_index();
    answer["active_warnings"]      = get_active_warnings_json(plater);
    return answer;
}

} // namespace

namespace Slic3r { namespace GUI { namespace OrcaMCP {

nlohmann::json plate_settings_entry_json(PartPlate& plate)
{
    const PlateSettings settings = plate_settings_of(plate);
    return {{"settings", plate_settings_json(settings)}, {"effective", effective_json(settings)}};
}

}}} // namespace Slic3r::GUI::OrcaMCP

void Slic3r::GUI::OrcaMCPServer::register_plate_tools()
{
    const nlohmann::json order_items = {{"type", "integer"}, {"minimum", 1}};
    register_tool({
        "set_plate_settings",
        ToolCategory::Plates,
        "Change a plate's settings, name or lock",
        "Change a plate's own settings, as its settings dialog, name editor and lock icon do: bed_type, print_sequence, the filament "
        "order of the first layer and of later layer ranges, spiral_vase, name and locked (a locked plate's objects stay put when "
        "arranging). Only what the call gives changes, in one undo step; the answer's settings are in this tool's argument shape "
        "(sent back, they change nothing), effective says what applies. \"global\" follows the printer's and print settings. A "
        "plate whose slicing settings change is made the current plate first, as the dialog does. spiral_vase \"on\" is the dialog's "
        "Enable answered Yes: every object on the plate gets the vase's object settings (one wall, no top shell, no infill, no "
        "support), which are the object's, so its copies on other plates print with them too (vase_settings_applied lists them); "
        "\"off\" and \"global\" leave those settings on the objects. A plate's own bed_type is offered for Bambu Lab printers only "
        "(elsewhere the plate follows the global one, apply_config's project curr_bed_type). The same settings again: changed "
        "false, no undo step; a value the plate already has is never refused. Refused while an arrange, orient or bed fill runs "
        "(an arrange of every plate locks and unlocks plates as it runs).",
        {{"type", "object"},
         {"properties",
          {{"plate_index", {{"type", "integer"}, {"minimum", 0}, {"description", "Plate index (0-based). Default: the current plate"}}},
           {"name", {{"type", "string"}, {"description", "The plate's name, up to 250 characters; \"\" for none"}}},
           {"locked", {{"type", "boolean"}, {"description", "Lock the plate: arranging and auto-orient leave its objects where they are"}}},
           {"bed_type",
            {{"type", "string"},
             {"description", "\"global\", or a bed type the printer offers, as the config spells it: \"Cool Plate\", \"Engineering Plate\", "
                             "\"High Temp Plate\", \"Textured PEI Plate\", \"Textured Cool Plate\", \"Supertack Plate\""}}},
           {"print_sequence", {{"type", "string"}, {"enum", {"global", "by layer", "by object"}}, {"description", "Print the plate by layer or by object"}}},
           {"first_layer_filament_order",
            {{"type", {"string", "array"}},
             {"items", order_items},
             {"description", "\"auto\", or every filament number (1-based) once, in the order the first layer prints them"}}},
           {"other_layers_filament_order",
            {{"type", {"string", "array"}},
             {"items",
              {{"type", "object"},
               {"properties",
                {{"from_layer", {{"type", "integer"}, {"minimum", 2}, {"description", "First layer of the range, 2 or more"}}},
                 {"to_layer", {{"type", {"integer", "null"}}, {"description", "Last layer of the range; null or left out: the last layer"}}},
                 {"order", {{"type", "array"}, {"items", order_items}, {"description", "Every filament number once, in printing order"}}}}},
               {"required", {"from_layer", "order"}},
               {"additionalProperties", false}}},
             {"description", "\"auto\", or layer ranges from layer 2 on, each with every filament number once in printing order; ranges "
                             "may not share a layer"}}},
           {"spiral_vase",
            {{"type", "string"},
             {"enum", {"global", "on", "off"}},
             {"description", "\"on\" also gives every object on the plate the vase's object settings (see the description); \"global\" "
                             "follows the print settings"}}}}},
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([&]() -> nlohmann::json { return set_plate_settings_on_main_thread(params); });
        }});
}
