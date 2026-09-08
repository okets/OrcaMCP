// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.cpp
#include "OrcaMCPPrinterUtils.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPresetConfigUtils.hpp"

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PrintHostDialogs.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <algorithm>
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

// Builds the {toolId, slotId, materialName, toolMaterialColor, slotMaterialColor} entries the Flashforge
// local API expects, matching each project tool to the first free material-station slot of the same
// normalized material family (same rule as FlashforgePrintHostSendDialog::auto_assign_mappings, minus
// its colour tie-break between same-family slots).
nlohmann::json auto_material_mappings(const nlohmann::json& project_filaments, const std::vector<FlashforgeApi::MaterialSlot>& slots)
{
    nlohmann::json    mappings = nlohmann::json::array();
    std::vector<bool> slot_used(slots.size(), false);

    for (const auto& filament : project_filaments) {
        const std::string project_material = flashforge_normalize_material(filament.value("type", std::string()));
        if (project_material.empty())
            continue;

        for (size_t i = 0; i < slots.size(); ++i) {
            if (slot_used[i] || !slots[i].has_filament)
                continue;
            if (flashforge_normalize_material(slots[i].material_name) != project_material)
                continue;

            slot_used[i] = true;
            mappings.push_back({{"toolId", filament.value("tool_id", 0)},
                                {"slotId", slots[i].slot_id},
                                {"materialName", slots[i].material_name},
                                {"toolMaterialColor", filament.value("color", std::string())},
                                {"slotMaterialColor", slots[i].material_color}});
            break;
        }
    }
    return mappings;
}

// Builds the same payload shape from caller-supplied {tool_id, slot_id} pairs, filling in the slot's
// reported material/colour so the printer sees a payload consistent with the auto-mapped one. Only
// checks the *shape* of each entry (both fields present and integers) -- validate_material_mappings is
// the single place that checks the slot actually exists/is loaded and every project tool got mapped.
bool build_explicit_material_mappings(const nlohmann::json&                           requested,
                                      const std::vector<FlashforgeApi::MaterialSlot>& slots,
                                      nlohmann::json&                                 mappings_out,
                                      std::string&                                    error)
{
    mappings_out = nlohmann::json::array();
    for (const auto& entry : requested) {
        if (!entry.is_object() || !entry.contains("tool_id") || !entry.contains("slot_id") ||
            !entry.at("tool_id").is_number_integer() || !entry.at("slot_id").is_number_integer()) {
            error = "Each material_mappings entry requires integer 'tool_id' and 'slot_id'";
            return false;
        }

        const int  tool_id = entry.at("tool_id").get<int>();
        const int  slot_id = entry.at("slot_id").get<int>();
        const auto slot_it = std::find_if(slots.begin(), slots.end(),
                                          [&](const FlashforgeApi::MaterialSlot& s) { return s.slot_id == slot_id; });

        mappings_out.push_back({{"toolId", tool_id},
                                {"slotId", slot_id},
                                {"materialName", slot_it != slots.end() ? slot_it->material_name : std::string()},
                                {"toolMaterialColor", std::string()},
                                {"slotMaterialColor", slot_it != slots.end() ? slot_it->material_color : std::string()}});
    }
    return true;
}

// "[0,1]" -- tool ids as they read in a validation error.
std::string tool_id_list(const std::vector<int>& tool_ids)
{
    std::string list = "[";
    for (size_t i = 0; i < tool_ids.size(); ++i)
        list += (i == 0 ? "" : ",") + std::to_string(tool_ids[i]);
    return list + "]";
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
        {"material_station", {{"present", s.has_material_station}, {"slots", material_slots_json(s.slots)}}},
        {"raw", s.raw}
    };
}

nlohmann::json material_slots_json(const std::vector<FlashforgeApi::MaterialSlot>& slots)
{
    nlohmann::json slots_json = nlohmann::json::array();
    for (const FlashforgeApi::MaterialSlot& slot : slots)
        slots_json.push_back({{"slot_id", slot.slot_id},
                              {"has_filament", slot.has_filament},
                              {"material", slot.material_name},
                              {"color", slot.material_color}});
    return slots_json;
}

bool validate_material_mappings(const nlohmann::json&                           mappings,
                                const std::vector<FlashforgeApi::MaterialSlot>& slots,
                                const std::vector<int>&                         project_tool_ids,
                                std::string&                                    error,
                                std::vector<int>&                               unmapped_tools)
{
    unmapped_tools.clear();
    std::vector<bool> tool_mapped(project_tool_ids.size(), false);

    for (const auto& mapping : mappings) {
        // Slot validity first: a caller-supplied slot_id that does not exist (or is not currently
        // loaded) on the printer must never reach print_gcode_file, regardless of the tool_id it names.
        const int  slot_id = mapping.value("slotId", -1);
        const auto slot_it = std::find_if(slots.begin(), slots.end(),
                                          [&](const FlashforgeApi::MaterialSlot& s) { return s.slot_id == slot_id; });
        if (slot_it == slots.end() || !slot_it->has_filament) {
            error = "Slot " + std::to_string(slot_id) + " is not present/loaded on the printer";
            return false;
        }

        const int  tool_id = mapping.value("toolId", -1);
        const auto tool_it = std::find(project_tool_ids.begin(), project_tool_ids.end(), tool_id);
        if (tool_it == project_tool_ids.end()) {
            error = "Mapping references tool " + std::to_string(tool_id) + ", which is not one of the project's " +
                    tool_id_list(project_tool_ids) + " filament(s)";
            return false;
        }

        tool_mapped[std::distance(project_tool_ids.begin(), tool_it)] = true;
    }

    for (size_t i = 0; i < project_tool_ids.size(); ++i)
        if (!tool_mapped[i])
            unmapped_tools.push_back(project_tool_ids[i]);

    if (!unmapped_tools.empty()) {
        error = "Could not map tools " + tool_id_list(unmapped_tools) +
                " to material station slots by material type; pass material_mappings explicitly";
        return false;
    }

    return true;
}

bool resolve_material_mappings(const nlohmann::json&                           requested,
                               const std::vector<FlashforgeApi::MaterialSlot>& slots,
                               const nlohmann::json&                           project_filaments,
                               nlohmann::json&                                 mappings_out,
                               nlohmann::json&                                 error_out)
{
    mappings_out = nlohmann::json::array();

    if (!requested.empty()) {
        std::string build_error;
        if (!build_explicit_material_mappings(requested, slots, mappings_out, build_error)) {
            error_out = {{"status", "error"}, {"message", build_error}};
            return false;
        }
    } else {
        mappings_out = auto_material_mappings(project_filaments, slots);
    }

    // Single gate for both tools: every mapping's slot must exist and be loaded, and every project
    // tool must end up mapped -- a partial mapping (auto or explicit) never reaches the printer.
    // Only the tools the sliced plate actually uses; their ids need not be contiguous (an object
    // printed with filament 2 alone yields the single tool id 1).
    std::vector<int> project_tool_ids;
    for (const auto& filament : project_filaments)
        project_tool_ids.push_back(filament.value("tool_id", -1));

    std::string      validation_error;
    std::vector<int> unmapped_tools;
    if (!validate_material_mappings(mappings_out, slots, project_tool_ids, validation_error, unmapped_tools)) {
        error_out = {{"status", "error"}, {"message", validation_error}};
        if (!unmapped_tools.empty()) {
            error_out["unmapped_tools"] = unmapped_tools;
            error_out["slots"]          = material_slots_json(slots);
        }
        return false;
    }
    return true;
}

nlohmann::json gather_project_filaments(std::string& error)
{
    nlohmann::json result = nlohmann::json::array();

    Plater*       plater = wxGetApp().plater();
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (plater == nullptr || bundle == nullptr) {
        error = "Plater not available";
        return result;
    }

    PartPlateList& plate_list = plater->get_partplate_list();
    const int      plate_idx  = plate_list.get_curr_plate_index();
    PartPlate*     plate      = plate_list.get_curr_plate();
    if (plate == nullptr) {
        error = "No current plate";
        return result;
    }

    // Plater::send_gcode_legacy's own source of truth: a store_to_3mf_structure snapshot runs
    // PlateData::parse_filament_info on the plate's live slice result, which is what actually has data
    // for a plate sliced in this session. PartPlate::slice_filaments_info (the plain member) is only
    // ever populated by reloading a previously-sliced 3MF, so it is checked second, as a fallback.
    std::vector<FilamentInfo> project_filaments;
    PlateDataPtrs             plate_data_list;
    plate_list.store_to_3mf_structure(plate_data_list, true, plate_idx);
    PlateData* selected_plate_data =
        (plate_idx >= 0 && plate_idx < static_cast<int>(plate_data_list.size())) ? plate_data_list[plate_idx] : nullptr;
    if (selected_plate_data == nullptr && !plate_data_list.empty())
        selected_plate_data = plate_data_list.front();
    if (selected_plate_data != nullptr)
        project_filaments = selected_plate_data->slice_filaments_info;
    release_PlateData_list(plate_data_list);

    if (project_filaments.empty())
        project_filaments = plate->get_slice_filaments_info();

    if (project_filaments.empty()) {
        // Both sources come up empty for an unsliced plate; a plate that has actually been sliced does
        // carry at least one filament in practice, so treat this as "not sliced" rather than "0 tools".
        if (!plate->is_slice_result_valid())
            error = "Plate is not sliced; run slice_all first";
        return result;
    }

    // Same enrichment as Plater::send_gcode_legacy's enrich_project_filaments: type/colour always come
    // from the *current* config, not whatever (possibly stale) values FilamentInfo itself carries.
    DynamicPrintConfig cfg            = bundle->full_config();
    const auto*        filament_color = dynamic_cast<const ConfigOptionStrings*>(cfg.option("filament_colour"));
    for (const FilamentInfo& filament : project_filaments) {
        if (filament.id < 0)
            continue;

        std::string display_type, type;
        try {
            type = cfg.get_filament_type(display_type, filament.id);
        } catch (...) {
        }
        if (type.empty())
            type = display_type;
        if (type.empty())
            type = "Unknown";

        std::string color = filament_color != nullptr ? filament_color->get_at(static_cast<size_t>(filament.id)) : std::string();
        if (color.empty())
            color = "#FFFFFF";

        result.push_back({{"tool_id", filament.id}, {"type", type}, {"color", color}});
    }
    return result;
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
