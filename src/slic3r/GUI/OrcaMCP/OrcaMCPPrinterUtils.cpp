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

std::string serialized_host_type(const DynamicPrintConfig& config)
{
    return config.has("host_type") ? config.opt_serialize("host_type") : std::string();
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

// The response shared by add_physical_printer and select_printer: whatever the edited preset now is.
nlohmann::json print_host_response(const std::string& name)
{
    const DynamicPrintConfig& config = edited_printer_config();
    return {{"status", "success"},
            {"physical_printer", name},
            {"printer_preset", name},
            {"host_type", serialized_host_type(config)},
            {"print_host", config_string(config, "print_host")}};
}

} // namespace

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
    host_type_name = serialized_host_type(out);
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
            {"host_type", serialized_host_type(config)},
            {"has_credentials", !config_string(config, "flashforge_serial_number").empty() &&
                                    !config_string(config, "printhost_apikey").empty()},
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

std::string save_print_host_preset(const std::string& name,
                                   const std::string& host,
                                   const std::string& host_type,
                                   const std::string& serial_number,
                                   const std::string& api_key,
                                   const std::string& printer_preset)
{
    PrinterPresetCollection& printers = wxGetApp().preset_bundle->printers;

    const std::string base_name = printer_preset.empty() ? printers.get_edited_preset().name : printer_preset;
    if (printers.find_preset(base_name, false) == nullptr)
        return "No printer preset named '" + base_name + "'";
    if (printers.get_edited_preset().name != base_name)
        OrcaMCPPresetConfigUtils::SelectPreset("printer", base_name);

    // Same settings PhysicalPrinterDialog writes into the edited printer config, through the shared
    // validation path.
    const nlohmann::json item = {
        {"type", "printer"},
        {"settings", {
            {"print_host", host},
            {"host_type", host_type},
            {"flashforge_serial_number", serial_number},
            {"printhost_apikey", api_key},
            {"printhost_authorization_type", "key"}
        }}
    };
    const ApplyConfigResult applied = OrcaMCPPresetConfigUtils::ApplyConfig(item);
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

    if (printers.get_edited_preset().name != name)
        return "Saving printer preset '" + name + "' did not take effect";
    return {};
}

}}} // namespace Slic3r::GUI::OrcaMCP
