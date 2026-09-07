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

// Main thread. Orca never loads the physical printer collection at start-up (PhysicalPrinterCollection::
// load_printers has no call site) and never persists its selection, so the MCP tools do both themselves,
// once per process, before touching preset_bundle->physical_printers.
void ensure_physical_printers_loaded();

// Main thread. The config OrcaSlicer would use for "Send": the edited printer preset, with the selected
// physical printer's print-host options applied on top. False + `error` when no print host is configured.
bool resolve_print_host_config(DynamicPrintConfig& out, std::string& host_type_name, std::string& error);

// Any thread. `cfg` must outlive the returned host.
std::unique_ptr<PrintHost> make_print_host(const DynamicPrintConfig& cfg);

// Any thread. Serializes a printer status into the `printer` object of get_printer_status.
nlohmann::json status_to_json(const FlashforgeApi::PrinterStatus& s);

// Main thread. Selects a physical printer by short ("C5P") or full ("C5P * Preset") name and refreshes the
// printer tab and plater combo the way PresetComboBox does. Returns the tool response.
nlohmann::json select_physical_printer(const std::string& name);

// Main thread. Creates or overwrites a physical printer and saves it. Returns an empty string on success,
// otherwise the error message.
std::string upsert_physical_printer(const std::string& name,
                                    const std::string& host,
                                    const std::string& host_type,
                                    const std::string& serial_number,
                                    const std::string& api_key,
                                    const std::string& printer_preset);

}}} // namespace Slic3r::GUI::OrcaMCP
