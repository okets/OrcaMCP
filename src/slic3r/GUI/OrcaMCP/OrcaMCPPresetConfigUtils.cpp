#include "OrcaMCPPresetConfigUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Jobs/OrientJob.hpp"

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

void OrcaMCPPresetConfigUtils::ApplyConfig(const nlohmann::json& item) {
    std::string type = item.value("type", "");
    Preset::Type preset_type;
    if (type == "print") {
        preset_type = Preset::Type::TYPE_PRINT;
    } else if (type == "filament") {
        preset_type = Preset::Type::TYPE_FILAMENT;
    } else if (type == "printer") {
        preset_type = Preset::Type::TYPE_PRINTER;
    } else {
        std::string error_message = "ApplyConfig: invalid type parameter: " + type;
        BOOST_LOG_TRIVIAL(error) << error_message;
        return;
    }

    Tab* tab = wxGetApp().get_tab(preset_type);
    if (tab != nullptr) {
        try {
            DynamicPrintConfig* config = tab->get_config();
            if (!config) return;

            ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Enable);
            const std::string value_str = item["value"].is_string() ? // Can't blindly dump json object to string, otherwise the original string will become "\"value\""
                item["value"].get<std::string>() : item["value"].dump();
            config->set_deserialize(item.value("key", ""), value_str, context);
        } catch (const std::exception& e) {
            std::string error_message = "ApplyConfig: '" + item.value("key", "") + ":" + item["value"].dump() + "' failed: " + e.what();
            BOOST_LOG_TRIVIAL(error) << error_message;
            return;
        }
    }
}

void OrcaMCPPresetConfigUtils::SelectPreset(const std::string& type, const std::string& presetName) {
    Preset::Type preset_type;
    if (type == "print") {
        preset_type = Preset::Type::TYPE_PRINT;
    } else if (type == "filament") {
        preset_type = Preset::Type::TYPE_FILAMENT;
    } else if (type == "printer") {
        preset_type = Preset::Type::TYPE_PRINTER;
    } else {
        BOOST_LOG_TRIVIAL(error) << "SelectPreset: invalid type parameter";
        throw std::runtime_error("Invalid type parameter");
    }

    DiscardCurrentPresetChanges(); // Selecting a printer will result in selecting a filament or print preset. So we need to discard changes for all presets in order not to have the "transfer or discard" dialog pop up

    Tab* tab = Slic3r::GUI::wxGetApp().get_tab(preset_type);
    if (tab != nullptr) {
        tab->select_preset(presetName, false, std::string(), false);
    }
}

}} // namespace Slic3r::GUI
