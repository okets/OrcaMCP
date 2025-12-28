#ifndef slic3r_GUI_OrcaMCPPresetConfigUtils_hpp_
#define slic3r_GUI_OrcaMCPPresetConfigUtils_hpp_

#include <nlohmann/json.hpp>
#include "libslic3r/Preset.hpp"

namespace Slic3r { namespace GUI {

class OrcaMCPPresetConfigUtils {
public:
    static nlohmann::json PresetToJson(const Preset* preset, bool is_selected);
    static nlohmann::json PresetsToJson(const std::vector<std::pair<const Preset*, bool>>& presets);
    static nlohmann::json GetPresetsJson(Preset::Type type);
    static nlohmann::json GetAllPresetJson();
    static nlohmann::json GetAllEditedPresetJson();
    static nlohmann::json GetEditedPresetJson(Preset::Type type);
    static void DiscardCurrentPresetChanges();
    static void UpdatePresetTabs();
    static void ApplyConfig(const nlohmann::json& item);
    static void SelectPreset(const std::string& type, const std::string& presetName);

    // Preset management tools
    static void ClonePreset(const std::string& type, const std::string& sourceName, const std::string& newName);
    static void SavePreset(const std::string& type, const std::string& name = "");
    static void DeletePreset(const std::string& type, const std::string& name);
    static void ResetPreset(const std::string& type);

private:
    static Preset::Type GetPresetTypeFromString(const std::string& type);
    static PresetCollection* GetPresetCollection(Preset::Type type);
};

}} // namespace Slic3r::GUI

#endif