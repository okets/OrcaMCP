#include "OrcaMCPPresetConfigUtils.hpp"
#include "OrcaMCPCommon.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Jobs/OrientJob.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <cctype>
#include <memory>

namespace Slic3r { namespace GUI {

namespace {
// Defined below, next to WriteProjectFilamentColor, which is its other caller.
bool sync_filament_color_keys(size_t config_index, const std::string& color, bool& flattened, std::string& error);

// Every value of a colour-typed option must be a real "#RRGGBB" (or "#RRGGBBAA"); an empty value
// means "no colour assigned" and is allowed, as extruder_colour's own default is empty.
bool color_option_is_valid(const ConfigOption* option, std::string& bad_value)
{
    if (option == nullptr)
        return true;
    std::vector<std::string> values;
    if (const auto* strings = dynamic_cast<const ConfigOptionStrings*>(option))
        values = strings->values;
    else if (const auto* single = dynamic_cast<const ConfigOptionString*>(option))
        values.push_back(single->value);
    else
        return true; // not a string-shaped colour; nothing to check

    for (const std::string& value : values)
        if (!value.empty() && !OrcaMCP::is_hex_color(value, /*allow_alpha=*/true)) {
            bad_value = value;
            return false;
        }
    return true;
}
}

namespace {

std::string to_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// A preset's vendor: the vendor profile it came from for a system preset, and for filaments the
// filament_vendor config key, which is what a user preset derived from a vendor profile keeps.
std::string preset_vendor(const Preset* preset)
{
    if (preset->vendor != nullptr && !preset->vendor->name.empty())
        return preset->vendor->name;
    if (const auto* opt = preset->config.option<ConfigOptionStrings>("filament_vendor"))
        if (!opt->values.empty())
            return opt->values.front();
    return std::string();
}

// One config value, or "" when this preset type does not have that key.
std::string preset_config_string(const Preset* preset, const std::string& key)
{
    if (!preset->config.has(key))
        return std::string();
    return preset->config.opt_serialize(key);
}

} // namespace

bool preset_query_matches(const std::string& name, const std::string& vendor, const PresetQuery& query)
{
    if (!query.vendor.empty() && to_lower(vendor).find(to_lower(query.vendor)) == std::string::npos)
        return false;
    if (!query.name_contains.empty() && to_lower(name).find(to_lower(query.name_contains)) == std::string::npos)
        return false;
    return true;
}

nlohmann::json OrcaMCPPresetConfigUtils::PresetsToJson(const std::vector<std::pair<const Preset*, bool>>& presets,
                                                      const PresetQuery& query)
{
    nlohmann::json j_array = nlohmann::json::array();
    for (const auto& [preset, is_selected] : presets) {
        if (!preset_query_matches(preset->name, preset_vendor(preset), query))
            continue;
        j_array.push_back(PresetToJson(preset, is_selected, query));
    }
    return j_array;
}

nlohmann::json OrcaMCPPresetConfigUtils::PresetToJson(const Preset* preset, bool is_selected, const PresetQuery& query)
{
    nlohmann::json j;
    j["name"] = preset->name;
    j["is_default"] = preset->is_default;
    j["is_selected"] = is_selected;
    j["is_system"] = preset->is_system;
    j["vendor"] = preset_vendor(preset);
    j["version"] = preset->version.to_string();

    // The few keys that identify what a preset *is*, so "a PETG profile for this printer" can be
    // answered without the caller downloading every config key of every preset.
    const std::string filament_type = preset_config_string(preset, "filament_type");
    if (!filament_type.empty())
        j["filament_type"] = filament_type;
    const std::string printer_model = preset_config_string(preset, "printer_model");
    if (!printer_model.empty())
        j["printer_model"] = printer_model;

    if (!query.summary) {
        // Serialize config keys and values
        nlohmann::json config_json = nlohmann::json::object();
        const DynamicPrintConfig& config = preset->config;
        for (const std::string& key : config.keys()) {
            config_json[key] = config.opt_serialize(key);
        }
        j["config"] = config_json;
    }
    return j;
}

nlohmann::json OrcaMCPPresetConfigUtils::GetPresetsJson(Preset::Type type, const PresetQuery& query) {
    Tab* tab = wxGetApp().get_tab(type);
    if (!tab) {
        return nlohmann::json::array();
    }

    TabPresetComboBox* combo = tab->get_combo_box();
    std::vector<std::pair<const Preset*, bool>> presets;

    for (unsigned int i = 0; i < combo->GetCount(); i++) {
        std::string preset_name = combo->GetString(i).ToUTF8().data();

        if (preset_name.substr(0, 5) == "-----") continue;   // Skip separator

        // Orca Slicer adds "* " to the preset name to indicate that it has been modified
        if (preset_name.substr(0, 2) == "* ") {
            preset_name = preset_name.substr(2);
        }

        const Preset* preset = tab->m_presets->find_preset(preset_name, false);
        if (preset) {
            presets.push_back({preset, combo->GetSelection() == i});
        }
    }

    return PresetsToJson(presets, query);
}

nlohmann::json OrcaMCPPresetConfigUtils::GetEditedPresetJson(Preset::Type type) {
    Tab* tab = wxGetApp().get_tab(type);
    if (!tab) {
        return nlohmann::json::array();
    }
    PresetCollection* presets = tab->get_presets();
    if (!presets) {
        return nlohmann::json::array();
    }

    // The edited preset is one preset, and the caller asked about its values: always the full config.
    PresetQuery full;
    full.summary = false;
    nlohmann::json j = PresetToJson(&presets->get_edited_preset(), true, full); // is_selected is true because it is the edited preset

    const bool deep_compare = (type == Preset::TYPE_PRINTER || type == Preset::TYPE_SLA_MATERIAL);
    j["dirty_options"] = presets->current_dirty_options(deep_compare);

    return j;
}


nlohmann::json OrcaMCPPresetConfigUtils::GetAllPresetJson(const PresetQuery& query) {
    nlohmann::json printerPresetsJson = GetPresetsJson(Preset::Type::TYPE_PRINTER, query);
    nlohmann::json filamentPresetsJson = GetPresetsJson(Preset::Type::TYPE_FILAMENT, query);
    nlohmann::json printPresetsJson = GetPresetsJson(Preset::Type::TYPE_PRINT, query);

    return {
        {"printerPresets", printerPresetsJson},
        {"filamentPresets", filamentPresetsJson},
        {"printProcessPresets", printPresetsJson}
    };
}

nlohmann::json OrcaMCPPresetConfigUtils::GetAllEditedPresetJson() {
    nlohmann::json editedPrinterPresetJson = GetEditedPresetJson(Preset::Type::TYPE_PRINTER);
    nlohmann::json editedFilamentPresetJson = GetEditedPresetJson(Preset::Type::TYPE_FILAMENT);
    nlohmann::json editedPrintProcessPresetJson = GetEditedPresetJson(Preset::Type::TYPE_PRINT);

    return {
        {"editedPrinterPreset", editedPrinterPresetJson},
        {"editedFilamentPreset", editedFilamentPresetJson},
        {"editedPrintProcessPreset", editedPrintProcessPresetJson}
    };
}

void OrcaMCPPresetConfigUtils::DiscardCurrentPresetChanges() {
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle) {
        return;
    }
    bundle->printers.discard_current_changes();
    bundle->filaments.discard_current_changes();
    bundle->prints.discard_current_changes();
}

void OrcaMCPPresetConfigUtils::UpdatePresetTabs() {
    std::array<Preset::Type, 3> preset_types = {Preset::Type::TYPE_PRINT, Preset::Type::TYPE_FILAMENT, Preset::Type::TYPE_PRINTER};

    for (const auto& preset_type : preset_types) {
        if (Tab* tab = wxGetApp().get_tab(preset_type)) {
            tab->reload_config();
            tab->update();
            tab->update_dirty();
        }
    }
}

ApplyConfigResult OrcaMCPPresetConfigUtils::ApplyConfig(const nlohmann::json& item) {
    ApplyConfigResult result;
    const std::string type = item.value("type", "");
    DynamicPrintConfig* config = nullptr;

    if (type == "project") {
        config = &wxGetApp().preset_bundle->project_config;
    } else {
        Preset::Type preset_type;
        try {
            preset_type = GetPresetTypeFromString(type);
        } catch (const std::exception&) {
            result.error = "Unknown preset type: " + type;
            return result;
        }
        Tab* tab = wxGetApp().get_tab(preset_type);
        if (!tab) {
            result.error = "No tab for preset type: " + type;
            return result;
        }
        config = tab->get_config();
        if (!config) {
            result.error = "No config available for preset type: " + type;
            return result;
        }
    }

    // A caller writing filament_colour through apply_config gets the same three-key treatment the
    // colour picker gives -- but only for the slots whose colour actually moved, so a gradient set
    // deliberately on some other slot is not flattened as a side effect.
    std::vector<std::string> colors_before;
    if (type == "project")
        if (const auto* opt = config->option<ConfigOptionStrings>("filament_colour"))
            colors_before = opt->values;

    ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Enable);
    for (auto& [key, value] : item.at("settings").items()) {
        // Can't blindly dump json object to string, otherwise the original string will become "\"value\""
        const std::string value_str = value.is_string() ? value.get<std::string>() : value.dump();
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def == nullptr) {
            result.invalid.push_back(key);
            continue;
        }
        // A colour key deserializes any string at all, and upstream then decodes an unparseable one
        // as black -- so "B17C38" (no '#') would be accepted here and show up as a black spool.
        // Keep the old value and report the key instead.
        const bool is_color = def->gui_type == ConfigOptionDef::GUIType::color;
        std::unique_ptr<ConfigOption> previous;
        if (is_color && config->option(key) != nullptr)
            previous.reset(config->option(key)->clone());
        try {
            config->set_deserialize(key, value_str, context);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "ApplyConfig: '" << key << ":" << value_str << "' failed: " << e.what();
            result.invalid.push_back(key);
            continue;
        }
        std::string bad_color;
        if (is_color && !color_option_is_valid(config->option(key), bad_color)) {
            BOOST_LOG_TRIVIAL(error) << "ApplyConfig: '" << key << ":" << value_str << "' is not a #RRGGBB colour ("
                                     << bad_color << ")";
            if (previous)
                config->set_key_value(key, previous.release());
            result.invalid.push_back(key);
            continue;
        }
        result.applied.push_back(key);
    }

    if (type == "project") {
        if (const auto* opt = config->option<ConfigOptionStrings>("filament_colour"))
            for (size_t i = 0; i < opt->values.size(); ++i)
                if (i >= colors_before.size() || colors_before[i] != opt->values[i]) {
                    bool        flattened = false;
                    std::string sync_error;
                    sync_filament_color_keys(i, opt->values[i], flattened, sync_error);
                }
        RefreshAfterProjectConfigChange();
    }

    return result;
}

void OrcaMCPPresetConfigUtils::RefreshAfterProjectConfigChange() {
    Plater* plater = wxGetApp().plater();
    plater->update_filament_colors_in_full_config();
    wxGetApp().sidebar().update_dynamic_filament_list();
    wxGetApp().sidebar().update_mixed_filament_list();
    plater->update_project_dirty_from_presets();
    // Several project_config keys survive a restart only through the per-printer app-config
    // snapshot; see the header. Without this, an MCP write of a filament colour or a flush volume
    // is forgotten the next time OrcaSlicer starts.
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
    wxPostEvent(&wxGetApp().sidebar(), SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, &wxGetApp().sidebar()));
}

namespace {

// filament_colour alone is only a third of a colour: filament_multi_colour is what the swatch and
// the sync paths read, and filament_colour_type says whether the slot is a gradient. Writing one
// without the others leaves the sidebar showing a different colour from the preview. No refresh
// here -- the two callers each already do exactly one.
bool sync_filament_color_keys(size_t config_index, const std::string& color, bool& flattened, std::string& error)
{
    flattened = false;

    DynamicPrintConfig& project     = wxGetApp().preset_bundle->project_config;
    auto*               colour      = project.option<ConfigOptionStrings>("filament_colour");
    auto*               multi       = project.option<ConfigOptionStrings>("filament_multi_colour", true);
    auto*               colour_type = project.option<ConfigOptionStrings>("filament_colour_type", true);

    if (colour == nullptr || config_index >= colour->values.size()) {
        error = "Filament slot " + std::to_string(config_index + 1) + " has no colour entry in the project.";
        return false;
    }
    while (multi->values.size() <= config_index) multi->values.push_back(std::string());
    while (colour_type->values.size() <= config_index) colour_type->values.push_back("1");

    // "0" is the gradient/multi-colour type; a single flat colour replaces it, and the caller is
    // told so it can say what it did rather than quietly discarding a spool's second colour.
    flattened = colour_type->values[config_index] == "0";

    colour->values[config_index]      = color;
    multi->values[config_index]       = color;
    colour_type->values[config_index] = "1";
    return true;
}

} // namespace

bool OrcaMCPPresetConfigUtils::WriteProjectFilamentColor(size_t             config_index,
                                                         const std::string& color,
                                                         bool&              flattened,
                                                         std::string&       error)
{
    if (!sync_filament_color_keys(config_index, color, flattened, error))
        return false;
    RefreshAfterProjectConfigChange();
    return true;
}

void OrcaMCPPresetConfigUtils::SelectPreset(const std::string& type, const std::string& presetName) {
    Preset::Type preset_type = GetPresetTypeFromString(type);

    DiscardCurrentPresetChanges(); // Selecting a printer will result in selecting a filament or print preset. So we need to discard changes for all presets in order not to have the "transfer or discard" dialog pop up

    Tab* tab = Slic3r::GUI::wxGetApp().get_tab(preset_type);
    if (tab != nullptr) {
        tab->select_preset(presetName, false, std::string(), false);
    }
}

bool OrcaMCPPresetConfigUtils::SelectFilamentSlotPreset(int slot, const std::string& presetName, std::string& error)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    const int slot_count = int(bundle->filament_presets.size());
    if (slot < 1 || slot > slot_count) {
        error = "slot " + std::to_string(slot) + " out of range 1.." + std::to_string(slot_count);
        return false;
    }
    // Looking the name up in the filaments collection is what makes it "a filament preset";
    // a print/printer preset name simply is not found here.
    const Preset* preset = bundle->filaments.find_preset(presetName, false);
    if (preset == nullptr) {
        error = "Filament preset '" + presetName + "' not found";
        return false;
    }
    if (!preset->is_compatible) {
        error = "Filament preset '" + presetName + "' is not compatible with the selected printer '" +
                bundle->printers.get_selected_preset_name() + "'";
        return false;
    }

    // Everything below mirrors the TYPE_FILAMENT branch of Plater::priv::on_select_preset(),
    // i.e. what picking the preset in the sidebar's filament combo does. Deliberately not the
    // filament tab: the tab edits whichever single slot it happens to be on.
    const size_t idx = size_t(slot - 1);
    Plater*  plater  = wxGetApp().plater();
    Sidebar& sidebar = wxGetApp().sidebar();

    bundle->set_filament_preset(idx, presetName);
    plater->update_project_dirty_from_presets();
    bundle->export_selections(*wxGetApp().app_config);
    sidebar.update_dynamic_filament_list();
    plater->on_filament_change(idx);

    // combos_filament() holds only the physical slots, and each combo carries the project_config
    // index it edits -- so with a mixed slot present, position and index are not the same thing.
    for (PlaterPresetComboBox* combo : sidebar.combos_filament())
        if (combo != nullptr && combo->get_filament_idx() == int(idx)) {
            combo->update();
            break;
        }

    // A single-filament printer takes PresetBundle::full_fff_config()'s num_filaments <= 1
    // branch, which reads the filament tab's edited preset rather than filament_presets[0], so
    // there the tab has to follow the slot -- exactly what on_select_preset does when the
    // sidebar is not multi-filament.
    if (!sidebar.is_multifilament()) {
        if (Tab* tab = wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT))
            tab->select_preset(presetName, false, std::string(), false);
    }

    // Pushes the new filament config into the plater and reschedules the background process.
    plater->on_config_change(bundle->full_config());

    if (wxGetApp().app_config->get("auto_calculate_flush") == "all")
        sidebar.auto_calc_flushing_volumes(int(idx));

    for (PartPlate* plate : plater->get_partplate_list().get_plate_list())
        plate->update_slice_result_valid_state(false);

    return true;
}

Preset::Type OrcaMCPPresetConfigUtils::GetPresetTypeFromString(const std::string& type) {
    if (type == "print") {
        return Preset::Type::TYPE_PRINT;
    } else if (type == "filament") {
        return Preset::Type::TYPE_FILAMENT;
    } else if (type == "printer") {
        return Preset::Type::TYPE_PRINTER;
    } else {
        BOOST_LOG_TRIVIAL(error) << "GetPresetTypeFromString: invalid type parameter: " << type;
        throw std::runtime_error("Invalid preset type: " + type + ". Must be 'print', 'filament', or 'printer'.");
    }
}

PresetCollection* OrcaMCPPresetConfigUtils::GetPresetCollection(Preset::Type type) {
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle) {
        throw std::runtime_error("Preset bundle not available");
    }

    switch (type) {
        case Preset::Type::TYPE_PRINT:
            return &bundle->prints;
        case Preset::Type::TYPE_FILAMENT:
            return &bundle->filaments;
        case Preset::Type::TYPE_PRINTER:
            return &bundle->printers;
        default:
            throw std::runtime_error("Unsupported preset type");
    }
}

void OrcaMCPPresetConfigUtils::ClonePreset(const std::string& type, const std::string& sourceName, const std::string& newName) {
    Preset::Type preset_type = GetPresetTypeFromString(type);
    PresetCollection* presets = GetPresetCollection(preset_type);

    // Find source preset
    const Preset* source = presets->find_preset(sourceName, false);
    if (!source) {
        throw std::runtime_error("Source preset '" + sourceName + "' not found");
    }

    // Check if target name already exists
    if (presets->find_preset(newName, false)) {
        throw std::runtime_error("Preset '" + newName + "' already exists");
    }

    // Clone the preset
    std::vector<Preset const*> to_clone = { source };
    std::vector<std::string> failures;

    bool success = presets->clone_presets(to_clone, failures,
        [&newName](Preset& p, Preset::Type& t) {
            p.name = newName;
        });

    if (!success || !failures.empty()) {
        std::string error_msg = "Failed to clone preset";
        if (!failures.empty()) {
            error_msg += ": " + failures[0];
        }
        throw std::runtime_error(error_msg);
    }

    UpdatePresetTabs();
}

void OrcaMCPPresetConfigUtils::SavePreset(const std::string& type, const std::string& name) {
    Preset::Type preset_type = GetPresetTypeFromString(type);
    Tab* tab = wxGetApp().get_tab(preset_type);
    if (!tab) {
        throw std::runtime_error("Tab not found for type: " + type);
    }

    PresetCollection* presets = tab->get_presets();
    if (!presets) {
        throw std::runtime_error("Preset collection not available");
    }

    const Preset& edited = presets->get_edited_preset();

    // Determine the name to save as
    std::string save_name = name.empty() ? edited.name : name;

    // Check if we're trying to overwrite a system preset
    const Preset* existing = presets->find_preset(save_name, false);
    if (existing && (existing->is_system || existing->is_default)) {
        throw std::runtime_error("Cannot overwrite system preset '" + save_name + "'. Use a different name.");
    }

    // Save the preset
    presets->save_current_preset(save_name, false, false);

    UpdatePresetTabs();
}

void OrcaMCPPresetConfigUtils::DeletePreset(const std::string& type, const std::string& name) {
    Preset::Type preset_type = GetPresetTypeFromString(type);
    PresetCollection* presets = GetPresetCollection(preset_type);

    // Find the preset
    const Preset* preset = presets->find_preset(name, false);
    if (!preset) {
        throw std::runtime_error("Preset '" + name + "' not found");
    }

    // Cannot delete system or default presets
    if (preset->is_system || preset->is_default) {
        throw std::runtime_error("Cannot delete system preset '" + name + "'");
    }

    // Check if it's a base preset with dependent children
    bool has_dependents = false;
    for (const Preset& p : presets->get_presets()) {
        if (p.inherits() == name) {
            has_dependents = true;
            break;
        }
    }
    if (has_dependents) {
        throw std::runtime_error("Cannot delete preset '" + name + "' - it has dependent presets that inherit from it");
    }

    // Delete the preset
    bool success = presets->delete_preset(name);
    if (!success) {
        throw std::runtime_error("Failed to delete preset '" + name + "'");
    }

    UpdatePresetTabs();
}

void OrcaMCPPresetConfigUtils::ResetPreset(const std::string& type) {
    Preset::Type preset_type = GetPresetTypeFromString(type);
    Tab* tab = wxGetApp().get_tab(preset_type);
    if (!tab) {
        throw std::runtime_error("Tab not found for type: " + type);
    }

    PresetCollection* presets = tab->get_presets();
    if (!presets) {
        throw std::runtime_error("Preset collection not available");
    }

    // Check if there are unsaved changes
    const bool deep_compare = (preset_type == Preset::TYPE_PRINTER || preset_type == Preset::TYPE_SLA_MATERIAL);
    if (!presets->current_is_dirty()) {
        throw std::runtime_error("No unsaved changes to discard for " + type + " preset");
    }

    // Discard changes - revert to the selected preset's last saved state
    presets->discard_current_changes();

    // Update UI
    tab->reload_config();
    tab->update();
    tab->update_dirty();
}

}} // namespace Slic3r::GUI
