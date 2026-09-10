#ifndef slic3r_GUI_OrcaMCPPresetConfigUtils_hpp_
#define slic3r_GUI_OrcaMCPPresetConfigUtils_hpp_

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace GUI {

// What get_presets was asked for. The full preset list with every config key is ~1.9 MB, which no
// MCP client can take in one response, so a query narrows it: `summary` (the default) drops the
// per-preset config blob and keeps the identifying fields, and the filters cut the list down to
// what the caller is actually looking for.
struct PresetQuery
{
    std::string vendor;        // case-insensitive substring of the preset's vendor, empty = any
    std::string name_contains; // case-insensitive substring of the preset name, empty = any
    bool        summary = true;
};

// Pure: the two text filters, applied the way a person means them (case-insensitive "contains").
// An empty filter matches everything, including a preset with no vendor at all.
bool preset_query_matches(const std::string& name, const std::string& vendor, const PresetQuery& query);

// One JSON value from a tool call, turned into the text ConfigOption::deserialize expects.
struct ConfigValueText
{
    bool        ok = false;
    std::string text;    // meaningful only when ok
    std::string reason;  // meaningful only when !ok
};

// get_valid_config_keys advertises list-typed keys as "strings" / "ints" / "bools" / "floats", so
// an array is the obvious thing for a caller to send -- but dumping the array and handing the
// literal text to deserialize does not fail loudly. ConfigOptionStrings stores the whole "[...]"
// as one string (Config.cpp:149 finds no ';'), and ConfigOptionFloats stores 0 for the element
// carrying the '[' and returns true anyway (Config.hpp:935). Both are silent corruption.
//
// So the shape is decided here, from the option's declared type, before anything is written. The
// separator is not one separator: ConfigOptionStrings splits on ';' (via unescape_strings_cstyle),
// while ConfigOptionInts (Config.hpp:1089), ConfigOptionFloats (Config.hpp:911) and
// ConfigOptionBools (Config.hpp:1959) split on ','.
ConfigValueText config_value_to_string(const nlohmann::json& value, ConfigOptionType type);

// What a caller should have sent for an option of this type, phrased for an error message.
std::string config_value_expected_shape(ConfigOptionType type);

// Result of applying a batch of settings for one {type, settings} item.
// `error` is non-empty only for a structural failure (unknown type / no tab);
// per-key failures are reported via `invalid` instead of aborting the whole item.
struct ApplyConfigResult {
    std::vector<std::string> applied;
    std::vector<std::string> invalid;
    std::string error;
    // Writing filament_colour gives each changed slot the colour picker's three-key treatment, and
    // that replaces a gradient with one flat colour. `flattened_slots` holds the 1-based slots that
    // happened to, so the caller can say what it did instead of the spool quietly losing its second
    // colour; `color_errors` holds the reason for any slot whose three keys could not be written.
    std::vector<int> flattened_slots;
    std::vector<std::string> color_errors;
};

class OrcaMCPPresetConfigUtils {
public:
    static nlohmann::json PresetToJson(const Preset* preset, bool is_selected, const PresetQuery& query);
    static nlohmann::json PresetsToJson(const std::vector<std::pair<const Preset*, bool>>& presets,
                                        const PresetQuery& query);
    // Only presets the tab's combo box lists, i.e. the visible ones compatible with the selected
    // printer, filtered by `query`.
    static nlohmann::json GetPresetsJson(Preset::Type type, const PresetQuery& query = {});
    static nlohmann::json GetAllPresetJson(const PresetQuery& query = {});
    static nlohmann::json GetAllEditedPresetJson();
    static nlohmann::json GetEditedPresetJson(Preset::Type type);
    static void DiscardCurrentPresetChanges();
    static void UpdatePresetTabs();
    // item = {"type": "print"|"filament"|"printer"|"project", "settings": {key: value, ...}}
    static ApplyConfigResult ApplyConfig(const nlohmann::json& item);
    // Refreshes derived UI/state after a direct write to preset_bundle->project_config
    // (filament colors, dynamic/mixed filament lists, project-dirty flag, background process) and
    // persists it with export_selections.
    //
    // The persistence is not optional bookkeeping. Several project_config keys -- filament_colour,
    // filament_multi_colour, filament_colour_type, flush_volumes_matrix/vector, flush_multiplier
    // and the mixed-filament metadata -- live *only* in the per-printer app-config snapshot between
    // sessions (PresetBundle::export_selections / load_selections). A write that skips it looks
    // right until the next restart and then silently reverts, which is exactly the kind of stale
    // project this whole area exists to prevent. Every GUI path that writes one of those keys
    // (the filament colour picker, the wipe-tower dialog, Sidebar::auto_calc_flushing_volumes)
    // calls export_selections, so every MCP path must too.
    static void RefreshAfterProjectConfigChange();

    // Sets one filament slot's colour the way the sidebar's own colour picker does
    // (PlaterPresetComboBox::sync_colour_config): filament_colour, filament_multi_colour and
    // filament_colour_type together, then RefreshAfterProjectConfigChange(). `config_index` is the
    // 0-based project_config index, NOT a position in combos_filament() -- that list holds only the
    // physical slots, so the two differ as soon as a mixed slot exists.
    // `flattened` comes back true when the slot held a multi-colour (gradient) that this replaced
    // with one flat colour. Returns false with `error` set and nothing written on a bad index.
    static bool WriteProjectFilamentColor(size_t config_index, const std::string& color, bool& flattened,
                                          std::string& error);
    // The same write, without the refresh, for a caller changing several slots at once: stage each
    // slot, then call RefreshAfterProjectConfigChange() exactly once. Matching a four-slot material
    // station otherwise rebuilt both filament lists, re-evaluated the project's dirty state and
    // rewrote the app config once per slot, for one user action.
    //
    // That final call is not optional. export_selections lives inside it and is the only thing that
    // makes any of these three keys survive a restart, so a batch that skips it looks right until
    // the next launch and then silently reverts.
    static bool StageProjectFilamentColor(size_t config_index, const std::string& color, bool& flattened,
                                          std::string& error);
    static void SelectPreset(const std::string& type, const std::string& presetName);
    // Sets one filament slot (1-based) to `presetName`, mirroring the sidebar filament combo
    // (Plater::priv::on_select_preset's TYPE_FILAMENT branch) instead of the filament tab, so a
    // multi-filament printer can have each slot targeted individually. Validates the slot range
    // and that the preset exists as a filament preset compatible with the selected printer;
    // returns false with `error` set and nothing changed otherwise. Main thread only.
    static bool SelectFilamentSlotPreset(int slot, const std::string& presetName, std::string& error);

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