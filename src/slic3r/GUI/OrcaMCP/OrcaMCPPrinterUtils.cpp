// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.cpp
#include "OrcaMCPPrinterUtils.hpp"
#include "OrcaMCPCommon.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// Orca neither exports the physical printer selection (PresetBundle::export_selections has the line
// commented out) nor keeps a value written into its own [presets] section across a shutdown, so the MCP
// tools remember the selection in a section of their own.
constexpr const char* kSelectionSection = "orcamcp";
constexpr const char* kSelectionKey     = "physical_printer";

// Where PhysicalPrinterCollection::load_printers/save_printer keep the printer files.
constexpr const char* kPrintersSubdir = "physical_printer";

void remember_selection()
{
    PhysicalPrinterCollection& printers = wxGetApp().preset_bundle->physical_printers;
    wxGetApp().app_config->set(kSelectionSection, kSelectionKey,
                               printers.has_selection() ? printers.get_selected_full_printer_name() : std::string());
    wxGetApp().app_config->save();
}

// Re-select the printer remembered by a previous session, but only when its preset is the one already
// selected in the printer tab, so that restoring never changes the user's visible printer profile.
void restore_selection()
{
    PresetBundle*              bundle   = wxGetApp().preset_bundle;
    PhysicalPrinterCollection& printers = bundle->physical_printers;
    if (printers.has_selection())
        return;

    const std::string full_name = wxGetApp().app_config->get(kSelectionSection, kSelectionKey);
    if (full_name.empty())
        return;

    const PhysicalPrinter* printer = printers.find_printer(PhysicalPrinter::get_short_name(full_name));
    if (printer == nullptr)
        return;

    const std::string& edited_preset = bundle->printers.get_edited_preset().name;
    if (printer->get_preset_names().count(edited_preset) == 0)
        return;

    printers.select_printer(printer->name, edited_preset);
}

std::string config_string(const DynamicPrintConfig* config, const std::string& key)
{
    return (config != nullptr && config->has(key)) ? config->opt_string(key) : std::string();
}

std::string serialized_host_type(const DynamicPrintConfig* config)
{
    return (config != nullptr && config->has("host_type")) ? config->opt_serialize("host_type") : std::string();
}

nlohmann::json temperature_json(double current, double target)
{
    return {{"current", current}, {"target", target}};
}

} // namespace

void ensure_physical_printers_loaded()
{
    static bool s_loaded = false;
    if (s_loaded)
        return;

    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;
    s_loaded = true;

    PresetsConfigSubstitutions substitutions;
    try {
        // ConfigBase::save_to_json fails silently when the directory is missing, so create it up front.
        boost::filesystem::create_directories(boost::filesystem::path(data_dir()) / kPrintersSubdir);
        bundle->physical_printers.load_printers(data_dir(), kPrintersSubdir, substitutions,
                                                ForwardCompatibilitySubstitutionRule::EnableSilent);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaMCP: failed to load physical printers: " << e.what();
    }
    restore_selection();
}

bool resolve_print_host_config(DynamicPrintConfig& out, std::string& host_type_name, std::string& error)
{
    ensure_physical_printers_loaded();

    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr) {
        error = "Preset bundle not available";
        return false;
    }

    // Start from the full printer preset so that options outside PhysicalPrinter::printer_options()
    // (gcode_flavor, printer_technology, ...) are present, then let the physical printer override the
    // print host settings.
    out = bundle->printers.get_edited_preset().config;
    if (const DynamicPrintConfig* physical = bundle->physical_printers.get_selected_printer_config())
        out.apply_only(*physical, PhysicalPrinter::printer_options(), true);

    if (config_string(&out, "print_host").empty()) {
        error = "No print host configured. Use add_physical_printer first.";
        return false;
    }
    host_type_name = serialized_host_type(&out);
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

nlohmann::json select_physical_printer(const std::string& name)
{
    ensure_physical_printers_loaded();

    PresetBundle*              bundle   = wxGetApp().preset_bundle;
    PhysicalPrinterCollection& printers = bundle->physical_printers;

    // Mirrors PresetComboBox::selection_is_changed_according_to_physical_printers().
    std::string old_full_name;
    std::string old_preset_name;
    if (printers.has_selection()) {
        old_full_name   = printers.get_selected_full_printer_name();
        old_preset_name = printers.get_selected_printer_preset_name();
    } else {
        old_preset_name = bundle->printers.get_edited_preset().name;
    }

    // select_printer() unselects when the name is unknown, so check before touching the selection.
    if (printers.find_printer(PhysicalPrinter::get_short_name(name)) == nullptr)
        return {{"status", "error"},
                {"message", "No physical printer named '" + name + "'. Use get_printers to list them."}};
    printers.select_printer(name);

    const std::string preset_name = printers.get_selected_printer_preset_name();

    McpDialogSuppressionGuard suppression;
    Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (old_preset_name == preset_name) {
        if (tab != nullptr)
            tab->update_preset_choice();
        wxGetApp().sidebar().update_presets(Preset::TYPE_PRINTER);
        bundle->export_selections(*wxGetApp().app_config);
    } else if (tab != nullptr) {
        tab->select_preset(preset_name, false, old_full_name);
    }

    if (!printers.has_selection())
        return {{"status", "error"},
                {"message", "Selecting physical printer '" + name + "' was reverted while switching printer preset '" +
                                preset_name + "'"}};

    remember_selection();

    const DynamicPrintConfig* config = printers.get_selected_printer_config();
    return {{"status", "success"},
            {"physical_printer", printers.get_selected_printer_name()},
            {"printer_preset", preset_name},
            {"host_type", serialized_host_type(config)},
            {"print_host", config_string(config, "print_host")}};
}

std::string upsert_physical_printer(const std::string& name,
                                    const std::string& host,
                                    const std::string& host_type,
                                    const std::string& serial_number,
                                    const std::string& api_key,
                                    const std::string& printer_preset)
{
    ensure_physical_printers_loaded();

    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return "Preset bundle not available";

    const t_config_enum_values& host_types = ConfigOptionEnum<PrintHostType>::get_enum_values();
    const auto                  host_type_it = host_types.find(host_type);
    if (host_type_it == host_types.end())
        return "Unknown host_type '" + host_type + "'";

    const std::string preset_name = printer_preset.empty() ? bundle->printers.get_edited_preset().name : printer_preset;
    const Preset*     preset      = bundle->printers.find_preset(preset_name, false);
    if (preset == nullptr)
        return "No printer preset named '" + preset_name + "'";

    PhysicalPrinter printer(name, bundle->physical_printers.default_config(), *preset);
    printer.config.set_key_value("print_host", new ConfigOptionString(host));
    printer.config.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(static_cast<PrintHostType>(host_type_it->second)));
    printer.config.set_key_value("printhost_apikey", new ConfigOptionString(api_key));
    printer.config.set_key_value("flashforge_serial_number", new ConfigOptionString(serial_number));
    printer.config.set_key_value("printhost_authorization_type", new ConfigOptionEnum<AuthorizationType>(atKeyPassword));

    bundle->physical_printers.save_printer(printer);
    return {};
}

}}} // namespace Slic3r::GUI::OrcaMCP
