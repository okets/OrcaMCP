// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPGcodeCheck.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPrinterControl.hpp"
#include "OrcaMCPPrinterUtils.hpp"
#include "OrcaMCPPrintOptions.hpp"
#include "OrcaMCPProjectMatch.hpp"
#include "OrcaMCPSliceEstimate.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/FlashforgeConsoleHandler.hpp"
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
// main thread. The host copies what it needs from the local `cfg`, so it outlives it safely.
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

// Why a send of plates first..last is refused: the first of them whose slice's check of its own
// G-code failed, which keeps the GUI's Print and Send buttons off too. A plate without a slice result
// has not been checked; the callers decide what an unsliced plate means.
std::optional<std::string> gcode_check_send_refusal(GUI::PartPlateList& plates, int first, int last)
{
    for (int index = first; index <= last; ++index)
        if (PartPlate* plate = plates.get_plate(index))
            if (std::optional<std::string> refusal = plate_gcode_check_refusal(*plate, index))
                return "The send did not start: " + *refusal;
    return std::nullopt;
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

        GUI::PartPlateList& plates = plater->get_partplate_list();
        const int           first  = all_plates ? 0 : plates.get_curr_plate_index();
        const int           last   = all_plates ? plates.get_plate_count() - 1 : first;
        if (std::optional<std::string> refusal = gcode_check_send_refusal(plates, first, last))
            return error_response(*refusal);

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

    nlohmann::json response = run_on_main_thread([station = cached.value.slots, slots, applies]() -> nlohmann::json {
        return match_project_to_printer(station, slots, /*dry_run=*/!applies);
    });
    return label_cached_match(std::move(response), cached.age_s, live_error, withheld);
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

// One entry of material_mappings, as send_to_printer and print_printer_file take it, and as they report
// it (mapping_report): the reported list can be sent back as it is. Any other key is refused: a
// misspelled one would otherwise leave the pair unmapped and the print on the wrong spool.
nlohmann::json material_mapping_schema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"tool_id", {{"type", "integer"}, {"description", "Project filament/tool index, 0-based"}}},
            {"slot_id", {{"type", "integer"}, {"description", "Material station slot id"}}},
            {"color_delta_e", {{"type", {"number", "null"}},
                               {"description", "Accepted and ignored: the colour match a send reports per pair. Here so "
                                               "the material_mappings a response lists can be sent back as they are."}}}
        }},
        {"required", {"tool_id", "slot_id"}},
        {"additionalProperties", false}
    };
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
                    return suppression.report(select_print_host_preset(physical_printer));
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
        "yourself). Pass direct=false to open OrcaSlicer's send dialog and leave the send to the user. "
        "Refused, as the GUI's Print and Send buttons are off, when a plate it would send failed the check its "
        "slice ran on its G-code (get_slicing_status's plates[].gcode_check). "
        "On a Flashforge the printer can calibrate the flow and level the bed before it starts; each adds minutes, "
        "and the choice is yours: pass flow_calibration and leveling_before_print true before a long print or after "
        "a filament, nozzle or bed change, false for a short print or a repeat soon after the last one on the same "
        "filaments (get_printer_status's last_print_started_here says when this OrcaMCP instance last started a print "
        "there, and with what). Omitted, each runs on a Creator 5 or 5 Pro when the plate's estimated time is " +
        calibration_gate_text() + " or more; other Flashforge models have not been checked, so there each stays off "
        "unless asked for. time_lapse is off unless asked for. The response's print_options says what was sent and "
        "why. An explicit true is refused where it would be ignored: direct=false, start_print=false (nothing "
        "starts; pass them to print_printer_file then), another print host, or a Flashforge without its serial "
        "number and check code; so are use_material_station: true and a material_mappings list, except with "
        "start_print=false, where the upload carries them. With start_print=false every print option is sent off.",
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
                {kLevelingParam, {
                    {"type", "boolean"},
                    {"description", "Level the bed before printing. Omitted: on a Creator 5 or 5 Pro, on when the plate's "
                                    "estimated time is " + calibration_gate_text() + " or more; off on other models. "
                                    "Direct sends that start the print on a Flashforge host with local-API credentials "
                                    "only; true is refused anywhere else."}
                }},
                {kFlowCalibrationParam, {
                    {"type", "boolean"},
                    {"description", "Calibrate the flow before printing. Omitted: on a Creator 5 or 5 Pro, on when the "
                                    "plate's estimated time is " + calibration_gate_text() + " or more; off on other "
                                    "models. Direct sends that start the print on a Flashforge host with local-API "
                                    "credentials only; true is refused anywhere else."}
                }},
                {kTimeLapseParam, {
                    {"type", "boolean"},
                    {"description", "Ask the printer to record a time-lapse (default false). Direct sends that start "
                                    "the print on a Flashforge host with local-API credentials only; true is refused "
                                    "anywhere else."}
                }},
                {"use_material_station", {
                    {"type", "boolean"},
                    {"description", "Print from the material station (default: true when the printer reports "
                                    "one). Direct sends to a Flashforge host with local-API credentials only; "
                                    "true is refused anywhere else."}
                }},
                {"material_mappings", {
                    {"type", "array"},
                    {"description", "Explicit tool-to-slot mapping. Omit to map the project's filaments onto "
                                    "matching loaded slots automatically. Direct sends to a Flashforge host "
                                    "with local-API credentials only; a mapping is refused anywhere else."},
                    {"items", material_mapping_schema()}
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
            PrintOptionRequest option_request;
            std::string        option_error;
            if (!read_print_option_request(params, option_request, option_error))
                return error_response(option_error);
            for (const char* flag : {"start_print", "use_material_station"})
                if (params.contains(flag) && !params.at(flag).is_boolean())
                    return error_response(std::string(flag) + " must be a boolean");
            if (params.contains("file_name") && !params.at("file_name").is_string())
                return error_response("file_name must be a string");
            const nlohmann::json requested_mappings = params.value("material_mappings", nlohmann::json::array());
            if (!requested_mappings.is_array())
                return error_response("material_mappings must be an array");

            // What only a direct send to a Flashforge with local-API credentials carries: asked for anywhere
            // else, it is refused before anything is sent rather than dropped.
            const std::vector<std::string> options_asked = requested_print_options(option_request);
            std::vector<std::string>       flashforge_asked = options_asked;
            for (const std::string& name : requested_station_arguments(params))
                flashforge_asked.push_back(name);

            const bool all_plates = params.value("all_plates", false);
            if (!params.value("direct", true)) {
                if (std::optional<std::string> refusal = ignored_arguments_refusal(flashforge_asked, SendReach::SendDialog, std::string()))
                    return error_response(*refusal);
                return open_send_dialog(all_plates);
            }

            if (all_plates)
                return error_response("direct send supports one plate at a time; select the plate first with "
                                      "select_plate, or pass direct=false to send all plates from the dialog");

            const bool        start_print = params.value("start_print", true);
            const std::string file_name   = params.value("file_name", std::string());
            if (!start_print)
                if (std::optional<std::string> refusal = ignored_arguments_refusal(options_asked, SendReach::UploadOnly, std::string()))
                    return error_response(*refusal);

            // Main thread: everything that reads the plater and the presets.
            DynamicPrintConfig       cfg;
            std::string              host_type;
            std::string              prep_error;
            nlohmann::json           project_filaments;
            int                      plate_idx = 0;
            std::optional<double>    estimated_print_s;
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
                if (std::optional<std::string> refusal = gcode_check_send_refusal(plater->get_partplate_list(), plate_idx, plate_idx)) {
                    prep_error = *refusal;
                    return nlohmann::json::object();
                }
                if (const GCodeProcessorResult* result = plate->get_slice_result())
                    estimated_print_s = normal_mode_print_time_s(result->print_statistics);
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

            // Before anything reaches the printer: what this host would drop.
            auto* ff = dynamic_cast<Slic3r::Flashforge*>(host.get());
            const SendReach reach = ff == nullptr                    ? SendReach::NotFlashforge :
                                    !ff->has_local_api_credentials() ? SendReach::FlashforgeWithoutLocalApi :
                                                                       SendReach::Honoured;
            if (std::optional<std::string> refusal = ignored_arguments_refusal(flashforge_asked, reach, host_type))
                return error_response(*refusal);

            if (start_print && !host->get_post_upload_actions().has(Slic3r::PrintHostPostUploadAction::StartPrint))
                return error_response("Print host type '" + host_type +
                                      "' does not support starting a print after upload; pass start_print=false");

            std::map<std::string, std::string> extended_info;
            nlohmann::json                     mappings_payload = nlohmann::json::array();
            nlohmann::json                     mappings_report  = nlohmann::json::array();
            std::optional<PrintOptionChoices>  print_options;
            GateFacts                          gate_facts;
            gate_facts.estimated_print_s = estimated_print_s;
            gate_facts.starts_print      = start_print;
            if (reach == SendReach::Honoured) {
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

                gate_facts.model_offers_calibration = Slic3r::FlashforgeApi::start_screen_offers_calibration(status.pid);
                gate_facts.printer_model            = Slic3r::FlashforgeApi::printer_model_name(status);
                print_options = choose_print_options(option_request, gate_facts);
                extended_info = Slic3r::FlashforgeApi::make_upload_extended_info(to_print_options(*print_options),
                                                                                 use_material_station, mappings_payload);
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
                // Judged again: the plate may have been sliced anew while the printer was asked for its slots.
                if (std::optional<std::string> refusal = gcode_check_send_refusal(plater->get_partplate_list(), plate_idx, plate_idx)) {
                    send_error = *refusal;
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
            if (print_options)
                response["print_options"] = print_options_json(*print_options, gate_facts);
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
                    return suppression.report(error_response(error));
                return suppression.report(select_print_host_preset(name));
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
        "When a Flashforge cannot be reached, the error says why and what to do next, and `cached` carries "
        "the material station from its last answer, with age_s. An answer that refuses (wrong check code) "
        "or cannot be read is returned as that error, without `cached`. last_print_started_here, in the answer and "
        "in the error for a printer that could not be reached (the one with `cached`), is the last print this "
        "OrcaMCP instance started on the printer: age_s, file_name, the "
        "leveling, flow_calibration and time_lapse it asked for, and the slots it fed from (fed_from); null when "
        "none since it launched. Prints started on the printer's screen, from another computer or from another "
        "OrcaMCP instance are not in it. Use it to judge whether a new print needs calibrating again. "
        "printer.progress is how far a Flashforge has read through the file, by bytes, not time. For a print this "
        "instance sliced and sent (printer.progress_source \"slice\"), printer.layer / layers, work_done (the "
        "slicer's time done, 0-1) and remaining_s are read off its slice; otherwise (\"printer\") they are the "
        "printer's own, whose layer count can run ahead and whose bytes can be far from the time done. "
        "printer.telemetry_valid false: the printer reported job and control numbers no reading can be, named in "
        "implausible_telemetry (a Flashforge does for a moment after a print starts, states downloading and unzipping); progress, "
        "the times and the numbers in controls are then null, raw keeps what it sent, and next_steps says to read "
        "again in a few seconds.",
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
            bool                         unreachable = false;
            if (!ff->fetch_status(status, msg, &unreachable)) {
                nlohmann::json error = error_response(msg.empty() ? "Failed to fetch printer status" : to_std(msg));
                // Only when the printer could not be talked to: a refusal (wrong check code) or an
                // unreadable answer is its own reply, and a stale status would hide it.
                if (const auto cached = unreachable ? ff->last_known_status() : std::nullopt)
                    error["cached"] = cached_station_json(*cached);
                if (unreachable)
                    error["last_print_started_here"] = print_start_json(ff->last_print_start());
                return error;
            }

            nlohmann::json answer = {{"status", "success"},
                                     {"host_type", host_type},
                                     {"print_host", print_host_value},
                                     {"online", true},
                                     {"obico", obico_status_json(cfg)},
                                     {"printer", status_to_json(status)},
                                     {"last_print_started_here", print_start_json(ff->last_print_start())}};
            add_next_steps(answer, untrusted_status_next_steps(status.implausible_telemetry));
            return answer;
        }
    });

    // printer_control - Pause/resume/cancel the current job, toggle the light, or set temperatures
    register_tool({
        "printer_control",
        ToolCategory::Printers,
        "Job, light, temps, fans, speed, Z offset",
        "Control the Flashforge printer as its Device page does: pause, resume or cancel the current job, turn "
        "the light on or off, set bed/chamber/nozzle target temperatures, switch the filtration fans "
        "(set_filtration: recirculation, exhaust), set the chamber and part-cooling fans (set_fans, 0-100 %), "
        "the print speed of the running job (set_print_speed: 50, 100, 125 or 166 %), or the Z offset "
        "(set_z_offset: mm, positive raises the nozzle, within -1 to 1 in 0.025 mm steps). It acts on the real "
        "printer. The set_* actions change only what they name: the printer's other settings are read first "
        "and sent back as it reports them; the answer gives what was sent and the printer's values before, and "
        "get_printer_status's printer.controls reads them back. Refused, sending nothing: a print speed while "
        "nothing prints, a fan or filtration the printer does not report, a status lacking a field the command "
        "sends back (it would go out as 0) or whose numbers no reading can be (telemetry_valid false), a value "
        "out of range, and an argument of another action.",
        {
            {"type", "object"},
            {"properties", {
                {"action", {
                    {"type", "string"},
                    {"enum", {"pause", "resume", "cancel", "light_on", "light_off", "set_temperature", "set_filtration",
                              "set_fans", "set_print_speed", "set_z_offset"}},
                    {"description", "Control action to perform"}
                }},
                {"bed", {
                    {"type", "number"},
                    {"description", "set_temperature only: target bed temperature, 0-150 C. Omit for no change."}
                }},
                {"chamber", {
                    {"type", "number"},
                    {"description", "set_temperature only: target chamber temperature, 0-100 C. Omit for no change."}
                }},
                {"nozzles", {
                    {"type", "array"},
                    {"description", "set_temperature only: per-tool target temperatures, 0-350 C. Tools not listed are left "
                                    "unchanged. set_temperature needs at least one of bed, chamber or a nozzle."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"tool", {{"type", "integer"}, {"description", "Tool/nozzle index, 0-3"}}},
                            {"temp", {{"type", "number"}, {"description", "Target temperature"}}}
                        }},
                        {"required", {"tool", "temp"}},
                        // A misspelled key would otherwise send the printer "no change" for that tool.
                        {"additionalProperties", false}
                    }}
                }},
                {"recirculation", {
                    {"type", "boolean"},
                    {"description", "set_filtration only: the internal (recirculation) filtration fan on or off. Omit to leave it."}
                }},
                {"exhaust", {
                    {"type", "boolean"},
                    {"description", "set_filtration only: the external (exhaust) filtration fan on or off. Omit to leave it."}
                }},
                {"chamber_fan", {
                    {"type", "number"},
                    {"description", "set_fans only: chamber fan speed, 0-100 %. Omit to leave it."}
                }},
                {"cooling_fan", {
                    {"type", "number"},
                    {"description", "set_fans only: part-cooling fan speed, 0-100 %. Omit to leave it."}
                }},
                {"cooling_left_fan", {
                    {"type", "number"},
                    {"description", "set_fans only: the left part-cooling fan, 0-100 %, on a printer that reports one."}
                }},
                {"speed", {
                    {"type", "integer"},
                    {"enum", {50, 100, 125, 166}},
                    {"description", "set_print_speed only: the running job's speed in percent, the printer's own steps."}
                }},
                {"z_offset", {
                    {"type", "number"},
                    {"description", "set_z_offset only: the Z offset in mm, positive raises the nozzle; -1 to 1 in 0.025 mm steps."}
                }}
            }},
            {"required", {"action"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            // Everything the call asks is checked before the printer is looked up, let alone sent anything.
            PrinterControlRequest request;
            if (const auto refusal = printer_control_request(params, request))
                return error_response(*refusal);

            std::unique_ptr<Slic3r::PrintHost> host;
            Slic3r::Flashforge*                ff = nullptr;
            nlohmann::json                     error_out;
            if (!resolve_flashforge(host, ff, error_out))
                return error_out;

            // A control that carries every field it owns sends the ones it does not change as the
            // printer reports them now, as the Device page does from its last status.
            nlohmann::json snapshot = nlohmann::json::object();
            if (request.reads_status) {
                Slic3r::FlashforgeApi::PrinterStatus status;
                wxString                             msg;
                if (!ff->fetch_status(status, msg))
                    return error_response("Nothing was sent: the printer's current settings could not be read. " +
                                          (msg.empty() ? std::string("Failed to fetch printer status") : to_std(msg)));
                snapshot = console_snapshot(status);
                if (const auto refusal = status_refusal(request, snapshot)) {
                    nlohmann::json answer = error_response(*refusal);
                    add_next_steps(answer, untrusted_status_next_steps(status.implausible_telemetry));
                    return answer;
                }
            }

            nlohmann::json operation;
            std::string    error;
            if (!build_console_operation(request.console_params, snapshot, operation, error))
                return error_response(error + " Nothing was sent.");

            wxString msg;
            if (!run_console_operation(*ff, operation, msg))
                return error_response(msg.empty() ? ("Failed to perform action '" + request.action + "'") : to_std(msg));

            nlohmann::json answer = {{"status", "success"}, {"action", request.action}};
            if (request.reads_status) {
                answer["sent"]   = {{"cmd", operation.value("cmd", std::string())}, {"args", operation.value("args", nlohmann::json::object())}};
                answer["before"] = printer_controls_json(snapshot["printer"]["raw"]);
                add_next_steps(answer, printer_control_next_steps());
            }
            return answer;
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
        std::string("Start printing a G-code file already stored on the Flashforge printer, with optional material "
        "station mapping. Leveling and flow calibration each add minutes before the print; decide them as for "
        "send_to_printer. Omitted, each runs on a Creator 5 or 5 Pro when the file's estimated time, from the "
        "printer's file list, is ") + calibration_gate_text() + " or more, and not when the printer does not report "
        "one; other Flashforge models have not been checked, so there each stays off unless asked for. The "
        "response's print_options says what was sent and why.",
        {
            {"type", "object"},
            {"properties", {
                {"file_name", {
                    {"type", "string"},
                    {"description", "File name as returned by list_printer_files"}
                }},
                {kLevelingParam, {
                    {"type", "boolean"},
                    {"description", "Level the bed before printing. Omitted: on a Creator 5 or 5 Pro, on when the file's "
                                    "estimated time, from the printer's file list, is " + calibration_gate_text() +
                                    " or more; off on other models."}
                }},
                {kFlowCalibrationParam, {
                    {"type", "boolean"},
                    {"description", "Calibrate the flow before printing. Omitted: on a Creator 5 or 5 Pro, on when the "
                                    "file's estimated time, from the printer's file list, is " + calibration_gate_text() +
                                    " or more; off on other models."}
                }},
                {kTimeLapseParam, {
                    {"type", "boolean"},
                    {"description", "Ask the printer to record a time-lapse (default false). Not yet confirmed to take "
                                    "effect for a stored file."}
                }},
                {"material_mappings", {
                    {"type", "array"},
                    {"description", "Explicit tool-to-slot mapping. Overrides auto_map when provided."},
                    {"items", material_mapping_schema()}
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

            PrintOptionRequest option_request;
            std::string        option_error;
            if (!read_print_option_request(params, option_request, option_error))
                return error_response(option_error);

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

            // One status read serves the mapping and the gate, which needs to know which machine this is.
            const bool                           needs_mapping = !requested_mappings.empty() || auto_map;
            const bool                           needs_gate    = needs_print_time(option_request);
            Slic3r::FlashforgeApi::PrinterStatus status;
            if (needs_mapping || needs_gate) {
                wxString msg;
                if (!ff->fetch_status(status, msg))
                    return error_response(msg.empty() ? "Failed to read printer status" : to_std(msg));
            }

            nlohmann::json mappings_payload = nlohmann::json::array();
            nlohmann::json mappings_report  = nlohmann::json::array();
            if (needs_mapping) {
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

            // The gate needs the file's own estimate, and only the printer's file list has it: read once the
            // mapping is settled, and only where the gate can turn calibration on. A list that cannot be read
            // leaves the estimate unknown; the print request below reports a dead printer.
            GateFacts gate_facts;
            if (needs_mapping || needs_gate) {
                gate_facts.model_offers_calibration = Slic3r::FlashforgeApi::start_screen_offers_calibration(status.pid);
                gate_facts.printer_model            = Slic3r::FlashforgeApi::printer_model_name(status);
            }
            if (needs_gate && gate_facts.model_offers_calibration) {
                std::optional<long> printing_time_s;
                wxString            list_msg;
                if (ff->stored_file_printing_time(file_name, printing_time_s, list_msg) && printing_time_s)
                    gate_facts.estimated_print_s = double(*printing_time_s);
            }
            const PrintOptionChoices print_options = choose_print_options(option_request, gate_facts);

            wxString msg;
            if (!ff->print_gcode_file(file_name, to_print_options(print_options), mappings_payload, msg))
                return error_response(msg.empty() ? "Failed to start print" : to_std(msg));

            return {{"status", "success"},
                    {"file_name", file_name},
                    {"material_mappings", mappings_report},
                    {"print_options", print_options_json(print_options, gate_facts)}};
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
        "the wrong colour. When the printer cannot be reached, the plan comes from its last known status "
        "(source: cached, with age_s and live_error) and changes the project only with allow_cached: true; "
        "a real run without it returns status not_applied with the plan and changes nothing.",
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
            bool                                 unreachable = false;
            if (!ff->fetch_status(status, msg, &unreachable)) {
                const std::string live_error = msg.empty() ? "Failed to read material station status" : to_std(msg);
                // The last known status stands in only for a printer that could not be reached; its
                // own refusal (wrong check code, HTTP error, unreadable answer) is returned as is.
                const auto cached = unreachable ? ff->last_known_status() : std::nullopt;
                if (!cached)
                    return error_response(live_error);
                return match_from_cached_status(*cached, slots, dry_run, allow_cached, live_error);
            }

            // Params and the station snapshot cross to the GUI thread by value; everything the match
            // touches (presets, project config, sidebar) is main-thread-only.
            nlohmann::json response = run_on_main_thread([station = status.slots, slots, dry_run]() -> nlohmann::json {
                nlohmann::json answer = match_project_to_printer(station, slots, dry_run);
                std::vector<int> missing;
                for (const nlohmann::json& slot : answer.value("slots", nlohmann::json::array()))
                    if (!slot.value("in_project", true))
                        missing.push_back(slot.value("slot", 0));
                add_next_steps(answer, missing_slot_next_steps(missing, Sidebar::should_show_SEMM_buttons()));
                return answer;
            });
            response["source"] = "live";
            return response;
        }
    });
}
