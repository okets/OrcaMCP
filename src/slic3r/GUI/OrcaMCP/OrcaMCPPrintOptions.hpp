// src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>

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

// Where a send's Flashforge-only arguments -- the print options, the material station and its
// mapping -- go.
enum class SendReach
{
    Honoured,                  // a direct send to a Flashforge with local-API credentials
    SendDialog,                // direct: false: the user's send dialog asks for its own
    UploadOnly,                // start_print: false: nothing starts, so the printer never reads the print options
    NotFlashforge,             // another print host takes none
    FlashforgeWithoutLocalApi, // no serial number and check code: the TCP console carries none
};

// The print options a call asks for: "leveling_before_print: true" for each explicit true. An explicit
// false asks for nothing: it is what happens anyway.
std::vector<std::string> requested_print_options(const PrintOptionRequest& request);

// The material-station arguments a call asks for: "use_material_station: true", and
// "material_mappings" when it is a list with an entry. false, an empty list or leaving them out ask
// for nothing.
std::vector<std::string> requested_station_arguments(const nlohmann::json& params);

// The refusal of the arguments in `asked` that a send would silently drop, naming them and why;
// nullopt when the send carries them, or nothing was asked. `host_type` names the host for NotFlashforge.
std::optional<std::string> ignored_arguments_refusal(const std::vector<std::string>& asked, SendReach reach,
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
