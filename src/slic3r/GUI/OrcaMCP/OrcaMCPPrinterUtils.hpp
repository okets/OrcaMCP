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
// OrcaMCPPrinterTools.cpp's auto_material_mappings / build_explicit_material_mappings) against the
// printer's material station and the project's tool count. Fails when a mapping's slot_id does not name
// a slot that is present and loaded on the printer, a mapping's toolId is outside [0, tool_count), or any
// tool in that range has no mapping at all -- the same "every project material must be assigned to a
// loaded slot" rule FlashforgePrintHostSendDialog::validate_before_close enforces before enabling Send.
// On failure `error` explains why; `unmapped_tools` additionally lists the specific tool ids left
// unmapped (only for that failure mode; empty otherwise). Shared by print_printer_file and (task 2.6)
// send_to_printer so the rule exists in exactly one place.
bool validate_material_mappings(const nlohmann::json&                           mappings,
                                const std::vector<FlashforgeApi::MaterialSlot>& slots,
                                size_t                                          tool_count,
                                std::string&                                    error,
                                std::vector<int>&                               unmapped_tools);

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
