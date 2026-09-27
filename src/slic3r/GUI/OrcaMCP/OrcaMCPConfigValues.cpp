// src/slic3r/GUI/OrcaMCP/OrcaMCPConfigValues.cpp
#include "OrcaMCPConfigValues.hpp"

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

// The project keys first (filament_colour is the plate's, not a filament preset's default), then the
// presets in the order apply_config's types are usually meant, then anything else the project holds.
ConfigSource source_of(const ConfigSources& sources, const std::string& key)
{
    if (project_keys.count(key) != 0 && carries(sources.project, key))
        return ConfigSource::project;
    if (carries(sources.print.edited, key))
        return ConfigSource::print;
    if (!sources.filaments.empty() && carries(sources.filaments.front().edited, key))
        return ConfigSource::filament;
    if (carries(sources.printer.edited, key))
        return ConfigSource::printer;
    if (carries(sources.project, key))
        return ConfigSource::project;
    return ConfigSource::none;
}

nlohmann::json value_or_null(const DynamicPrintConfig* config, const std::string& key)
{
    return carries(config, key) ? nlohmann::json(reported_config_value(*config, key)) : nlohmann::json(nullptr);
}

// Whether `key` in the preset's edited config differs from its saved preset, judged as the GUI judges
// a preset dirty (PresetCollection::dirty_options): only a key both configs carry. The edited config
// can gain keys the saved preset never had (extruder_nozzle_stats, for one), and those are not edits.
bool differs(const PresetConfigs& preset, const std::string& key)
{
    if (!carries(preset.edited, key) || !carries(preset.saved, key))
        return false;
    return reported_config_value(*preset.edited, key) != reported_config_value(*preset.saved, key);
}

const PresetConfigs& preset_of(const ConfigSources& sources, ConfigSource source)
{
    return source == ConfigSource::printer ? sources.printer : sources.print;
}

// The 1-based slots whose filament differs from its saved preset for `key`.
std::vector<int> dirty_slots(const ConfigSources& sources, const std::string& key)
{
    std::vector<int> slots;
    for (size_t i = 0; i < sources.filaments.size(); ++i)
        if (differs(sources.filaments[i], key))
            slots.push_back(int(i) + 1);
    return slots;
}

nlohmann::json per_slot_values(const ConfigSources& sources, const std::string& key)
{
    nlohmann::json values = nlohmann::json::array();
    for (const PresetConfigs& slot : sources.filaments)
        values.push_back(value_or_null(slot.edited, key));
    return values;
}

nlohmann::json preset_json(const PresetConfigs& preset) { return {{"name", preset.name}, {"dirty", preset.dirty}}; }

// Every key of `preset` that differs from its saved preset, as {key: {value, saved}}.
void add_unsaved(nlohmann::json& group, const PresetConfigs& preset)
{
    if (preset.edited == nullptr)
        return;
    for (const std::string& key : preset.edited->keys())
        if (differs(preset, key))
            group[key] = {{"value", reported_config_value(*preset.edited, key)}, {"saved", value_or_null(preset.saved, key)}};
}

} // namespace

ConfigSources config_sources_from(const PresetBundle& bundle)
{
    const auto selected = [](const PresetCollection& collection) {
        const Preset& edited = collection.get_edited_preset();
        return PresetConfigs{edited.name, &edited.config, &collection.get_selected_preset().config, collection.current_is_dirty()};
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
            sources.filaments.push_back({name, &preset->config, &preset->config, false});
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
    nlohmann::json        values  = nlohmann::json::object();
    nlohmann::json        dirty   = nlohmann::json::object();
    nlohmann::json        missing = nlohmann::json::array();
    std::set<std::string> seen;
    for (const std::string& key : keys) {
        if (!seen.insert(key).second)
            continue;
        const ConfigSource source = source_of(sources, key);
        const char*        group  = source_name(source);
        switch (source) {
        case ConfigSource::none: missing.push_back(key); break;
        case ConfigSource::project:
            if (!dirty_only)
                values[group][key] = value_or_null(sources.project, key);
            break;
        case ConfigSource::filament: {
            const std::vector<int> slots = dirty_slots(sources, key);
            if (dirty_only && slots.empty())
                break;
            values[group][key] = per_slot_values(sources, key);
            if (!slots.empty())
                dirty[group][key] = {{"slots", slots}, {"saved", value_or_null(sources.filaments[size_t(slots.front() - 1)].saved, key)}};
            break;
        }
        case ConfigSource::print:
        case ConfigSource::printer: {
            const PresetConfigs& preset   = preset_of(sources, source);
            const bool           is_dirty = differs(preset, key);
            if (dirty_only && !is_dirty)
                break;
            values[group][key] = value_or_null(preset.edited, key);
            if (is_dirty)
                dirty[group][key] = {{"saved", value_or_null(preset.saved, key)}};
            break;
        }
        }
    }
    nlohmann::json result = {{"status", "success"}, {"values", values}};
    if (!dirty.empty())
        result["dirty"] = dirty;
    if (!missing.empty())
        result["not_in_presets"] = missing;
    return result;
}

nlohmann::json unsaved_changes_json(const ConfigSources& sources)
{
    nlohmann::json changes = nlohmann::json::object();
    nlohmann::json print   = nlohmann::json::object();
    add_unsaved(print, sources.print);
    nlohmann::json printer = nlohmann::json::object();
    add_unsaved(printer, sources.printer);

    // One entry per key, however many slots share the edited filament preset.
    nlohmann::json filament = nlohmann::json::object();
    for (size_t i = 0; i < sources.filaments.size(); ++i) {
        const PresetConfigs& slot = sources.filaments[i];
        if (slot.edited == nullptr)
            continue;
        for (const std::string& key : slot.edited->keys()) {
            if (!differs(slot, key))
                continue;
            if (filament.contains(key)) {
                filament[key]["slots"].push_back(int(i) + 1);
                continue;
            }
            filament[key] = {{"slots", nlohmann::json::array({int(i) + 1})},
                             {"value", reported_config_value(*slot.edited, key)},
                             {"saved", value_or_null(slot.saved, key)}};
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
