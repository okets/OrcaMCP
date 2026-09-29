// src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.cpp
#include "OrcaMCPPrintOptions.hpp"

#include <cmath>
#include <utility>
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

PrintOptionChoice gated(std::optional<bool> requested, bool model_offers_calibration, std::optional<double> estimate)
{
    if (requested)
        return {*requested, "caller"};
    if (!model_offers_calibration)
        return {false, "not_offered_by_model"};
    if (!estimate)
        return {false, "print_time_unknown"};
    return {*estimate >= kCalibrationGateSeconds, "print_time_gate"};
}

nlohmann::json choice_json(const PrintOptionChoice& choice)
{
    return {{"on", choice.on}, {"decided_by", choice.decided_by}};
}

std::string why_dropped(SendReach reach, const std::string& host_type)
{
    switch (reach) {
    case SendReach::SendDialog:
        return "with direct: false the send is left to the user in OrcaSlicer's send dialog, which asks for its own. "
               "Pass direct: true to choose them here, or leave them out.";
    case SendReach::UploadOnly:
        return "with start_print: false nothing starts, so the printer never reads them. Pass them to print_printer_file "
               "when you start the uploaded file, or pass start_print: true.";
    case SendReach::NotFlashforge:
        return "only a Flashforge print host takes them, and the selected host is '" + host_type + "'. Leave them out.";
    case SendReach::FlashforgeWithoutLocalApi:
        return "without its serial number and check code a Flashforge is sent over its TCP console, which carries none "
               "of them. Set them with add_physical_printer, or leave these out.";
    case SendReach::Honoured: break;
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

std::vector<std::string> requested_print_options(const PrintOptionRequest& request)
{
    std::vector<std::string> asked;
    for (const auto& [name, value] : {std::pair<const char*, std::optional<bool>>{kLevelingParam, request.leveling},
                                      {kFlowCalibrationParam, request.flow_calibration},
                                      {kTimeLapseParam, request.time_lapse}})
        if (value.value_or(false))
            asked.push_back(std::string(name) + ": true");
    return asked;
}

std::vector<std::string> requested_station_arguments(const nlohmann::json& params)
{
    std::vector<std::string> asked;
    if (!params.is_object())
        return asked;
    if (const auto it = params.find("use_material_station"); it != params.end() && it->is_boolean() && it->get<bool>())
        asked.push_back("use_material_station: true");
    if (const auto it = params.find("material_mappings"); it != params.end() && it->is_array() && !it->empty())
        asked.push_back("material_mappings");
    return asked;
}

std::optional<std::string> ignored_arguments_refusal(const std::vector<std::string>& asked, SendReach reach,
                                                     const std::string& host_type)
{
    if (reach == SendReach::Honoured || asked.empty())
        return std::nullopt;
    std::string names;
    for (const std::string& name : asked)
        names += (names.empty() ? "" : ", ") + name;
    return "Nothing was sent. " + names + " would be ignored: " + why_dropped(reach, host_type);
}

PrintOptionChoices choose_print_options(const PrintOptionRequest& request, const GateFacts& facts)
{
    const std::optional<double> estimate = known_estimate(facts.estimated_print_s);
    PrintOptionChoices          choices;
    choices.leveling         = gated(request.leveling, facts.model_offers_calibration, estimate);
    choices.flow_calibration = gated(request.flow_calibration, facts.model_offers_calibration, estimate);
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

nlohmann::json print_options_json(const PrintOptionChoices& choices, const GateFacts& facts)
{
    const std::optional<double> estimate = known_estimate(facts.estimated_print_s);
    return {{"leveling", choice_json(choices.leveling)},
            {"flow_calibration", choice_json(choices.flow_calibration)},
            {"time_lapse", choice_json(choices.time_lapse)},
            {"estimated_print_s", estimate ? nlohmann::json(std::lround(*estimate)) : nlohmann::json(nullptr)},
            {"gate_s", std::lround(kCalibrationGateSeconds)},
            {"printer_model", facts.printer_model.empty() ? nlohmann::json(nullptr) : nlohmann::json(facts.printer_model)}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
