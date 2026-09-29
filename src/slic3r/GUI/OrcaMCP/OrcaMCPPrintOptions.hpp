// src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/FlashforgeApi.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// How send_to_printer and print_printer_file decide what a Flashforge does before and during the
// print they start. The agent decides: an explicit boolean always wins. Left to the tool, leveling
// and flow calibration run for a print estimated at kCalibrationGateSeconds or longer, where the
// minutes they add are small beside what a failed print wastes -- on a Creator 5 or 5 Pro only, the
// machines whose start screen was checked (FlashforgeApi::start_screen_offers_calibration) -- and
// time-lapse stays off. Pure and any thread; tests/slic3rutils/test_mcp_print_options.cpp pins it.

constexpr double kCalibrationGateSeconds = 4 * 3600.0;

// The tool parameters, as the schemas declare them and read_print_option_request reads them.
constexpr const char* kLevelingParam        = "leveling_before_print";
constexpr const char* kFlowCalibrationParam = "flow_calibration";
constexpr const char* kTimeLapseParam       = "time_lapse";

// The gate as the tool descriptions state it ("4 h"), so they cannot drift from the constant.
std::string calibration_gate_text();

// The tool parameters as the caller gave them; unset when omitted.
struct PrintOptionRequest
{
    std::optional<bool> leveling;         // leveling_before_print
    std::optional<bool> flow_calibration; // flow_calibration
    std::optional<bool> time_lapse;       // time_lapse
};

// Reads the three parameters from a tool's `params`. False with `error` ("<name> must be a
// boolean") when one is present and is not a boolean.
bool read_print_option_request(const nlohmann::json& params, PrintOptionRequest& out, std::string& error);

// Whether choose_print_options reads the estimate: some calibration was left to the gate.
bool needs_print_time(const PrintOptionRequest& request);

// Where a send's print options go.
enum class PrintOptionsReach
{
    Honoured,                  // a direct send to a Flashforge with local-API credentials
    SendDialog,                // direct: false: the user's send dialog asks for its own
    UploadOnly,                // start_print: false: nothing starts, so the printer never reads them
    NotFlashforge,             // another print host takes none
    FlashforgeWithoutLocalApi, // no serial number and check code: the TCP console carries none
};

// The refusal of each explicit `true` a send would silently drop, naming the parameters and why;
// nullopt when the send carries them, or none is asked for. An explicit false is never refused: it is
// what happens anyway. `host_type` names the host for NotFlashforge.
std::optional<std::string> ignored_print_options_refusal(const PrintOptionRequest& request, PrintOptionsReach reach,
                                                         const std::string& host_type);

// What the gate reads about the print and the printer.
struct GateFacts
{
    std::optional<double> estimated_print_s;            // unset or not positive when unknown
    bool                  model_offers_calibration{false}; // FlashforgeApi::start_screen_offers_calibration
    std::string           printer_model;                // as the report names it (FlashforgeApi::printer_model_name); empty: not read
};

// One option as decided, and by what: "caller", "print_time_gate", "print_time_unknown" (left to
// the gate with no estimate, so off), "not_offered_by_model" (left to the gate on a machine whose
// start screen nobody checked, so off) or "default" (time-lapse left to the tool, so off).
struct PrintOptionChoice
{
    bool        on{false};
    std::string decided_by;
};

struct PrintOptionChoices
{
    PrintOptionChoice leveling;
    PrintOptionChoice flow_calibration;
    PrintOptionChoice time_lapse;
};

PrintOptionChoices choose_print_options(const PrintOptionRequest& request, const GateFacts& facts);

FlashforgeApi::PrintOptions to_print_options(const PrintOptionChoices& choices);

// The response's `print_options`: each option's {on, decided_by}, the estimate the gate read
// (`estimated_print_s`, whole seconds, null when unknown), the gate (`gate_s`) and the machine
// (`printer_model`).
nlohmann::json print_options_json(const PrintOptionChoices& choices, const GateFacts& facts);

}}} // namespace Slic3r::GUI::OrcaMCP
