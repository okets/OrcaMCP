// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.cpp
#include "OrcaMCPPrinterUtils.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPresetConfigUtils.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <boost/algorithm/string/join.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

std::string config_string(const DynamicPrintConfig& config, const std::string& key)
{
    return config.has(key) ? config.opt_string(key) : std::string();
}

const DynamicPrintConfig& edited_printer_config()
{
    return wxGetApp().preset_bundle->printers.get_edited_preset().config;
}

// The preset's effective config: the selected preset may carry unsaved print host edits.
const DynamicPrintConfig& live_printer_config(const Preset& preset)
{
    const PrinterPresetCollection& printers = wxGetApp().preset_bundle->printers;
    return preset.name == printers.get_edited_preset().name ? edited_printer_config() : preset.config;
}

nlohmann::json temperature_json(double current, double target)
{
    return {{"current", current}, {"target", target}};
}

// The credentials a host type actually uses, so has_credentials is not Flashforge-only.
bool has_print_host_credentials(const DynamicPrintConfig& config)
{
    if (print_host_type_name(config) == "flashforge")
        return !config_string(config, "flashforge_serial_number").empty() &&
               !config_string(config, "printhost_apikey").empty();
    if (config.has("printhost_authorization_type") && config.opt_serialize("printhost_authorization_type") == "user")
        return !config_string(config, "printhost_user").empty() && !config_string(config, "printhost_password").empty();
    return !config_string(config, "printhost_apikey").empty();
}

// SavePresetDialog::Item::update's name rules. Tab::save_preset(from_input=true) bypasses that dialog's
// validation and save_current_preset then returns silently, so we must reject the same names it does.
std::string invalid_preset_name_reason(const PrinterPresetCollection& printers, const std::string& name)
{
    static const std::string illegal_characters = "<>[]:/\\|?*\"";

    if (name.empty())
        return "Preset name must not be empty";
    if (name.find_first_of(illegal_characters) != std::string::npos)
        return "Preset name '" + name + "' contains illegal characters: " + illegal_characters;
    if (name.find(PresetCollection::get_suffix_modified()) != std::string::npos)
        return "Preset name '" + name + "' must not contain '" + PresetCollection::get_suffix_modified() + "'";
    if (name.front() == ' ' || name.back() == ' ')
        return "Preset name '" + name + "' must not start or end with a space";
    if (name == "Default Setting" || name == "Default Printer" || name == PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER)
        return "Preset name '" + name + "' is reserved";
    if (printers.get_preset_name_by_alias(name) != name)
        return "Preset name '" + name + "' is a preset alias";

    const Preset* existing = printers.find_preset(name, false);
    if (existing != nullptr && !existing->can_overwrite())
        return "Cannot overwrite the system preset '" + name + "'";
    return {};
}

// Selecting a preset discards unsaved edits in every collection, so refuse to switch while any is dirty.
std::string dirty_preset_collections()
{
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    const std::vector<std::pair<std::string, const PresetCollection*>> collections = {
        {"print", &bundle->prints}, {"filament", &bundle->filaments}, {"printer", &bundle->printers}};

    std::vector<std::string> dirty;
    for (const auto& [label, presets] : collections)
        if (presets->current_is_dirty())
            dirty.push_back(label);
    return boost::algorithm::join(dirty, ", ");
}

// The response shared by add_physical_printer and select_printer: whatever the edited preset now is.
nlohmann::json print_host_response(const std::string& name)
{
    const DynamicPrintConfig& config = edited_printer_config();
    return {{"status", "success"},
            {"physical_printer", name},
            {"printer_preset", name},
            {"host_type", print_host_type_name(config)},
            {"print_host", config_string(config, "print_host")}};
}

} // namespace

std::string print_host_type_name(const DynamicPrintConfig& config)
{
    return config.has("host_type") ? config.opt_serialize("host_type") : std::string();
}

bool resolve_print_host_config(DynamicPrintConfig& out, std::string& host_type_name, std::string& error)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr) {
        error = "Preset bundle not available";
        return false;
    }

    // Exactly what Plater::send_gcode_legacy uses.
    out = bundle->printers.get_edited_preset().config;
    if (config_string(out, "print_host").empty()) {
        error = "No print host configured. Use add_physical_printer first.";
        return false;
    }
    host_type_name = print_host_type_name(out);
    return true;
}

std::unique_ptr<PrintHost> make_print_host(const DynamicPrintConfig& cfg)
{
    return std::unique_ptr<PrintHost>(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(&cfg)));
}

nlohmann::json status_to_json(const FlashforgeApi::PrinterStatus& s)
{
    nlohmann::json nozzles = nlohmann::json::array();
    for (size_t i = 0; i < s.nozzles.size(); ++i)
        nozzles.push_back({{"tool", static_cast<int>(i)},
                           {"current", s.nozzles[i].current},
                           {"target", s.nozzles[i].target}});

    nlohmann::json slots = nlohmann::json::array();
    for (const FlashforgeApi::MaterialSlot& slot : s.slots)
        slots.push_back({{"slot_id", slot.slot_id},
                         {"has_filament", slot.has_filament},
                         {"material", slot.material_name},
                         {"color", slot.material_color}});

    return {
        {"state", s.state},
        {"print_file", s.print_file},
        {"progress", s.progress},
        {"duration_s", s.duration_s},
        {"remaining_s", s.remaining_s},
        {"temperatures", {
            {"bed", temperature_json(s.bed_temp, s.bed_target)},
            {"chamber", temperature_json(s.chamber_temp, s.chamber_target)},
            {"nozzles", nozzles}
        }},
        {"light_on", s.light_on},
        {"door", s.door},
        {"error_code", s.error_code},
        {"name", s.name},
        {"model", s.model},
        {"firmware", s.firmware},
        {"camera_stream_url", s.camera_stream_url},
        {"material_station", {{"present", s.has_material_station}, {"slots", slots}}},
        {"raw", s.raw}
    };
}

nlohmann::json print_host_presets_json()
{
    const PrinterPresetCollection& printers      = wxGetApp().preset_bundle->printers;
    const std::string&             selected_name = printers.get_edited_preset().name;

    nlohmann::json presets = nlohmann::json::array();
    for (const Preset& preset : printers) {
        const DynamicPrintConfig& config = live_printer_config(preset);
        const std::string         host   = config_string(config, "print_host");
        if (host.empty())
            continue;

        presets.push_back({
            {"name", preset.name},
            {"print_host", host},
            {"host_type", print_host_type_name(config)},
            {"has_credentials", has_print_host_credentials(config)},
            {"is_selected", preset.name == selected_name}
        });
    }
    return presets;
}

nlohmann::json selected_print_host_preset_json()
{
    const PrinterPresetCollection& printers = wxGetApp().preset_bundle->printers;
    if (config_string(edited_printer_config(), "print_host").empty())
        return nullptr;
    return printers.get_edited_preset().name;
}

nlohmann::json select_print_host_preset(const std::string& name)
{
    PrinterPresetCollection& printers = wxGetApp().preset_bundle->printers;

    const Preset* preset = printers.find_preset(name, false);
    if (preset == nullptr)
        return {{"status", "error"}, {"message", "No printer preset named '" + name + "'"}};
    if (config_string(live_printer_config(*preset), "print_host").empty())
        return {{"status", "error"}, {"message", "Preset " + name + " has no print host configured"}};

    if (printers.get_edited_preset().name != name) {
        OrcaMCPPresetConfigUtils::SelectPreset("printer", name);
        if (printers.get_edited_preset().name != name)
            return {{"status", "error"}, {"message", "Failed to select printer preset '" + name + "'"}};
    }

    return print_host_response(name);
}

std::string save_print_host_preset(const std::string&                name,
                                   const std::string&                host,
                                   const std::string&                host_type,
                                   const std::optional<std::string>& serial_number,
                                   const std::optional<std::string>& api_key,
                                   const std::string&                printer_preset)
{
    PrinterPresetCollection& printers = wxGetApp().preset_bundle->printers;

    const std::string name_error = invalid_preset_name_reason(printers, name);
    if (!name_error.empty())
        return name_error;

    const std::string base_name = printer_preset.empty() ? printers.get_edited_preset().name : printer_preset;
    if (printers.find_preset(base_name, false) == nullptr)
        return "No printer preset named '" + base_name + "'";
    if (printers.get_edited_preset().name != base_name) {
        const std::string dirty = dirty_preset_collections();
        if (!dirty.empty())
            return "Unsaved preset changes in: " + dirty +
                   ". Save or reset them first (save_preset / reset_preset).";
        OrcaMCPPresetConfigUtils::SelectPreset("printer", base_name);
        if (printers.get_edited_preset().name != base_name)
            return "Failed to select printer preset '" + base_name + "'";
    }

    // Same settings PhysicalPrinterDialog writes into the edited printer config, through the shared
    // validation path. Credentials are written only when supplied, so updating the host of an existing
    // printer does not blank them.
    nlohmann::json settings = {
        {"print_host", host},
        {"host_type", host_type},
        {"printhost_authorization_type", "key"}
    };
    if (serial_number.has_value())
        settings["flashforge_serial_number"] = *serial_number;
    if (api_key.has_value())
        settings["printhost_apikey"] = *api_key;

    const ApplyConfigResult applied = OrcaMCPPresetConfigUtils::ApplyConfig({{"type", "printer"}, {"settings", settings}});
    if (!applied.error.empty())
        return applied.error;
    if (!applied.invalid.empty())
        return "Failed to apply print host settings: " + boost::algorithm::join(applied.invalid, ", ");
    OrcaMCPPresetConfigUtils::UpdatePresetTabs();

    // PhysicalPrinterDialog::OnOK: save the edited printer preset under the entered name.
    Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (tab == nullptr)
        return "Printer settings tab not available";
    tab->save_preset("", false, false, true, name);

    // save_current_preset returns silently when it refuses, so check the stored preset, not just the name.
    const Preset* saved = printers.find_preset(name, false);
    if (saved == nullptr || saved->is_system || config_string(saved->config, "print_host") != host)
        return "Saving printer preset '" + name + "' did not take effect";
    return {};
}

}}} // namespace Slic3r::GUI::OrcaMCP
