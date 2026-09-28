// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterControl.hpp
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

// printer_control's decisions, apart from the printer, so they are tested without one
// (tests/slic3rutils/test_mcp_printer_control.cpp). Every action becomes the Device page's own command
// params, which build_console_operation (FlashforgeConsoleHandler.hpp) turns into the printer call with
// the page's limits, and run_console_operation makes: the page and the tool send the same commands.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// One printer_control call, as the Device page would send it.
struct PrinterControlRequest
{
    std::string    action;
    nlohmann::json console_params; // {"name": "light" | "job" | "temperature" | "filtration" | "printer_ctl", ...}
    // A control that carries every field it owns (printerCtl_cmd, circulateCtl_cmd) reads the others from
    // the printer's status first, and answers with what it sent and what the printer reported before.
    bool reads_status = false;
};

// printer_control's `params` as the Device page's command params, or why the call is refused: an unknown
// action, an argument of another action, a value that is not what it names, or nothing to set. Nothing is
// sent to the printer before this has passed.
std::optional<std::string> printer_control_request(const nlohmann::json& params, PrinterControlRequest& out);

// Why a status-reading action (reads_status) must not go out on `snapshot` (console_snapshot's): it lacks
// a field the command sends back as the printer reports it, which build_console_operation would send as 0
// (or "close") -- a Z offset, a print speed, the part-cooling fan, the Pro's chamber fan, the other
// filtration fan. The Device page sends those as it does; an agent's call is refused, naming them.
std::optional<std::string> status_refusal(const PrinterControlRequest& request, const nlohmann::json& snapshot);

// The live controls a Flashforge status reports, from its `raw` (status_to_json's allowlist), named as
// printer_control takes them: print_speed_percent (null while no job runs: the printer reports 0),
// z_offset_mm, chamber_fan_percent, cooling_fan_percent, cooling_left_fan_percent (only on a printer
// with one), recirculation and exhaust (the filtration fans, on or off). A field the printer does not
// report is null.
nlohmann::json printer_controls_json(const nlohmann::json& raw);

}}} // namespace Slic3r::GUI::OrcaMCP
