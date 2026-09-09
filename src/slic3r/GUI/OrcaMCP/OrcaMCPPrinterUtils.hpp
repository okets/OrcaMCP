// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/FlashforgeApi.hpp"

namespace Slic3r {
class PrintHost;
namespace GUI {
namespace OrcaMCP {

// In OrcaSlicer the print host lives in the printer preset itself: PhysicalPrinterDialog edits
// printers.get_edited_preset().config and saves it as a user printer preset, and
// Plater::send_gcode_legacy sends to whatever the edited preset points at. "Physical printer" in the
// MCP tools therefore means "a printer preset that has a print_host".

// Any thread. The config's host_type as the option's own enum name ("flashforge", "crealityprint", ...).
std::string print_host_type_name(const DynamicPrintConfig& config);

// Main thread. The config OrcaSlicer would send to: the edited printer preset.
// False + `error` when it has no print host.
bool resolve_print_host_config(DynamicPrintConfig& out, std::string& host_type_name, std::string& error);

// Any thread. `cfg` must outlive the returned host.
std::unique_ptr<PrintHost> make_print_host(const DynamicPrintConfig& cfg);

// Any thread. Serializes a printer status into the `printer` object of get_printer_status.
nlohmann::json status_to_json(const FlashforgeApi::PrinterStatus& s);

// Any thread. The material_station.slots shape shared between status_to_json and a material-mapping
// validation error's "slots" field.
nlohmann::json material_slots_json(const std::vector<FlashforgeApi::MaterialSlot>& slots);

// Any thread; pure data, no I/O. Validates a built {toolId, slotId, ...} mapping payload (see
// resolve_material_mappings) against the printer's material station and the project's own tool ids.
// Fails when a mapping's slot_id does not name a slot that is present and loaded on the printer, a
// mapping's toolId is not one of `project_tool_ids`, or one of those tools has no mapping at all --
// the same "every project material must be assigned to a loaded slot" rule
// FlashforgePrintHostSendDialog::validate_before_close enforces before enabling Send. The ids are
// passed as a list rather than a count because a sliced plate only reports the tools it actually
// uses: an object printed with filament 2 alone yields the single tool id 1.
// On failure `error` explains why; `unmapped_tools` additionally lists the specific tool ids left
// unmapped (only for that failure mode; empty otherwise). Called through resolve_material_mappings,
// which is what the tools use.
bool validate_material_mappings(const nlohmann::json&                           mappings,
                                const std::vector<FlashforgeApi::MaterialSlot>& slots,
                                const std::vector<int>&                         project_tool_ids,
                                std::string&                                    error,
                                std::vector<int>&                               unmapped_tools);

// Any thread; pure. CIE76 (Lab) distance between two colours as the project and the printer report
// them, or nullopt when either is not a colour we can decode. Both are validated with
// OrcaMCP::is_hex_color (alpha allowed) before being decomposed, because upstream's hex parser reads
// "#GGGGGG" as black and ignores trailing garbage -- a wrong number is worse than no number here,
// since the number is what an agent warns the user on. The metric is the one suggest_color_mix uses;
// there is deliberately no second colour metric in this codebase.
std::optional<double> material_color_delta_e(const std::string& a, const std::string& b);

// Any thread; pure. One project filament, as auto-mapping sees it.
struct ProjectFilamentTool
{
    int         tool_id{-1};
    std::string material; // the project's raw type ("PLA", "PETG-CF", ...); normalized internally
    std::string color;    // "#RRGGBB"; empty or unparseable means "colour unknown"
};

// Any thread; pure. One tool's chosen slot.
struct AutoMaterialMapping
{
    int                   tool_id{-1};
    int                   slot_id{-1};
    std::optional<double> color_delta_e; // nullopt when the choice could not be scored by colour
};

// Any thread; pure. Chooses one loaded material-station slot per project tool, in tool order,
// never reusing a slot. Candidates are the free loaded slots whose normalized material family
// (flashforge_normalize_material) equals the tool's; among them the nearest colour wins, ties break
// on the lower slot id so the result never depends on the order the printer listed its slots, and a
// candidate whose colour cannot be scored only wins if no candidate can be scored -- which is the
// old first-free behaviour, kept as the fallback. A tool with no candidate is left out entirely;
// validate_material_mappings is what turns that into the caller's error.
std::vector<AutoMaterialMapping> auto_map_tools_to_slots(const std::vector<ProjectFilamentTool>&         tools,
                                                        const std::vector<FlashforgeApi::MaterialSlot>& slots);

// Any thread; pure data, no I/O. The whole material-mapping step of print_printer_file and
// send_to_printer: builds the {toolId, slotId, materialName, toolMaterialColor, slotMaterialColor}
// payload the Flashforge local API takes -- from the caller's explicit {tool_id, slot_id} pairs when
// it gave any, otherwise by matching each project filament to a free loaded slot of the same material
// family and the nearest colour (auto_map_tools_to_slots) -- and puts the result through
// validate_material_mappings. On failure `error_out` is a ready-to-return tool error response,
// carrying `unmapped_tools` and `slots` when auto-mapping came up short so the caller can see what
// the printer has loaded.
// `report_out`, when given, receives the tool-facing echo of the same mapping: an array of
// {"tool_id", "slot_id", "color_delta_e"}, the last null when the pair could not be scored. It is a
// separate array and not extra keys on `mappings_out` because `mappings_out` goes over the wire to
// the printer verbatim.
bool resolve_material_mappings(const nlohmann::json&                           requested,
                               const std::vector<FlashforgeApi::MaterialSlot>& slots,
                               const nlohmann::json&                           project_filaments,
                               nlohmann::json&                                 mappings_out,
                               nlohmann::json&                                 error_out,
                               nlohmann::json*                                 report_out = nullptr);

// Main thread. The current plate's per-tool filament types/colours for material-mapping, mirroring how
// Plater::send_gcode_legacy builds `project_filaments` (Plater.cpp ~19297-19311): a
// PartPlateList::store_to_3mf_structure snapshot (which runs PlateData::parse_filament_info on the
// plate's live slice result) is the primary source, falling back to PartPlate::get_slice_filaments_info
// (only ever populated by reloading a previously-sliced 3MF) if that snapshot is empty.
// `PartPlate::slice_filaments_info` is never populated by slice_all alone, so neither source being
// non-empty means the plate has not actually been sliced -- as opposed to a plate that was sliced but
// genuinely uses no filaments, an edge case that in practice does not happen. Returns an empty array
// either way; `error` is set to "Plate is not sliced; run slice_all first" only in the former case, and
// left empty otherwise (including "no plate"/"nothing on it" cases some callers may treat as fine).
// Shared by print_printer_file and send_to_printer.
nlohmann::json gather_project_filaments(std::string& error);

// Main thread. Every printer preset carrying a non-empty print_host.
nlohmann::json print_host_presets_json();

// Main thread. The edited printer preset's name when it has a print host, else null.
nlohmann::json selected_print_host_preset_json();

// Main thread, caller holds a McpDialogSuppressionGuard. Selects a printer preset that has a print
// host and returns the tool response.
nlohmann::json select_print_host_preset(const std::string& name);

// Main thread, caller holds a McpDialogSuppressionGuard. Writes the print host settings into the
// edited printer preset and saves it as the user preset `name`, the way PhysicalPrinterDialog does.
// `serial_number`/`api_key` are left untouched when not supplied. Returns an empty string on success,
// otherwise the error message.
std::string save_print_host_preset(const std::string&                name,
                                   const std::string&                host,
                                   const std::string&                host_type,
                                   const std::optional<std::string>& serial_number,
                                   const std::optional<std::string>& api_key,
                                   const std::string&                printer_preset);

}}} // namespace Slic3r::GUI::OrcaMCP
