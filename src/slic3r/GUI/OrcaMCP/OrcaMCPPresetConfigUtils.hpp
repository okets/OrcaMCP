#ifndef slic3r_GUI_OrcaMCPPresetConfigUtils_hpp_
#define slic3r_GUI_OrcaMCPPresetConfigUtils_hpp_

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include "libslic3r/Preset.hpp"

namespace Slic3r { namespace GUI {

// Result of applying a batch of settings for one {type, settings} item.
// `error` is non-empty only for a structural failure (unknown type / no tab);
// per-key failures are reported via `invalid` instead of aborting the whole item.
struct ApplyConfigResult {
    std::vector<std::string> applied;
    std::vector<std::string> invalid;
    std::string error;
};

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
    // item = {"type": "print"|"filament"|"printer"|"project", "settings": {key: value, ...}}
    static ApplyConfigResult ApplyConfig(const nlohmann::json& item);
    // Refreshes derived UI/state after a direct write to preset_bundle->project_config
    // (filament colors, dynamic/mixed filament lists, project-dirty flag, background process).
    static void RefreshAfterProjectConfigChange();
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