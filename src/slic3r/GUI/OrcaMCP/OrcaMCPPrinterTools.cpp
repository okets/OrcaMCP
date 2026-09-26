// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPrinterUtils.hpp"
#include "OrcaMCPProjectMatch.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/FlashforgeApi.hpp"
#include "slic3r/Utils/FlashforgeLocalApi.hpp"
#include "slic3r/Utils/ObicoLink.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <optional>
#include <boost/algorithm/string/join.hpp>

using namespace Slic3r;
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

std::string to_std(const wxString& s)
{
    return std::string(s.ToUTF8().data());
}

// The Flashforge local-API-credentials message, shared so it exists in exactly one place: resolve_flashforge
// (used by printer_control/list_printer_files/print_printer_file) and get_printer_status (which cannot use
// resolve_flashforge because it also has to handle non-Flashforge hosts) both return this.
nlohmann::json flashforge_credentials_error()
{
    return error_response("Flashforge local API requires both a serial number and an access code. "
                          "Use add_physical_printer to set them.");
}

// Resolves the print host the same way Plater::send_gcode_legacy does (main thread, fast), then builds
// the concrete PrintHost off the main thread so no network I/O ever blocks the GUI.
bool resolve_print_host(std::unique_ptr<Slic3r::PrintHost>& host, DynamicPrintConfig& cfg, std::string& host_type, nlohmann::json& error_out)
{
    std::string err;
    const nlohmann::json resolved = run_on_main_thread([&]() -> nlohmann::json {
        bool ok = resolve_print_host_config(cfg, host_type, err);
        return {{"ok", ok}};
    });
    if (!resolved["ok"].get<bool>()) {
        error_out = error_response(err);
        return false;
    }

    host = make_print_host(cfg);
    if (!host) {
        error_out = error_response("Failed to create a print host for type '" + host_type + "'");
        return false;
    }
    return true;
}

// The four Flashforge control/status tools all need the same thing: a resolved Flashforge host, off the
// main thread. `cfg` is kept alive by the caller for as long as `host` is used.
bool resolve_flashforge(std::unique_ptr<Slic3r::PrintHost>& host, Slic3r::Flashforge*& ff, nlohmann::json& error_out)
{
    DynamicPrintConfig cfg;
    std::string        host_type;
    if (!resolve_print_host(host, cfg, host_type, error_out))
        return false;

    ff = dynamic_cast<Slic3r::Flashforge*>(host.get());
    if (!ff) {
        error_out = error_response("Selected printer host type is '" + host_type +
                                    "'; this tool requires a Flashforge host");
        return false;
    }
    if (!ff->has_local_api_credentials()) {
        error_out = flashforge_credentials_error();
        return false;
    }
    return true;
}

// send_to_printer with direct=false: hand the send over to the user's own dialog.
nlohmann::json open_send_dialog(bool all_plates)
{
    return run_on_main_thread([all_plates]() {
        Plater* plater = wxGetApp().plater();
        if (!plater)
            return error_response("Plater not available");

        // Check if slicing is complete
        if (plater->is_background_process_slicing())
            return error_response("Slicing is still in progress. Wait for slicing to complete before sending to printer.");

        // Check if current printer preset has a print host configured (OctoPrint, Klipper, etc.)
        PresetBundle* preset_bundle  = wxGetApp().preset_bundle;
        bool          has_print_host = false;
        std::string   host_type_str  = "unknown";
        std::string   print_host;

        if (preset_bundle) {
            const DynamicPrintConfig& printer_config = preset_bundle->printers.get_edited_preset().config;
            if (printer_config.has("print_host")) {
                print_host     = printer_config.opt_string("print_host");
                has_print_host = !print_host.empty();
            }
            if (has_print_host && !print_host_type_name(printer_config).empty())
                host_type_str = print_host_type_name(printer_config);
        }

        // Both send paths open a modal dialog (SelectMachineDialog / the print-host send
        // dialog). Opening one here would block the GUI thread until the user dismisses it,
        // and this call would never return. Schedule it to open after the tool has replied,
        // so the user gets the dialog and MCP gets an immediate answer.
        nlohmann::json result;
        if (has_print_host) {
            // Use legacy send for OctoPrint/Klipper/etc. printers
            int plate_idx = all_plates ? PLATE_ALL_IDX : plater->get_partplate_list().get_curr_plate_index();
            wxGetApp().CallAfter([plate_idx]() {
                if (Plater* p = wxGetApp().plater())
                    p->send_gcode_legacy(plate_idx);
            });

            result = {
                {"status", "dialog_opened"},
                {"method", "send_gcode_legacy"},
                {"host_type", host_type_str},
                {"print_host", print_host},
                {"note", "Send G-code dialog opening for print host upload; it is driven by the user, not by MCP."}
            };
        } else {
            // Use Bambu-specific send dialog
            wxGetApp().CallAfter([all_plates]() {
                if (Plater* p = wxGetApp().plater())
                    p->send_to_printer(all_plates);
            });

            result = {
                {"status", "dialog_opened"},
                {"method", "send_to_printer"},
                {"all_plates", all_plates},
                {"note", "The send-to-printer dialog is opening; the user selects printer and options there."}
            };
        }

        return result;
    });
}

// The material station from the printer's last answer, for a response whose live read failed.
nlohmann::json cached_station_json(const FlashforgeLocalApi::CachedStatus& cached)
{
    return {{"source", "cached"},
            {"age_s", cached.age_s},
            {"material_station",
             {{"present", cached.status.has_material_station}, {"slots", material_slots_json(cached.status.slots)}}}};
}

// match_project_to_printer when the live read failed: plan from the printer's last answer, and change
// the project only when the caller opted in, since a spool may have been swapped since.
nlohmann::json match_from_cached_status(const FlashforgeLocalApi::CachedStatus& cached,
                                        const std::vector<int>&                 slots,
                                        bool                                    dry_run,
                                        bool                                    allow_cached,
                                        const std::string&                      live_error)
{
    const bool applies  = cached_match_applies(dry_run, allow_cached);
    const bool withheld = !dry_run && !applies;

    nlohmann::json response = run_on_main_thread([station = cached.status.slots, slots, applies]() -> nlohmann::json {
        return match_project_to_printer(station, slots, /*dry_run=*/!applies);
    });
    response["source"]     = "cached";
    response["age_s"]      = cached.age_s;
    response["live_error"] = live_error;
    response["note"]       = cached_match_note(cached.age_s, withheld);
    if (withheld)
        response["applied"] = false;
    return response;
}

// How long ago the preset's Flashforge last answered a status read: null when it has not since the
// app started, and absent for any other host type. Never touches the network.
std::optional<nlohmann::json> last_status_age_json(const DynamicPrintConfig& config)
{
    const std::unique_ptr<Slic3r::PrintHost> host = make_print_host(config);
    const auto*                              ff   = dynamic_cast<const Slic3r::Flashforge*>(host.get());
    if (ff == nullptr)
        return std::nullopt;
    const auto cached = ff->last_known_status();
    return cached ? nlohmann::json(cached->age_s) : nlohmann::json(nullptr);
}
} // namespace

void OrcaMCPServer::register_printer_tools()
{
    // get_printers - Get list of available printers
    register_tool({
        "get_printers",
        ToolCategory::Printers,
        "Printers and print-host presets",
        "Get available printers and their status. local_printers[].is_online is the device list's flag, set "
        "when a device is added, not a live check. For a Flashforge print host, "
        "current_print_host.last_status_age_s is how many seconds ago the printer last answered (null: not "
        "since the app started); get_printer_status reads it live.",
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
                            if (const auto age = last_status_age_json(printer_config))
                                result["current_print_host"]["last_status_age_s"] = *age;
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
        ToolCategory::Printers,
        "Select a Bambu device or a print host",
        "Select a printer: a Bambu device by dev_id, or a printer preset with a print host by name. A "
        "switch of printer preset also reports the resulting filaments and colors_source, as select_preset "
        "{type: printer} does.",
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

    // send_to_printer - Upload the sliced plate to the configured print host
    register_tool({
        "send_to_printer",
        ToolCategory::Printers,
        "Upload the plate AND START printing it",
        "Upload the sliced plate to the configured print host and START PRINTING IT: start_print defaults to true, so a bare call begins a print on real hardware. Pass start_print=false to upload only. The upload runs "
        "without any dialog: on a Flashforge printer with a material station the project's filaments are "
        "mapped onto the loaded slots automatically (pass material_mappings to choose the slots "
        "yourself). Pass direct=false to open OrcaSlicer's send dialog and leave the send to the user.",
        {
            {"type", "object"},
            {"properties", {
                {"direct", {
                    {"type", "boolean"},
                    {"description", "When true (default), upload straight to the printer. When false, open "
                                    "OrcaSlicer's send dialog for the user instead."}
                }},
                {"start_print", {
                    {"type", "boolean"},
                    {"description", "Start printing once the upload finishes (default true). Direct sends only."}
                }},
                {"leveling_before_print", {
                    {"type", "boolean"},
                    {"description", "Run bed leveling before printing (default false). Direct sends to a "
                                    "Flashforge host with local-API credentials only; ignored otherwise."}
                }},
                {"use_material_station", {
                    {"type", "boolean"},
                    {"description", "Print from the material station (default: true when the printer reports "
                                    "one). Direct sends to a Flashforge host with local-API credentials only; "
                                    "ignored otherwise."}
                }},
                {"material_mappings", {
                    {"type", "array"},
                    {"description", "Explicit tool-to-slot mapping. Omit to map the project's filaments onto "
                                    "matching loaded slots automatically. Direct sends to a Flashforge host "
                                    "with local-API credentials only; ignored otherwise."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"tool_id", {{"type", "integer"}, {"description", "Project filament/tool index, 0-based"}}},
                            {"slot_id", {{"type", "integer"}, {"description", "Material station slot id"}}}
                        }},
                        {"required", {"tool_id", "slot_id"}}
                    }}
                }},
                {"file_name", {
                    {"type", "string"},
                    {"description", "Name to store the upload under (default: the plate's own output file name). "
                                    "Direct sends only."}
                }},
                {"all_plates", {
                    {"type", "boolean"},
                    {"description", "Dialog sends only (direct=false): send all plates instead of the current one."}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            if (params.contains("direct") && !params.at("direct").is_boolean())
                return error_response("direct must be a boolean");
            const bool all_plates = params.value("all_plates", false);
            if (!params.value("direct", true))
                return open_send_dialog(all_plates);

            if (all_plates)
                return error_response("direct send supports one plate at a time; select the plate first with "
                                      "select_plate, or pass direct=false to send all plates from the dialog");

            for (const char* flag : {"start_print", "leveling_before_print", "use_material_station"})
                if (params.contains(flag) && !params.at(flag).is_boolean())
                    return error_response(std::string(flag) + " must be a boolean");
            if (params.contains("file_name") && !params.at("file_name").is_string())
                return error_response("file_name must be a string");
            const nlohmann::json requested_mappings = params.value("material_mappings", nlohmann::json::array());
            if (!requested_mappings.is_array())
                return error_response("material_mappings must be an array");

            const bool        start_print = params.value("start_print", true);
            const bool        leveling    = params.value("leveling_before_print", false);
            const std::string file_name   = params.value("file_name", std::string());

            // Main thread: everything that reads the plater and the presets.
            DynamicPrintConfig       cfg;
            std::string              host_type;
            std::string              prep_error;
            nlohmann::json           project_filaments;
            int                      plate_idx = 0;
            std::vector<std::string> info_messages;
            run_on_main_thread([&]() -> nlohmann::json {
                McpDialogSuppressionGuard suppression;
                Plater*                   plater = wxGetApp().plater();
                if (!plater) {
                    prep_error = "Plater not available";
                    return nlohmann::json::object();
                }
                if (plater->is_background_process_slicing()) {
                    prep_error = "Slicing is still in progress. Wait for slicing to complete before sending to printer.";
                    return nlohmann::json::object();
                }
                PartPlate* plate = plater->get_partplate_list().get_curr_plate();
                if (plate == nullptr || !plate->is_slice_result_valid()) {
                    prep_error = "Plate is not sliced; run slice_all first";
                    return nlohmann::json::object();
                }
                plate_idx = plater->get_partplate_list().get_curr_plate_index();
                if (!resolve_print_host_config(cfg, host_type, prep_error))
                    return nlohmann::json::object();

                // Material mapping is a Flashforge-only concern, and so is this snapshot.
                if (host_type == "flashforge")
                    project_filaments = gather_project_filaments(prep_error);
                info_messages = suppression.messages();
                return nlohmann::json::object();
            });
            if (!prep_error.empty())
                return error_response(prep_error);

            // Off the main thread: the material station round-trip, so the GUI is never blocked on it.
            std::unique_ptr<Slic3r::PrintHost> host = make_print_host(cfg);
            if (!host)
                return error_response("Failed to create a print host for type '" + host_type + "'");

            if (start_print && !host->get_post_upload_actions().has(Slic3r::PrintHostPostUploadAction::StartPrint))
                return error_response("Print host type '" + host_type +
                                      "' does not support starting a print after upload; pass start_print=false");

            std::map<std::string, std::string> extended_info;
            nlohmann::json                     mappings_payload = nlohmann::json::array();
            nlohmann::json                     mappings_report  = nlohmann::json::array();
            auto*                              ff = dynamic_cast<Slic3r::Flashforge*>(host.get());
            if (ff != nullptr && ff->has_local_api_credentials()) {
                Slic3r::FlashforgeApi::PrinterStatus status;
                wxString                             msg;
                if (!ff->fetch_status(status, msg))
                    return error_response(msg.empty() ? "Failed to read printer status" : to_std(msg));

                const bool use_material_station = params.value("use_material_station", status.has_material_station);
                if (use_material_station) {
                    nlohmann::json mapping_error;
                    if (!resolve_material_mappings(requested_mappings, status.slots, project_filaments,
                                                   mappings_payload, mapping_error, &mappings_report))
                        return mapping_error;
                }

                // Exactly the keys FlashforgePrintHostSendDialog::extendedInfo() produces. The dialog's
                // time-lapse checkbox has no tool parameter, so it stays off.
                extended_info = {{"levelingBeforePrint", leveling ? "1" : "0"},
                                 {"timeLapseVideo", "0"},
                                 {"useMatlStation", use_material_station ? "1" : "0"},
                                 {"gcodeToolCnt", std::to_string(mappings_payload.size())},
                                 {"materialMappings", mappings_payload.dump()}};
            }

            // Main thread again: exporting the plate and queueing the upload are Plater work.
            std::string    send_error;
            std::string    uploaded_file_name;
            const nlohmann::json sent = run_on_main_thread([&]() -> nlohmann::json {
                McpDialogSuppressionGuard suppression;
                Plater*                   plater = wxGetApp().plater();
                if (!plater) {
                    send_error = "Plater not available";
                    return {{"ok", false}};
                }
                const bool ok = plater->send_gcode_direct(plate_idx, extended_info,
                                                          start_print ? Slic3r::PrintHostPostUploadAction::StartPrint
                                                                      : Slic3r::PrintHostPostUploadAction::None,
                                                          send_error, file_name, &uploaded_file_name);
                const std::vector<std::string> messages = suppression.messages();
                info_messages.insert(info_messages.end(), messages.begin(), messages.end());
                return {{"ok", ok}};
            });
            if (!sent.value("ok", false))
                return error_response(send_error.empty() ? "Failed to queue the upload" : send_error);

            nlohmann::json response = {{"status", "queued"},
                                       {"host_type", host_type},
                                       {"file_name", uploaded_file_name},
                                       {"material_mappings", mappings_report},
                                       {"start_print", start_print},
                                       {"note", "Upload progress is shown in OrcaSlicer; poll get_printer_status."}};
            if (!info_messages.empty())
                response["info_messages"] = info_messages;
            return response;
        }
    });

    // discover_printers - Find Flashforge printers on the local network
    register_tool({
        "discover_printers",
        ToolCategory::Printers,
        "Find Flashforge printers on the LAN",
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
        ToolCategory::Printers,
        "Save a print host as a printer preset",
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
                {"obico_url", {
                    {"type", "string"},
                    {"description", "Flashforge only: base URL of a self-hosted Obico server that watches this printer "
                                    "(e.g. http://10.0.0.2:3334). Give it together with obico_token; pass \"\" for both to clear."}
                }},
                {"obico_token", {
                    {"type", "string"},
                    {"description", "Flashforge only: the printer's Obico auth token. Never returned by any tool."}
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
            const std::optional<std::string> obico_url     = optional_string(params, "obico_url");
            const std::optional<std::string> obico_token   = optional_string(params, "obico_token");

            if (name.empty() || host.empty() || host_type.empty())
                return error_response("name, host and host_type are required");
            if (obico_url.has_value() != obico_token.has_value() ||
                (obico_url.has_value() && obico_url->empty() != obico_token->empty()))
                return error_response("obico_url and obico_token go together: give both, or pass \"\" for both to clear");
            if (std::find(kSupportedHostTypes.begin(), kSupportedHostTypes.end(), host_type) == kSupportedHostTypes.end())
                return error_response("Unsupported host_type '" + host_type + "'. Supported: " +
                                      boost::algorithm::join(kSupportedHostTypes, ", "));

            return run_on_main_thread([name, host, host_type, serial_number, api_key, printer_preset, obico_url,
                                       obico_token]() -> nlohmann::json {
                McpDialogSuppressionGuard suppression;
                const std::string error = save_print_host_preset(name, host, host_type, serial_number, api_key,
                                                                 printer_preset, obico_url, obico_token);
                if (!error.empty())
                    return error_response(error);
                return select_print_host_preset(name);
            });
        }
    });

    // get_printer_status - Live status from the configured print host (full detail for Flashforge)
    register_tool({
        "get_printer_status",
        ToolCategory::Printers,
        "Live printer state and temperatures",
        "Get live status from the configured print host: state, progress, temperatures, light, material "
        "station. Full detail is only available for Flashforge hosts; other host types report online/offline. "
        "When a Flashforge cannot be read, the error says why and what to do next, and `cached` carries the "
        "material station from its last answer, with age_s.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json&) -> nlohmann::json {
            std::unique_ptr<Slic3r::PrintHost> host;
            DynamicPrintConfig                 cfg;
            std::string                        host_type;
            nlohmann::json                     error_out;
            if (!resolve_print_host(host, cfg, host_type, error_out))
                return error_out;

            const std::string print_host_value = cfg.has("print_host") ? cfg.opt_string("print_host") : std::string();

            auto* ff = dynamic_cast<Slic3r::Flashforge*>(host.get());
            if (!ff) {
                wxString test_msg;
                const bool online = host->test(test_msg);
                return {{"status", "success"},
                        {"host_type", host_type},
                        {"print_host", print_host_value},
                        {"online", online},
                        {"printer", nullptr},
                        {"obico", obico_status_json(cfg)},
                        {"note", "Status details are only implemented for Flashforge hosts"}};
            }

            if (!ff->has_local_api_credentials())
                return flashforge_credentials_error();

            Slic3r::FlashforgeApi::PrinterStatus status;
            wxString                     msg;
            if (!ff->fetch_status(status, msg)) {
                nlohmann::json error = error_response(msg.empty() ? "Failed to fetch printer status" : to_std(msg));
                if (const auto cached = ff->last_known_status())
                    error["cached"] = cached_station_json(*cached);
                return error;
            }

            return {{"status", "success"},
                    {"host_type", host_type},
                    {"print_host", print_host_value},
                    {"online", true},
                    {"obico", obico_status_json(cfg)},
                    {"printer", status_to_json(status)}};
        }
    });

    // printer_control - Pause/resume/cancel the current job, toggle the light, or set temperatures
    register_tool({
        "printer_control",
        ToolCategory::Printers,
        "Pause, resume, cancel, light, temps",
        "Control the Flashforge printer: pause, resume or cancel the current job, turn the enclosure "
        "light on/off, or set bed/chamber/nozzle target temperatures.",
        {
            {"type", "object"},
            {"properties", {
                {"action", {
                    {"type", "string"},
                    {"enum", {"pause", "resume", "cancel", "light_on", "light_off", "set_temperature"}},
                    {"description", "Control action to perform"}
                }},
                {"bed", {
                    {"type", "number"},
                    {"description", "set_temperature only: target bed temperature. Omit for no change."}
                }},
                {"chamber", {
                    {"type", "number"},
                    {"description", "set_temperature only: target chamber temperature. Omit for no change."}
                }},
                {"nozzles", {
                    {"type", "array"},
                    {"description", "set_temperature only: per-tool target temperatures. Tools not listed are left unchanged."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"tool", {{"type", "integer"}, {"description", "Tool/nozzle index, 0-3"}}},
                            {"temp", {{"type", "number"}, {"description", "Target temperature"}}}
                        }},
                        {"required", {"tool", "temp"}}
                    }}
                }}
            }},
            {"required", {"action"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            static const std::vector<std::string> kActions = {"pause", "resume", "cancel", "light_on", "light_off", "set_temperature"};
            if (!params.contains("action") || !params.at("action").is_string())
                return error_response("action must be a string");
            const std::string action = params.at("action").get<std::string>();
            if (std::find(kActions.begin(), kActions.end(), action) == kActions.end())
                return error_response("Unknown action '" + action + "'. Supported: " + boost::algorithm::join(kActions, ", "));

            std::optional<double>              bed;
            std::optional<double>              chamber;
            std::vector<std::optional<double>> nozzles(4, std::nullopt);
            if (action == "set_temperature") {
                // parse_double_param, and an error rather than a skip: this used to ignore a bed
                // or chamber value that was not a JSON number, so a client sending "60" as a string
                // got "success" and a printer that never heated.
                double parsed = 0.0;
                if (params.contains("bed")) {
                    if (!parse_double_param(params.at("bed"), parsed))
                        return error_response("bed must be a finite number, in degrees C");
                    bed = parsed;
                }
                if (params.contains("chamber")) {
                    if (!parse_double_param(params.at("chamber"), parsed))
                        return error_response("chamber must be a finite number, in degrees C");
                    chamber = parsed;
                }
                if (params.contains("nozzles") && params.at("nozzles").is_array()) {
                    for (const auto& entry : params.at("nozzles")) {
                        if (!entry.is_object() || !entry.contains("tool") || !entry.contains("temp"))
                            return error_response("Each entry in nozzles requires 'tool' and 'temp'");
                        // parse_integer_param, not is_number_integer(): a client whose JSON layer
                        // widens numbers sends 0 as 0.0. The sibling temp check below is already
                        // permissive, so the strict one here refused entries its own pair accepted.
                        int tool = 0;
                        if (!parse_integer_param(entry.at("tool"), tool))
                            return error_response("nozzles[].tool must be an integer");
                        double temp = 0.0;
                        if (!parse_double_param(entry.at("temp"), temp))
                            return error_response("nozzles[].temp must be a finite number, in degrees C");
                        if (tool < 0 || tool > 3)
                            return error_response("nozzles[].tool must be between 0 and 3");
                        nozzles[tool] = temp;
                    }
                }
            }

            std::unique_ptr<Slic3r::PrintHost> host;
            Slic3r::Flashforge*                ff = nullptr;
            nlohmann::json                     error_out;
            if (!resolve_flashforge(host, ff, error_out))
                return error_out;

            wxString msg;
            bool     ok = false;
            if (action == "pause")
                ok = ff->pause_job(msg);
            else if (action == "resume")
                ok = ff->resume_job(msg);
            else if (action == "cancel")
                ok = ff->cancel_job(msg);
            else if (action == "light_on")
                ok = ff->set_light(true, msg);
            else if (action == "light_off")
                ok = ff->set_light(false, msg);
            else // set_temperature
                ok = ff->set_temperatures(bed, chamber, nozzles, msg);

            if (!ok)
                return error_response(msg.empty() ? ("Failed to perform action '" + action + "'") : to_std(msg));
            return {{"status", "success"}, {"action", action}};
        }
    });

    // list_printer_files - G-code files stored on the Flashforge printer
    register_tool({
        "list_printer_files",
        ToolCategory::Printers,
        "G-code files stored on the printer",
        "List G-code files stored on the Flashforge printer.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json&) -> nlohmann::json {
            std::unique_ptr<Slic3r::PrintHost> host;
            Slic3r::Flashforge*                ff = nullptr;
            nlohmann::json                     error_out;
            if (!resolve_flashforge(host, ff, error_out))
                return error_out;

            std::vector<std::string> files;
            wxString                 msg;
            if (!ff->list_gcode_files(files, msg))
                return error_response(msg.empty() ? "Failed to list printer files" : to_std(msg));
            return {{"status", "success"}, {"files", files}};
        }
    });

    // print_printer_file - Start printing a G-code file already on the Flashforge printer
    register_tool({
        "print_printer_file",
        ToolCategory::Printers,
        "Start printing a file on the printer",
        "Start printing a G-code file already stored on the Flashforge printer, with optional material "
        "station mapping.",
        {
            {"type", "object"},
            {"properties", {
                {"file_name", {
                    {"type", "string"},
                    {"description", "File name as returned by list_printer_files"}
                }},
                {"leveling_before_print", {
                    {"type", "boolean"},
                    {"description", "Run bed leveling before printing (default false)"}
                }},
                {"material_mappings", {
                    {"type", "array"},
                    {"description", "Explicit tool-to-slot mapping. Overrides auto_map when provided."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"tool_id", {{"type", "integer"}, {"description", "Project filament/tool index, 0-based"}}},
                            {"slot_id", {{"type", "integer"}, {"description", "Material station slot id"}}}
                        }},
                        {"required", {"tool_id", "slot_id"}}
                    }}
                }},
                {"auto_map", {
                    {"type", "boolean"},
                    {"description", "When true (default) and material_mappings is not given, build the mapping "
                                    "from the current project's filament types matched against the material "
                                    "station's loaded slots."}
                }}
            }},
            {"required", {"file_name"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            if (!params.contains("file_name") || !params.at("file_name").is_string())
                return error_response("file_name must be a string");
            const std::string file_name = params.at("file_name").get<std::string>();
            if (file_name.empty())
                return error_response("file_name is required");

            if (params.contains("leveling_before_print") && !params.at("leveling_before_print").is_boolean())
                return error_response("leveling_before_print must be a boolean");
            const bool leveling = params.value("leveling_before_print", false);

            if (params.contains("auto_map") && !params.at("auto_map").is_boolean())
                return error_response("auto_map must be a boolean");
            const bool auto_map = params.value("auto_map", true);

            const nlohmann::json requested_mappings = params.value("material_mappings", nlohmann::json::array());
            if (!requested_mappings.is_array())
                return error_response("material_mappings must be an array");

            std::unique_ptr<Slic3r::PrintHost> host;
            Slic3r::Flashforge*                ff = nullptr;
            nlohmann::json                     error_out;
            if (!resolve_flashforge(host, ff, error_out))
                return error_out;

            nlohmann::json mappings_payload = nlohmann::json::array();
            nlohmann::json mappings_report  = nlohmann::json::array();
            if (!requested_mappings.empty() || auto_map) {
                Slic3r::FlashforgeApi::PrinterStatus status;
                wxString                             msg;
                if (!ff->fetch_status(status, msg))
                    return error_response(msg.empty() ? "Failed to read material station status" : to_std(msg));

                // The project's own tool count/types, needed both to auto-map and to check an explicit
                // mapping's tool_id is one of this project's actual tools.
                std::string filaments_error;
                const nlohmann::json project_filaments = run_on_main_thread([&]() -> nlohmann::json {
                    return gather_project_filaments(filaments_error);
                });
                if (!filaments_error.empty())
                    return error_response(filaments_error);

                // Build and validate the mapping the same way send_to_printer does.
                nlohmann::json mapping_error;
                if (!resolve_material_mappings(requested_mappings, status.slots, project_filaments,
                                               mappings_payload, mapping_error, &mappings_report))
                    return mapping_error;
            }

            wxString msg;
            if (!ff->print_gcode_file(file_name, leveling, mappings_payload, msg))
                return error_response(msg.empty() ? "Failed to start print" : to_std(msg));

            return {{"status", "success"}, {"file_name", file_name}, {"material_mappings", mappings_report}};
        }
    });
    // match_project_to_printer - Make the project's filament slots say what the machine actually holds
    register_tool({
        "match_project_to_printer",
        ToolCategory::Printers,
        "Match slots to the printer's filaments",
        "Match the project's filament slots to the printer's material station: for every loaded slot, "
        "pick a filament preset of the material the printer reports and set that slot's colour to the "
        "colour it reports. Slots the printer reports as empty are left untouched. Fixes the two things "
        "a stale project causes: send_to_printer refusing on a material mismatch, and a plate preview in "
        "the wrong colour. When the printer cannot be read live, the plan comes from its last known status "
        "(source: cached, with age_s and live_error) and changes the project only with allow_cached: true; "
        "otherwise it is returned as a dry run.",
        {
            {"type", "object"},
            {"properties", {
                {"slots", {
                    {"type", "array"},
                    {"description", "1-based material-station slot ids to match. Omit for every loaded slot."},
                    {"items", {{"type", "integer"}}}
                }},
                {"dry_run", {
                    {"type", "boolean"},
                    {"description", "Report the plan without changing anything (default false). Each slot's "
                                    "'changed' then means 'would change'."}
                }},
                {"allow_cached", {
                    {"type", "boolean"},
                    {"description", "Apply a plan made from the printer's last known status when it cannot be read "
                                    "live (default false). That status may be stale; age_s says how old it is."}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::vector<int> slots;
            if (params.contains("slots") && !params["slots"].is_null()) {
                if (!params["slots"].is_array())
                    return error_response("slots must be an array of 1-based material-station slot ids");
                for (const auto& value : params["slots"]) {
                    int slot = 0;
                    if (!parse_integer_param(value, slot) || slot < 1)
                        return error_response("slots must contain 1-based integers, got: " + value.dump());
                    slots.push_back(slot);
                }
            }

            bool dry_run      = false;
            bool allow_cached = false;
            for (auto [key, target] : {std::pair<const char*, bool*>{"dry_run", &dry_run}, {"allow_cached", &allow_cached}})
                if (params.contains(key) && !params[key].is_null() && !parse_boolean_param(params[key], *target))
                    return error_response(std::string(key) + " must be a boolean, got: " + params[key].dump());

            std::unique_ptr<Slic3r::PrintHost> host;
            Slic3r::Flashforge*                ff = nullptr;
            nlohmann::json                     error_out;
            if (!resolve_flashforge(host, ff, error_out))
                return error_out;

            Slic3r::FlashforgeApi::PrinterStatus status;
            wxString                             msg;
            if (!ff->fetch_status(status, msg)) {
                const std::string live_error = msg.empty() ? "Failed to read material station status" : to_std(msg);
                const auto        cached     = ff->last_known_status();
                if (!cached)
                    return error_response(live_error);
                return match_from_cached_status(*cached, slots, dry_run, allow_cached, live_error);
            }

            // Params and the station snapshot cross to the GUI thread by value; everything the match
            // touches (presets, project config, sidebar) is main-thread-only.
            nlohmann::json response = run_on_main_thread([station = status.slots, slots, dry_run]() -> nlohmann::json {
                return match_project_to_printer(station, slots, dry_run);
            });
            response["source"] = "live";
            return response;
        }
    });
}
