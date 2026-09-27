// src/slic3r/GUI/OrcaMCP/OrcaMCPConfigValues.cpp
#include "OrcaMCPConfigValues.hpp"

#include <algorithm>
#include <set>

#include "OrcaMCPConfigKeys.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// Where a setting lives: the `type` apply_config takes for it, and the group get_config_values
// reports it under.
enum class ConfigSource { print, filament, printer, project, none };

const char* source_name(ConfigSource source)
{
    switch (source) {
    case ConfigSource::print: return "print";
    case ConfigSource::filament: return "filament";
    case ConfigSource::printer: return "printer";
    case ConfigSource::project: return "project";
    case ConfigSource::none: break;
    }
    return "none";
}

bool carries(const DynamicPrintConfig* config, const std::string& key) { return config != nullptr && config->has(key); }

const DynamicPrintConfig* config_of(const Preset* preset) { return preset != nullptr ? &preset->config : nullptr; }

bool one_of(const std::vector<std::string>& options, const std::string& key)
{
    return std::find(options.begin(), options.end(), key) != options.end();
}

// A key's group, by the preset type that defines it, so a slot whose preset is gone does not hide
// every filament key. The project keys first (filament_colour is the plate's, not a filament preset's
// default), then anything else the project holds. A key in two types' lists (inherits,
// compatible_printers) is read from the first.
ConfigSource source_of(const ConfigSources& sources, const std::string& key)
{
    if (project_keys.count(key) != 0)
        return ConfigSource::project;
    if (one_of(Preset::print_options(), key))
        return ConfigSource::print;
    if (one_of(Preset::filament_options(), key))
        return ConfigSource::filament;
    if (one_of(Preset::printer_options(), key))
        return ConfigSource::printer;
    if (carries(sources.project, key))
        return ConfigSource::project;
    return ConfigSource::none;
}

nlohmann::json value_or_null(const DynamicPrintConfig* config, const std::string& key)
{
    return carries(config, key) ? nlohmann::json(reported_config_value(*config, key)) : nlohmann::json(nullptr);
}

// The keys that differ between a preset and its saved version, as the GUI marks them
// (PresetCollection::dirty_options, compared on the raw values: a changed credential counts too).
std::set<std::string> dirty_keys(const PresetConfigs& preset)
{
    if (preset.edited == nullptr || preset.saved == nullptr || preset.edited == preset.saved)
        return {};
    const std::vector<std::string> keys = PresetCollection::dirty_options(preset.edited, preset.saved, /*deep_compare=*/false);
    return {keys.begin(), keys.end()};
}

// The dirty keys of every selected preset, worked out once per call.
struct DirtyKeys
{
    std::set<std::string>              print;
    std::set<std::string>              printer;
    std::vector<std::set<std::string>> filaments; // per slot

    explicit DirtyKeys(const ConfigSources& sources) : print(dirty_keys(sources.print)), printer(dirty_keys(sources.printer))
    {
        for (const PresetConfigs& slot : sources.filaments)
            filaments.push_back(dirty_keys(slot));
    }

    // The 1-based slots whose filament differs from its saved preset for `key`.
    std::vector<int> slots(const std::string& key) const
    {
        std::vector<int> dirty;
        for (size_t i = 0; i < filaments.size(); ++i)
            if (filaments[i].count(key) != 0)
                dirty.push_back(int(i) + 1);
        return dirty;
    }
};

const PresetConfigs& preset_of(const ConfigSources& sources, ConfigSource source)
{
    return source == ConfigSource::printer ? sources.printer : sources.print;
}

nlohmann::json per_slot_values(const ConfigSources& sources, const std::string& key)
{
    nlohmann::json values = nlohmann::json::array();
    for (const PresetConfigs& slot : sources.filaments)
        values.push_back(value_or_null(config_of(slot.edited), key));
    return values;
}

nlohmann::json preset_json(const PresetConfigs& preset) { return {{"name", preset.name}, {"dirty", preset.dirty}}; }

// Every dirty key of `preset`, as {key: {value, saved}}.
void add_unsaved(nlohmann::json& group, const PresetConfigs& preset, const std::set<std::string>& dirty)
{
    for (const std::string& key : dirty)
        group[key] = {{"value", value_or_null(config_of(preset.edited), key)}, {"saved", value_or_null(config_of(preset.saved), key)}};
}

} // namespace

ConfigSources config_sources_from(const PresetBundle& bundle)
{
    const auto selected = [](const PresetCollection& collection) {
        const Preset& edited = collection.get_edited_preset();
        return PresetConfigs{edited.name, &edited, &collection.get_selected_preset(), collection.current_is_dirty()};
    };
    ConfigSources sources;
    sources.print   = selected(bundle.prints);
    sources.printer = selected(bundle.printers);
    sources.project = &bundle.project_config;
    // A slot shows its preset's saved config unless its preset is the one the Filament tab edits,
    // which is how PresetBundle::full_fff_config composes what a slice uses.
    const std::string& edited_filament = bundle.filaments.get_edited_preset().name;
    for (const std::string& name : bundle.filament_presets) {
        if (name == edited_filament)
            sources.filaments.push_back(selected(bundle.filaments));
        else if (const Preset* preset = bundle.filaments.find_preset(name, false))
            sources.filaments.push_back({name, preset, preset, false});
        else
            sources.filaments.push_back({name, nullptr, nullptr, false});
    }
    return sources;
}

std::string reported_config_value(const DynamicPrintConfig& config, const std::string& key)
{
    const std::string value = config.opt_serialize(key);
    if (Preset::is_print_host_secret_key(key))
        return value.empty() ? "" : "<redacted>";
    return value;
}

std::vector<std::string> unknown_config_keys(const std::vector<std::string>& keys)
{
    std::vector<std::string> unknown;
    for (const std::string& key : keys)
        if (print_config_def.get(key) == nullptr)
            unknown.push_back(key);
    return unknown;
}

nlohmann::json selected_presets_json(const ConfigSources& sources)
{
    nlohmann::json filaments = nlohmann::json::array();
    for (size_t i = 0; i < sources.filaments.size(); ++i)
        filaments.push_back({{"slot", int(i) + 1}, {"name", sources.filaments[i].name}, {"dirty", sources.filaments[i].dirty}});
    return {{"printer", preset_json(sources.printer)}, {"print", preset_json(sources.print)}, {"filaments", filaments}};
}

nlohmann::json config_values_json(const ConfigSources& sources, const std::vector<std::string>& keys, bool dirty_only)
{
    const DirtyKeys       dirty_keys(sources);
    nlohmann::json        values     = nlohmann::json::object();
    nlohmann::json        dirty      = nlohmann::json::object();
    nlohmann::json        missing    = nlohmann::json::array();
    nlohmann::json        not_judged = nlohmann::json::array();
    std::set<std::string> seen;
    for (const std::string& key : keys) {
        if (!seen.insert(key).second)
            continue;
        const ConfigSource source = source_of(sources, key);
        const char*        group  = source_name(source);
        switch (source) {
        case ConfigSource::none: missing.push_back(key); break;
        case ConfigSource::project:
            not_judged.push_back(key);
            if (!dirty_only)
                values[group][key] = value_or_null(sources.project, key);
            break;
        case ConfigSource::filament: {
            const std::vector<int> slots = dirty_keys.slots(key);
            if (dirty_only && slots.empty())
                break;
            values[group][key] = per_slot_values(sources, key);
            if (!slots.empty())
                dirty[group][key] = {{"slots", slots},
                                     {"saved", value_or_null(config_of(sources.filaments[size_t(slots.front() - 1)].saved), key)}};
            break;
        }
        case ConfigSource::print:
        case ConfigSource::printer: {
            const PresetConfigs& preset   = preset_of(sources, source);
            const bool           is_dirty = (source == ConfigSource::print ? dirty_keys.print : dirty_keys.printer).count(key) != 0;
            if (dirty_only && !is_dirty)
                break;
            values[group][key] = value_or_null(config_of(preset.edited), key);
            if (is_dirty)
                dirty[group][key] = {{"saved", value_or_null(config_of(preset.saved), key)}};
            break;
        }
        }
    }
    nlohmann::json result = {{"status", "success"}, {"values", values}};
    if (!dirty.empty())
        result["dirty"] = dirty;
    if (!not_judged.empty())
        result["not_judged"] = {{"keys", not_judged}, {"reason", "project settings have no saved preset to compare with"}};
    if (!missing.empty())
        result["not_in_presets"] = missing;
    return result;
}

nlohmann::json unsaved_changes_json(const ConfigSources& sources)
{
    const DirtyKeys dirty_keys(sources);
    nlohmann::json  changes = nlohmann::json::object();
    nlohmann::json  print   = nlohmann::json::object();
    add_unsaved(print, sources.print, dirty_keys.print);
    nlohmann::json printer = nlohmann::json::object();
    add_unsaved(printer, sources.printer, dirty_keys.printer);

    // One entry per key, however many slots share the edited filament preset.
    nlohmann::json filament = nlohmann::json::object();
    for (size_t i = 0; i < sources.filaments.size(); ++i) {
        const PresetConfigs& slot = sources.filaments[i];
        for (const std::string& key : dirty_keys.filaments[i]) {
            if (filament.contains(key)) {
                filament[key]["slots"].push_back(int(i) + 1);
                continue;
            }
            filament[key] = {{"slots", nlohmann::json::array({int(i) + 1})},
                             {"value", value_or_null(config_of(slot.edited), key)},
                             {"saved", value_or_null(config_of(slot.saved), key)}};
        }
    }
    if (!print.empty())
        changes["print"] = print;
    if (!filament.empty())
        changes["filament"] = filament;
    if (!printer.empty())
        changes["printer"] = printer;
    return changes;
}

}}} // namespace Slic3r::GUI::OrcaMCP
