// src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterControl.cpp
#include "OrcaMCPPrinterControl.hpp"
#include "OrcaMCPCommon.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

using json = nlohmann::json;

// Every action, with the arguments it takes besides `action`.
const std::map<std::string, std::vector<std::string>>& action_arguments()
{
    static const std::map<std::string, std::vector<std::string>> arguments = {
        {"pause", {}},
        {"resume", {}},
        {"cancel", {}},
        {"light_on", {}},
        {"light_off", {}},
        {"set_temperature", {"bed", "chamber", "nozzles"}},
        {"set_filtration", {"recirculation", "exhaust"}},
        {"set_fans", {"chamber_fan", "cooling_fan", "cooling_left_fan"}},
        {"set_print_speed", {"speed"}},
        {"set_z_offset", {"z_offset"}},
    };
    return arguments;
}

std::string action_list()
{
    std::string list;
    for (const auto& [action, arguments] : action_arguments())
        list += (list.empty() ? "" : ", ") + action;
    return list;
}

bool given(const json& params, const char* key) { return params.contains(key) && !params.at(key).is_null(); }

// An argument given for another action: sent along, it would have been ignored and the call reported done.
std::optional<std::string> foreign_argument(const std::string& action, const json& params)
{
    const std::vector<std::string>& own = action_arguments().at(action);
    for (const auto& [owner, arguments] : action_arguments())
        for (const std::string& argument : arguments)
            if (params.contains(argument) && std::find(own.begin(), own.end(), argument) == own.end())
                return argument + " goes with action " + owner + ", not " + action + ": leave it out, or send it with " + owner;
    return std::nullopt;
}

std::optional<std::string> read_number(const json& params, const char* key, const std::string& what, json& out)
{
    if (!given(params, key))
        return std::nullopt;
    double value = 0.0;
    if (!parse_double_param(params.at(key), value))
        return std::string(key) + " must be a finite number, " + what;
    out = value;
    return std::nullopt;
}

std::optional<std::string> temperature_params(const json& params, json& out)
{
    json bed, chamber;
    if (auto error = read_number(params, "bed", "in degrees C", bed))
        return error;
    if (auto error = read_number(params, "chamber", "in degrees C", chamber))
        return error;
    // Four entries, as the firmware takes them: null leaves that nozzle alone, 0 switches it off.
    json nozzles = json::array({nullptr, nullptr, nullptr, nullptr});
    if (params.contains("nozzles")) {
        // Not an array used to be skipped in silence, the printer sent "no change" for every tool.
        if (!params.at("nozzles").is_array())
            return std::string("nozzles must be an array of {tool, temp}");
        for (const json& entry : params.at("nozzles")) {
            if (!entry.is_object() || !entry.contains("tool") || !entry.contains("temp"))
                return std::string("Each entry in nozzles requires 'tool' and 'temp'");
            // parse_integer_param, not is_number_integer(): a client whose JSON layer widens numbers
            // sends 0 as 0.0.
            int tool = 0;
            if (!parse_integer_param(entry.at("tool"), tool))
                return std::string("nozzles[].tool must be an integer");
            double temp = 0.0;
            if (!parse_double_param(entry.at("temp"), temp))
                return std::string("nozzles[].temp must be a finite number, in degrees C");
            if (tool < 0 || tool > 3)
                return std::string("nozzles[].tool must be between 0 and 3");
            nozzles[static_cast<size_t>(tool)] = temp;
        }
    }
    // Nothing to set used to reach the printer as "no change" for every heater, and succeed.
    const bool any_nozzle = std::any_of(nozzles.begin(), nozzles.end(), [](const json& t) { return !t.is_null(); });
    if (bed.is_null() && chamber.is_null() && !any_nozzle)
        return std::string("set_temperature needs something to set: bed, chamber or nozzles ([{tool, temp}])");
    out = {{"name", "temperature"}, {"bed", bed}, {"chamber", chamber}, {"nozzles", nozzles}};
    return std::nullopt;
}

std::optional<std::string> filtration_params(const json& params, json& out)
{
    out = {{"name", "filtration"}};
    for (const auto& [argument, side] : {std::pair<const char*, const char*>{"recirculation", "internal"}, {"exhaust", "external"}}) {
        if (!given(params, argument))
            continue;
        bool on = false;
        if (!parse_boolean_param(params.at(argument), on))
            return std::string(argument) + " must be true (on) or false (off)";
        out[side] = on ? "open" : "close";
    }
    if (!out.contains("internal") && !out.contains("external"))
        return std::string("set_filtration needs something to set: recirculation or exhaust (true or false)");
    return std::nullopt;
}

std::optional<std::string> fan_params(const json& params, json& out)
{
    out = {{"name", "printer_ctl"}};
    for (const auto& [argument, field] : {std::pair<const char*, const char*>{"chamber_fan", "chamberFan"},
                                          {"cooling_fan", "coolingFan"},
                                          {"cooling_left_fan", "coolingLeftFan"}}) {
        json value;
        if (auto error = read_number(params, argument, "a fan speed in percent, 0 to 100", value))
            return error;
        if (!value.is_null())
            out[field] = value;
    }
    if (out.size() == 1)
        return std::string("set_fans needs something to set: chamber_fan, cooling_fan or cooling_left_fan (0 to 100 %)");
    return std::nullopt;
}

std::optional<std::string> single_control(const json& params, const char* argument, const char* field, const std::string& what,
                                          json& out)
{
    json value;
    if (auto error = read_number(params, argument, what, value))
        return error;
    if (value.is_null())
        return std::string(argument) + " is required: " + what;
    out = {{"name", "printer_ctl"}, {field, value}};
    return std::nullopt;
}

json number_or_null(const json& raw, const char* key)
{
    const auto it = raw.find(key);
    return it != raw.end() && it->is_number() ? *it : json(nullptr);
}

json switch_or_null(const json& raw, const char* key)
{
    const auto it = raw.find(key);
    return it != raw.end() && it->is_string() ? json(it->get<std::string>() == "open") : json(nullptr);
}

} // namespace

std::optional<std::string> printer_control_request(const json& params, PrinterControlRequest& out)
{
    if (!params.contains("action") || !params.at("action").is_string())
        return std::string("action must be a string");
    out.action = params.at("action").get<std::string>();
    if (action_arguments().count(out.action) == 0)
        return "Unknown action '" + out.action + "'. Supported: " + action_list();
    if (auto error = foreign_argument(out.action, params))
        return error;

    const std::string& action = out.action;
    out.reads_status          = false;
    if (action == "pause" || action == "resume" || action == "cancel") {
        out.console_params = {{"name", "job"}, {"action", action == "cancel" ? "stop" : action}};
        return std::nullopt;
    }
    if (action == "light_on" || action == "light_off") {
        out.console_params = {{"name", "light"}, {"on", action == "light_on"}};
        return std::nullopt;
    }
    if (action == "set_temperature")
        return temperature_params(params, out.console_params);

    out.reads_status = true;
    if (action == "set_filtration")
        return filtration_params(params, out.console_params);
    if (action == "set_fans")
        return fan_params(params, out.console_params);
    if (action == "set_print_speed")
        return single_control(params, "speed", "speed", "a print speed in percent, one of 50, 100, 125 or 166", out.console_params);
    return single_control(params, "z_offset", "zAxisCompensation", "the Z offset in mm, within -1 to 1 in 0.025 mm steps",
                          out.console_params);
}

json printer_controls_json(const json& raw)
{
    if (!raw.is_object())
        return json::object();
    const json speed = number_or_null(raw, "printSpeedAdjust");
    json       controls = {{"print_speed_percent", speed.is_number() && speed.get<double>() > 0 ? speed : json(nullptr)},
                           {"z_offset_mm", number_or_null(raw, "zAxisCompensation")},
                           {"chamber_fan_percent", number_or_null(raw, "chamberFanSpeed")},
                           {"cooling_fan_percent", number_or_null(raw, "coolingFanSpeed")},
                           {"recirculation", switch_or_null(raw, "internalFanStatus")},
                           {"exhaust", switch_or_null(raw, "externalFanStatus")}};
    if (raw.contains("coolingLeftFanSpeed"))
        controls["cooling_left_fan_percent"] = number_or_null(raw, "coolingLeftFanSpeed");
    return controls;
}

}}} // namespace Slic3r::GUI::OrcaMCP
