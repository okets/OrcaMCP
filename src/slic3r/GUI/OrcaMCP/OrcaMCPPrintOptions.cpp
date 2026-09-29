// src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.cpp
#include "OrcaMCPPrintOptions.hpp"

#include <cmath>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

bool read_optional_bool(const nlohmann::json& params, const char* name, std::optional<bool>& out, std::string& error)
{
    out.reset();
    if (!params.is_object() || !params.contains(name))
        return true;
    const nlohmann::json& value = params.at(name);
    if (!value.is_boolean()) {
        error = std::string(name) + " must be a boolean";
        return false;
    }
    out = value.get<bool>();
    return true;
}

std::optional<double> known_estimate(std::optional<double> estimated_print_s)
{
    if (estimated_print_s && std::isfinite(*estimated_print_s) && *estimated_print_s > 0)
        return estimated_print_s;
    return std::nullopt;
}

PrintOptionChoice gated(std::optional<bool> requested, std::optional<double> estimate)
{
    if (requested)
        return {*requested, "caller"};
    if (!estimate)
        return {false, "print_time_unknown"};
    return {*estimate >= kCalibrationGateSeconds, "print_time_gate"};
}

nlohmann::json choice_json(const PrintOptionChoice& choice)
{
    return {{"on", choice.on}, {"decided_by", choice.decided_by}};
}

// "leveling_before_print: true, time_lapse: true": the parameters a send would drop.
std::string requested_on(const PrintOptionRequest& request)
{
    std::vector<const char*> names;
    if (request.leveling.value_or(false))
        names.push_back(kLevelingParam);
    if (request.flow_calibration.value_or(false))
        names.push_back(kFlowCalibrationParam);
    if (request.time_lapse.value_or(false))
        names.push_back(kTimeLapseParam);

    std::string text;
    for (const char* name : names)
        text += (text.empty() ? "" : ", ") + std::string(name) + ": true";
    return text;
}

std::string why_dropped(PrintOptionsReach reach, const std::string& host_type)
{
    switch (reach) {
    case PrintOptionsReach::SendDialog:
        return "with direct: false the send is left to the user in OrcaSlicer's send dialog, which asks for its own print "
               "options. Pass direct: true to choose them here, or leave them out.";
    case PrintOptionsReach::NotFlashforge:
        return "only a Flashforge print host takes print options, and the selected host is '" + host_type +
               "'. Leave them out.";
    case PrintOptionsReach::FlashforgeWithoutLocalApi:
        return "without its serial number and check code a Flashforge is sent over its TCP console, which carries no "
               "print options. Set them with add_physical_printer, or leave these out.";
    case PrintOptionsReach::Honoured: break;
    }
    return {};
}

} // namespace

std::string calibration_gate_text()
{
    return std::to_string(std::lround(kCalibrationGateSeconds / 3600.0)) + " h";
}

bool read_print_option_request(const nlohmann::json& params, PrintOptionRequest& out, std::string& error)
{
    return read_optional_bool(params, kLevelingParam, out.leveling, error) &&
           read_optional_bool(params, kFlowCalibrationParam, out.flow_calibration, error) &&
           read_optional_bool(params, kTimeLapseParam, out.time_lapse, error);
}

bool needs_print_time(const PrintOptionRequest& request)
{
    return !request.leveling || !request.flow_calibration;
}

std::optional<std::string> ignored_print_options_refusal(const PrintOptionRequest& request, PrintOptionsReach reach,
                                                         const std::string& host_type)
{
    if (reach == PrintOptionsReach::Honoured)
        return std::nullopt;
    const std::string requested = requested_on(request);
    if (requested.empty())
        return std::nullopt;
    return "Nothing was sent. " + requested + " would be ignored: " + why_dropped(reach, host_type);
}

PrintOptionChoices choose_print_options(const PrintOptionRequest& request, std::optional<double> estimated_print_s)
{
    const std::optional<double> estimate = known_estimate(estimated_print_s);
    PrintOptionChoices          choices;
    choices.leveling         = gated(request.leveling, estimate);
    choices.flow_calibration = gated(request.flow_calibration, estimate);
    choices.time_lapse       = request.time_lapse ? PrintOptionChoice{*request.time_lapse, "caller"}
                                                  : PrintOptionChoice{false, "default"};
    return choices;
}

FlashforgeApi::PrintOptions to_print_options(const PrintOptionChoices& choices)
{
    FlashforgeApi::PrintOptions options;
    options.leveling         = choices.leveling.on;
    options.flow_calibration = choices.flow_calibration.on;
    options.time_lapse       = choices.time_lapse.on;
    return options;
}

nlohmann::json print_options_json(const PrintOptionChoices& choices, std::optional<double> estimated_print_s)
{
    const std::optional<double> estimate = known_estimate(estimated_print_s);
    return {{"leveling", choice_json(choices.leveling)},
            {"flow_calibration", choice_json(choices.flow_calibration)},
            {"time_lapse", choice_json(choices.time_lapse)},
            {"estimated_print_s", estimate ? nlohmann::json(std::lround(*estimate)) : nlohmann::json(nullptr)},
            {"gate_s", std::lround(kCalibrationGateSeconds)}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
