#include "OrcaMCPPresetConfigUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Jobs/OrientJob.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace GUI {

nlohmann::json OrcaMCPPresetConfigUtils::PresetsToJson(const std::vector<std::pair<const Preset*, bool>>& presets)
{
    nlohmann::json j_array = nlohmann::json::array();
    for (const auto& [preset, is_selected] : presets) {
        j_array.push_back(PresetToJson(preset, is_selected));
    }
    return j_array;
}

nlohmann::json OrcaMCPPresetConfigUtils::PresetToJson(const Preset* preset, bool is_selected)
{
    nlohmann::json j;
    j["name"] = preset->name;
    j["is_default"] = preset->is_default;
    j["is_selected"] = is_selected;

    // Serialize config keys and values
    nlohmann::json config_json = nlohmann::json::object();
    const DynamicPrintConfig& config = preset->config;
    for (const std::string& key : config.keys()) {
        config_json[key] = config.opt_serialize(key);
    }
    j["config"] = config_json;
    j["version"] = preset->version.to_string();
    return j;
}

nlohmann::json OrcaMCPPresetConfigUtils::GetPresetsJson(Preset::Type type) {
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

    return PresetsToJson(presets);
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

    nlohmann::json j = PresetToJson(&presets->get_edited_preset(), true); // is_selected is true because it is the edited preset

    const bool deep_compare = (type == Preset::TYPE_PRINTER || type == Preset::TYPE_SLA_MATERIAL);
    j["dirty_options"] = presets->current_dirty_options(deep_compare);

    return j;
}


nlohmann::json OrcaMCPPresetConfigUtils::GetAllPresetJson() {
    nlohmann::json printerPresetsJson = GetPresetsJson(Preset::Type::TYPE_PRINTER);
    nlohmann::json filamentPresetsJson = GetPresetsJson(Preset::Type::TYPE_FILAMENT);
    nlohmann::json printPresetsJson = GetPresetsJson(Preset::Type::TYPE_PRINT);

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

    ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Enable);
    for (auto& [key, value] : item.at("settings").items()) {
        // Can't blindly dump json object to string, otherwise the original string will become "\"value\""
        const std::string value_str = value.is_string() ? value.get<std::string>() : value.dump();
        if (print_config_def.get(key) == nullptr) {
            result.invalid.push_back(key);
            continue;
        }
        try {
            config->set_deserialize(key, value_str, context);
            result.applied.push_back(key);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "ApplyConfig: '" << key << ":" << value_str << "' failed: " << e.what();
            result.invalid.push_back(key);
        }
    }

    if (type == "project") {
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
    wxPostEvent(&wxGetApp().sidebar(), SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, &wxGetApp().sidebar()));
}

void OrcaMCPPresetConfigUtils::SelectPreset(const std::string& type, const std::string& presetName) {
    Preset::Type preset_type = GetPresetTypeFromString(type);

    DiscardCurrentPresetChanges(); // Selecting a printer will result in selecting a filament or print preset. So we need to discard changes for all presets in order not to have the "transfer or discard" dialog pop up

    Tab* tab = Slic3r::GUI::wxGetApp().get_tab(preset_type);
    if (tab != nullptr) {
        tab->select_preset(presetName, false, std::string(), false);
    }
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
