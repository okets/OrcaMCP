// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp
#pragma once

#include <memory>
#include <string>
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

// Main thread. The config OrcaSlicer would send to: the edited printer preset.
// False + `error` when it has no print host.
bool resolve_print_host_config(DynamicPrintConfig& out, std::string& host_type_name, std::string& error);

// Any thread. `cfg` must outlive the returned host.
std::unique_ptr<PrintHost> make_print_host(const DynamicPrintConfig& cfg);

// Any thread. Serializes a printer status into the `printer` object of get_printer_status.
nlohmann::json status_to_json(const FlashforgeApi::PrinterStatus& s);

// Main thread. Every printer preset carrying a non-empty print_host.
nlohmann::json print_host_presets_json();

// Main thread. The edited printer preset's name when it has a print host, else null.
nlohmann::json selected_print_host_preset_json();

// Main thread, caller holds a McpDialogSuppressionGuard. Selects a printer preset that has a print
// host and returns the tool response.
nlohmann::json select_print_host_preset(const std::string& name);

// Main thread, caller holds a McpDialogSuppressionGuard. Writes the print host settings into the
// edited printer preset and saves it as the user preset `name`, the way PhysicalPrinterDialog does.
// Returns an empty string on success, otherwise the error message.
std::string save_print_host_preset(const std::string& name,
                                   const std::string& host,
                                   const std::string& host_type,
                                   const std::string& serial_number,
                                   const std::string& api_key,
                                   const std::string& printer_preset);

}}} // namespace Slic3r::GUI::OrcaMCP
