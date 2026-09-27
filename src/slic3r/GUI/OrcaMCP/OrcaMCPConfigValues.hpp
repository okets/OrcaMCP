// src/slic3r/GUI/OrcaMCP/OrcaMCPConfigValues.hpp
#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"

// get_config_values: which presets are selected, and the values of just the settings a caller names,
// small enough to read as often as it needs. get_edited_presets answers the same questions with every
// key of three presets, 25-48 KB. No wx: the core is unit-tested over plain configs in
// tests/slic3rutils/test_config_values.cpp.

namespace Slic3r {
class Preset;
class PresetBundle;
namespace GUI { namespace OrcaMCP {

// One selected preset: the preset slicing uses now (edited) and the saved one it came from. For a
// filament slot whose preset is not the one the Filament tab edits, both are that saved preset.
// Which keys differ between the two is PresetCollection::dirty_options' answer, the GUI's own.
struct PresetConfigs
{
    std::string   name;
    const Preset* edited = nullptr; // null for a slot whose preset no longer exists
    const Preset* saved  = nullptr;
    bool          dirty  = false;   // it has unsaved changes (PresetCollection::current_is_dirty)
};

// Every config a setting can be read from.
struct ConfigSources
{
    PresetConfigs              print;
    PresetConfigs              printer;
    std::vector<PresetConfigs> filaments; // one per filament slot, slot 1 first
    const DynamicPrintConfig*  project = nullptr;
};

// The selected presets of `bundle`, and its project config. The configs are the bundle's own, so the
// result is valid only while the bundle is unchanged: gather and read on the GUI thread, in one go.
ConfigSources config_sources_from(const PresetBundle& bundle);

// A key's value as MCP reports it: the slicer's own text (opt_serialize), which is what apply_config
// accepts back. A print-host credential is reported as "<redacted>" when set and "" when not, never
// by value: tool output ends up in transcripts and logs.
std::string reported_config_value(const DynamicPrintConfig& config, const std::string& key);

// The keys print_config_def does not know, in the order given: the same check apply_config makes
// before it writes a key.
std::vector<std::string> unknown_config_keys(const std::vector<std::string>& keys);

// {printer: {name, dirty}, print: {name, dirty}, filaments: [{slot, name, dirty}]}
nlohmann::json selected_presets_json(const ConfigSources& sources);

// The report for `keys`, every one known to print_config_def:
//   values: {print|filament|printer|project: {key: value}}, grouped by where each key lives, which is
//           the `type` apply_config takes for it. A filament key has one value per slot.
//   dirty:  {print|printer: {key: {saved}}, filament: {key: {slots, saved}}}, for the keys whose value
//           differs from the saved preset; for a credential {changed: true, secret: true} instead of
//           a saved value. Omitted when none does.
//   not_judged: {keys, reason} for project keys, which have no saved preset to compare with, so
//           neither "dirty" nor its absence says anything about them. Omitted when none was asked.
//   not_in_presets: known keys that belong to no preset type nor the project (an object-only key).
//           Omitted when empty.
// A key's group comes from its preset type (Preset::print_options and the others), not from whether
// one config happens to carry it. dirty_only keeps only the dirty keys; project keys it cannot judge
// are still listed under not_judged.
nlohmann::json config_values_json(const ConfigSources& sources, const std::vector<std::string>& keys, bool dirty_only);

// Every unsaved change: {print|printer: {key: {value, saved}}, filament: {key: {slots, value, saved}}};
// a credential's entry is {changed: true, secret: true}, with no value.
nlohmann::json unsaved_changes_json(const ConfigSources& sources);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
