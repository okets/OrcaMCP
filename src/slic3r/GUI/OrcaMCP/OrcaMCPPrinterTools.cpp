// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPrinterUtils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/Utils/Flashforge.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <optional>
#include <boost/algorithm/string/join.hpp>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// Host types add_physical_printer accepts; also the schema's enum.
const std::vector<std::string> kSupportedHostTypes = {
    "flashforge", "moonraker", "octoprint", "prusalink", "duet", "repetier", "mks", "elegoolink", "crealityprint"
};

nlohmann::json error_response(const std::string& message)
{
    return {{"status", "error"}, {"message", message}};
}

// Absent when the caller did not send the key, so it can be told apart from an explicit "".
std::optional<std::string> optional_string(const nlohmann::json& params, const std::string& key)
{
    if (!params.contains(key) || !params.at(key).is_string())
        return std::nullopt;
    return params.at(key).get<std::string>();
}

} // namespace

void OrcaMCPServer::register_printer_tools()
{
    // get_printers - Get list of available printers
    register_tool({
        "get_printers",
        "Get available printers and their status.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([]() {
                DeviceManager* device_mgr = wxGetApp().getDeviceManager();
                if (!device_mgr) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "Device manager not available"}
                    };
                }

                nlohmann::json result;
                result["local_printers"] = nlohmann::json::array();
                result["cloud_printers"] = nlohmann::json::array();

                // Get local network printers
                auto local_machines = device_mgr->get_local_machinelist();
                for (const auto& [dev_id, machine] : local_machines) {
                    if (!machine) continue;
                    nlohmann::json printer_info = {
                        {"dev_id", machine->get_dev_id()},
                        {"dev_name", machine->get_dev_name()},
                        {"dev_ip", machine->get_dev_ip()},
                        {"printer_type", machine->printer_type},
                        {"connection_type", machine->connection_type()},
                        {"is_online", machine->m_is_online},
                        {"bind_state", machine->bind_state},
                        {"has_access_right", machine->has_access_right()}
                    };

                    // Add print status if available
                    if (machine->is_system_printing()) {
                        printer_info["print_status"] = "printing";
                        printer_info["print_percent"] = machine->mc_print_percent;
                    } else {
                        printer_info["print_status"] = "idle";
                    }

                    result["local_printers"].push_back(printer_info);
                }

                // Get cloud printers (user's machines)
                auto cloud_machines = device_mgr->get_my_cloud_machine_list();
                for (const auto& [dev_id, machine] : cloud_machines) {
                    if (!machine) continue;
                    // Skip if already in local list
                    bool in_local = false;
                    for (const auto& local : result["local_printers"]) {
                        if (local["dev_id"] == machine->get_dev_id()) {
                            in_local = true;
                            break;
                        }
                    }
                    if (in_local) continue;

                    nlohmann::json printer_info = {
                        {"dev_id", machine->get_dev_id()},
                        {"dev_name", machine->get_dev_name()},
                        {"printer_type", machine->printer_type},
                        {"connection_type", machine->connection_type()},
                        {"is_online", machine->m_is_online}
                    };

                    if (machine->is_system_printing()) {
                        printer_info["print_status"] = "printing";
                        printer_info["print_percent"] = machine->mc_print_percent;
                    } else {
                        printer_info["print_status"] = "idle";
                    }

                    result["cloud_printers"].push_back(printer_info);
                }

                // Get currently selected printer
                MachineObject* selected = device_mgr->get_selected_machine();
                if (selected) {
                    result["selected_printer"] = {
                        {"dev_id", selected->get_dev_id()},
                        {"dev_name", selected->get_dev_name()}
                    };
                }

                // "Physical printers" are the printer presets that carry a print host: that is where
                // PhysicalPrinterDialog writes it and where Plater::send_gcode_legacy reads it from.
                result["physical_printers"] = nlohmann::json::array();
                result["selected_physical_printer"] = nullptr;
                PresetBundle* preset_bundle = wxGetApp().preset_bundle;
                if (preset_bundle) {
                    result["physical_printers"] = print_host_presets_json();
                    result["selected_physical_printer"] = selected_print_host_preset_json();

                    // Also check current printer preset for embedded print host config
                    const Preset& current_printer = preset_bundle->printers.get_edited_preset();
                    const DynamicPrintConfig& printer_config = current_printer.config;
                    if (printer_config.has("print_host")) {
                        std::string print_host = printer_config.opt_string("print_host");
                        if (!print_host.empty()) {
                            // Same spelling as physical_printers[].host_type, so it round-trips into
                            // add_physical_printer.
                            result["current_print_host"] = {
                                {"name", current_printer.name},
                                {"type", "printer_preset_host"},
                                {"print_host", print_host},
                                {"host_type", print_host_type_name(printer_config)},
                                {"is_current", true}
                            };
                        }
                    }
                }

                result["total_count"] = result["local_printers"].size() + result["cloud_printers"].size() + result["physical_printers"].size();
                return result;
            });
        }
    });

    // select_printer - Select a Bambu device by dev_id, or a physical printer (print host) by name
    register_tool({
        "select_printer",
        "Select a printer: a Bambu device by dev_id, or a printer preset with a print host by name.",
        {
            {"type", "object"},
            {"properties", {
                {"dev_id", {
                    {"type", "string"},
                    {"description", "Bambu device ID"}
                }},
                {"physical_printer", {
                    {"type", "string"},
                    {"description", "Printer preset with a print host, as listed by get_printers"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const std::string dev_id           = params.value("dev_id", std::string());
            const std::string physical_printer = params.value("physical_printer", std::string());
            if (dev_id.empty() == physical_printer.empty())
                return error_response("Provide exactly one of dev_id (Bambu device) or physical_printer");

            if (!physical_printer.empty())
                return run_on_main_thread([physical_printer]() -> nlohmann::json {
                    McpDialogSuppressionGuard suppression;
                    return select_print_host_preset(physical_printer);
                });

            return run_on_main_thread([dev_id]() -> nlohmann::json {
                DeviceManager* device_mgr = wxGetApp().getDeviceManager();
                if (!device_mgr)
                    return error_response("Device manager not available");

                if (!device_mgr->set_selected_machine(dev_id))
                    return error_response("Failed to select printer with dev_id: " + dev_id);

                MachineObject* machine = device_mgr->get_selected_machine();
                return nlohmann::json{
                    {"status", "success"},
                    {"dev_id", dev_id},
                    {"dev_name", machine ? machine->get_dev_name() : ""},
                    {"is_online", machine ? machine->m_is_online : false}
                };
            });
        }
    });

    // send_to_printer - Open the send-to-printer dialog
    register_tool({
        "send_to_printer",
        "Send sliced G-code to printer.",
        {
            {"type", "object"},
            {"properties", {
                {"all_plates", {
                    {"type", "boolean"},
                    {"description", "If true, send all plates. If false (default), send current plate only."}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            bool all_plates = params.value("all_plates", false);
            return run_on_main_thread([all_plates]() {
                Plater* plater = wxGetApp().plater();
                if (!plater) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "Plater not available"}
                    };
                }

                // Check if slicing is complete
                if (plater->is_background_process_slicing()) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "Slicing is still in progress. Wait for slicing to complete before sending to printer."}
                    };
                }

                // Check if current printer preset has a print host configured (OctoPrint, Klipper, etc.)
                PresetBundle* preset_bundle = wxGetApp().preset_bundle;
                bool has_print_host = false;
                std::string host_type_str = "unknown";
                std::string print_host;

                if (preset_bundle) {
                    const DynamicPrintConfig& printer_config = preset_bundle->printers.get_edited_preset().config;
                    if (printer_config.has("print_host")) {
                        print_host = printer_config.opt_string("print_host");
                        has_print_host = !print_host.empty();
                    }
                    if (has_print_host && !print_host_type_name(printer_config).empty())
                        host_type_str = print_host_type_name(printer_config);
                }

                // Suppress any dialogs during send operation
                set_mcp_dialog_suppression(true);

                nlohmann::json result;
                if (has_print_host) {
                    // Use legacy send for OctoPrint/Klipper/etc. printers
                    int plate_idx = all_plates ? -1 : plater->get_partplate_list().get_curr_plate_index();
                    plater->send_gcode_legacy(plate_idx);

                    result = {
                        {"status", "dialog_opened"},
                        {"method", "send_gcode_legacy"},
                        {"host_type", host_type_str},
                        {"print_host", print_host},
                        {"note", "Send G-code dialog opened for print host upload."}
                    };
                } else {
                    // Use Bambu-specific send dialog
                    plater->send_to_printer(all_plates);

                    result = {
                        {"status", "dialog_opened"},
                        {"method", "send_to_printer"},
                        {"all_plates", all_plates},
                        {"note", "The send-to-printer dialog is now open. User can select printer and options."}
                    };
                }

                auto info_messages = get_mcp_suppressed_messages();
                set_mcp_dialog_suppression(false);

                // Check for errors
                bool has_errors = false;
                for (const auto& msg : info_messages) {
                    if (msg.find("error") != std::string::npos ||
                        msg.find("Error") != std::string::npos ||
                        msg.find("Failed") != std::string::npos ||
                        msg.find("failed") != std::string::npos) {
                        has_errors = true;
                        break;
                    }
                }

                if (has_errors) {
                    result["status"] = "error";
                    result["error_messages"] = info_messages;
                } else if (!info_messages.empty()) {
                    result["info_messages"] = info_messages;
                }

                return result;
            });
        }
    });

    // discover_printers - Find Flashforge printers on the local network
    register_tool({
        "discover_printers",
        "Discover Flashforge printers on the local network via UDP broadcast.",
        {
            {"type", "object"},
            {"properties", {
                {"timeout_ms", {
                    {"type", "integer"},
                    {"description", "Discovery timeout in milliseconds (default 5000)"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const int timeout_ms = std::clamp(params.value("timeout_ms", 5000), 500, 60000);

            // Pure network I/O: stays on the HTTP worker so the GUI is not blocked while discovering.
            std::vector<Slic3r::FlashforgeDiscoveredPrinter> discovered;
            wxString msg;
            if (!Slic3r::Flashforge::discover_printers(discovered, msg, timeout_ms))
                return error_response(msg.empty() ? "Printer discovery failed" : std::string(msg.ToUTF8().data()));

            nlohmann::json printers = nlohmann::json::array();
            for (const auto& printer : discovered)
                printers.push_back({
                    {"name", printer.name},
                    {"serial_number", printer.serial_number},
                    {"ip_address", printer.ip_address}
                });

            return {{"status", "success"}, {"printers", printers}};
        }
    });

    // add_physical_printer - Create or overwrite a physical printer and select it
    register_tool({
        "add_physical_printer",
        "Configure a print host on the current printer preset and save it as a user preset with this name.",
        {
            {"type", "object"},
            {"properties", {
                {"name", {
                    {"type", "string"},
                    {"description", "Name to save the printer preset under, e.g. \"C5P\""}
                }},
                {"host", {
                    {"type", "string"},
                    {"description", "IP address or hostname of the printer"}
                }},
                {"host_type", {
                    {"type", "string"},
                    {"enum", kSupportedHostTypes},
                    {"description", "Print host type"}
                }},
                {"serial_number", {
                    {"type", "string"},
                    {"description", "Flashforge serial number (from discover_printers). Omit to keep the stored one."}
                }},
                {"api_key", {
                    {"type", "string"},
                    {"description", "API key, or the Flashforge LAN check code. Omit to keep the stored one."}
                }},
                {"printer_preset", {
                    {"type", "string"},
                    {"description", "Printer preset to base it on (default: the edited printer preset). "
                                    "Settings you do not pass are taken from that preset, so naming a "
                                    "different one replaces the stored credentials of an existing printer."}
                }}
            }},
            {"required", {"name", "host", "host_type"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const std::string name           = params.value("name", std::string());
            const std::string host           = params.value("host", std::string());
            const std::string host_type      = params.value("host_type", std::string());
            const std::string printer_preset = params.value("printer_preset", std::string());
            // Omitted credentials keep whatever the preset already stores.
            const std::optional<std::string> serial_number = optional_string(params, "serial_number");
            const std::optional<std::string> api_key       = optional_string(params, "api_key");

            if (name.empty() || host.empty() || host_type.empty())
                return error_response("name, host and host_type are required");
            if (std::find(kSupportedHostTypes.begin(), kSupportedHostTypes.end(), host_type) == kSupportedHostTypes.end())
                return error_response("Unsupported host_type '" + host_type + "'. Supported: " +
                                      boost::algorithm::join(kSupportedHostTypes, ", "));

            return run_on_main_thread([name, host, host_type, serial_number, api_key, printer_preset]() -> nlohmann::json {
                McpDialogSuppressionGuard suppression;
                const std::string error = save_print_host_preset(name, host, host_type, serial_number, api_key, printer_preset);
                if (!error.empty())
                    return error_response(error);
                return select_print_host_preset(name);
            });
        }
    });
}
