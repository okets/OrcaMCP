#include "FlashforgeApi.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string>

namespace Slic3r { namespace FlashforgeApi {

namespace {

// Mirrors the int/bool/numeric-string tolerance of Flashforge.cpp's file-local
// try_parse_json_int() (~Flashforge.cpp:120-150), duplicated here because that helper is a
// translation-unit-local static and FlashforgeApi must stay a pure, dependency-free layer.
bool try_parse_json_int(const nlohmann::json& value, int& out)
{
    try {
        if (value.is_number_integer() || value.is_number_unsigned()) {
            out = value.get<int>();
            return true;
        }
        if (value.is_boolean()) {
            out = value.get<bool>() ? 1 : 0;
            return true;
        }
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (text.empty())
                return false;
            size_t pos = 0;
            const long parsed = std::stol(text, &pos, 10);
            if (pos == text.size()) {
                out = static_cast<int>(parsed);
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

double get_number(const nlohmann::json& obj, const char* key, double def = 0.0)
{
    if (!obj.is_object() || !obj.contains(key))
        return def;
    const auto& v = obj[key];
    if (v.is_number())
        return v.get<double>();
    if (v.is_boolean())
        return v.get<bool>() ? 1.0 : 0.0;
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
        }
    }
    return def;
}

std::string get_string(const nlohmann::json& obj, const char* key, const std::string& def = {})
{
    if (!obj.is_object() || !obj.contains(key))
        return def;
    const auto& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    if (v.is_number_float())
        return std::to_string(v.get<double>());
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    return def;
}

bool get_bool(const nlohmann::json& obj, const char* key, bool def = false)
{
    if (!obj.is_object() || !obj.contains(key))
        return def;
    const auto& v = obj[key];
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number())
        return v.get<double>() != 0.0;
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        return s == "true" || s == "1" || s == "open";
    }
    return def;
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Flashforge's normalized state -> the gcode_state vocabulary MachineObject expects.
std::string map_state_to_gcode_state(const std::string& state)
{
    const std::string s = to_lower(state);
    if (s == "printing")
        return "RUNNING";
    if (s == "paused")
        return "PAUSE";
    if (s == "completed")
        return "FINISH";
    if (s == "error")
        return "FAILED";
    return "IDLE"; // ready | busy | heating | cancelled | unknown
}

// Bambu's numeric stage, shown next to the state on the Device tab.
int map_state_to_print_stage(const std::string& state)
{
    const std::string s = to_lower(state);
    if (s == "printing")
        return 1;
    if (s == "paused")
        return 2;
    if (s == "completed")
        return 3;
    if (s == "error")
        return 4;
    return 0;
}

// A 0..1 fraction as whole percent. Truncating (with an epsilon that absorbs binary
// representation error, so 0.29 is 29 and not 28) keeps a print at 99% until it really ends.
int scale_progress_to_percent(double progress)
{
    if (!(progress > 0.0))
        return 0; // also catches NaN
    return std::min(100, static_cast<int>(progress * 100.0 + 1e-6));
}

// The tool the UI should show: the first one actually being heated, else tool 0.
size_t active_nozzle_index(const std::vector<NozzleTemp>& nozzles)
{
    for (size_t i = 0; i < nozzles.size(); ++i) {
        if (nozzles[i].target > 0)
            return i;
    }
    return 0;
}

// Only a purely numeric code can be forwarded; see the call site for why.
int parse_error_code(const std::string& error_code)
{
    if (error_code.empty() || !std::all_of(error_code.begin(), error_code.end(),
                                           [](unsigned char c) { return std::isdigit(c) != 0; }))
        return 0;
    try {
        return std::stoi(error_code);
    } catch (const std::exception&) {
        return 0; // out of int range
    }
}

void fill_nozzles(const nlohmann::json& detail, PrinterStatus& out)
{
    out.nozzles.clear();

    if (detail.contains("nozzleTemps") && detail["nozzleTemps"].is_array()) {
        const auto& temps = detail["nozzleTemps"];
        const auto& targets = (detail.contains("nozzleTargetTemps") && detail["nozzleTargetTemps"].is_array())
                                   ? detail["nozzleTargetTemps"]
                                   : nlohmann::json::array();
        for (size_t i = 0; i < temps.size(); ++i) {
            NozzleTemp nt;
            nt.current = temps[i].is_number() ? temps[i].get<double>() : 0.0;
            nt.target  = (i < targets.size() && targets[i].is_number()) ? targets[i].get<double>() : 0.0;
            out.nozzles.push_back(nt);
        }
        return;
    }

    // Legacy single-nozzle printers report rightTemp/rightTargetTemp instead of the arrays.
    if (detail.contains("rightTemp")) {
        NozzleTemp nt;
        nt.current = get_number(detail, "rightTemp");
        nt.target  = get_number(detail, "rightTargetTemp");
        out.nozzles.push_back(nt);
    }
}

void fill_material_slots(const nlohmann::json& detail, PrinterStatus& out)
{
    out.slots.clear();

    if (!detail.contains("matlStationInfo") || !detail["matlStationInfo"].is_object())
        return;

    const auto& matl = detail["matlStationInfo"];
    if (!matl.contains("slotInfos") || !matl["slotInfos"].is_array())
        return;

    for (const auto& slot : matl["slotInfos"]) {
        MaterialSlot ms;
        ms.slot_id         = static_cast<int>(get_number(slot, "slotId"));
        ms.has_filament    = get_bool(slot, "hasFilament");
        ms.material_name   = get_string(slot, "materialName");
        ms.material_color  = get_string(slot, "materialColor");
        out.slots.push_back(ms);
    }
}

} // namespace

bool parse_detail(const std::string& body, PrinterStatus& out, std::string& error)
{
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "Flashforge API: invalid JSON response";
        return false;
    }

    int  code     = 0;
    bool has_code = false;
    if (parsed.contains("code"))
        has_code = try_parse_json_int(parsed["code"], code);
    if (!has_code && parsed.contains("err"))
        has_code = try_parse_json_int(parsed["err"], code);

    if (has_code && code != 0) {
        std::string message = get_string(parsed, "message");
        if (message.empty())
            message = get_string(parsed, "msg", "Request failed");
        error = "Flashforge API error " + std::to_string(code) + ": " + message;
        return false;
    }

    const nlohmann::json& detail = (parsed.contains("detail") && parsed["detail"].is_object()) ? parsed["detail"] : parsed;

    PrinterStatus result;
    result.state = to_lower(get_string(detail, "status", "unknown"));
    if (result.state == "cancel")
        result.state = "cancelled";

    result.print_file   = get_string(detail, "printFileName");
    result.progress     = get_number(detail, "printProgress");
    result.duration_s   = static_cast<long>(get_number(detail, "printDuration"));
    result.remaining_s  = static_cast<long>(get_number(detail, "estimatedTime"));

    result.bed_temp       = get_number(detail, "platTemp");
    result.bed_target     = get_number(detail, "platTargetTemp");
    result.chamber_temp   = get_number(detail, "chamberTemp");
    result.chamber_target = get_number(detail, "chamberTargetTemp");

    fill_nozzles(detail, result);

    result.light_on   = get_string(detail, "lightStatus") == "open";
    result.door       = get_string(detail, "doorStatus");
    result.error_code = get_string(detail, "errorCode");

    result.firmware = get_string(detail, "firmwareVersion");
    result.name     = get_string(detail, "name");
    result.model    = get_string(detail, "model");
    result.pid      = static_cast<int>(get_number(detail, "pid"));
    result.ip       = get_string(detail, "ipAddr");
    result.camera_stream_url = get_string(detail, "cameraStreamUrl");

    fill_material_slots(detail, result);
    result.has_material_station = get_bool(detail, "hasMatlStation") || !result.slots.empty();

    result.raw = detail;

    out = std::move(result);
    error.clear();
    return true;
}

nlohmann::json flashforge_status_to_bambu_payload(const PrinterStatus& status)
{
    nlohmann::json print;
    print["command"]            = "push_status";
    print["msg"]                = 0; // 0 = full status push (not a diff)
    print["support_mqtt_alive"] = true;

    print["gcode_state"]    = map_state_to_gcode_state(status.state);
    print["mc_print_stage"] = map_state_to_print_stage(status.state);

    // The UI reads these as Bambu HMS codes, so only a numeric Flashforge code can be
    // forwarded; anything else (or none) stays at 0 to avoid a bogus error dialog.
    print["mc_print_error_code"] = 0;
    print["print_error"]         = parse_error_code(status.error_code);

    print["bed_temper"]        = status.bed_temp;
    print["bed_target_temper"] = status.bed_target;
    print["chamber_temper"]    = status.chamber_temp;

    if (!status.nozzles.empty()) {
        const NozzleTemp& active = status.nozzles[active_nozzle_index(status.nozzles)];
        print["nozzle_temper"]        = active.current;
        print["nozzle_target_temper"] = active.target;

        nlohmann::json extruders = nlohmann::json::array();
        for (const NozzleTemp& nozzle : status.nozzles)
            extruders.push_back(nlohmann::json{{"temp", nozzle.current}, {"target", nozzle.target}});
        print["extruder"] = std::move(extruders);
    }

    print["mc_percent"]        = scale_progress_to_percent(status.progress);
    print["mc_remaining_time"] = static_cast<int>(std::max<long>(0, status.remaining_s) / 60);

    if (!status.print_file.empty()) {
        print["subtask_name"] = status.print_file;
        print["gcode_file"]   = status.print_file;
    }

    print["lights_report"] = nlohmann::json::array({
        nlohmann::json{{"node", "chamber_light"}, {"mode", status.light_on ? "on" : "off"}}});

    // No `ipcam`, deliberately. MachineObject reads `ipcam.rtsp_url` as a Bambu liveview address
    // (DeviceManager.cpp:3849-3852) and arms MediaPlayCtrl's BambuSource player with
    // "bambu:///rtsp___<user>:<code>@<ip>/streaming/live/1" (MediaPlayCtrl.cpp:280-284). Flashforge
    // serves an HTTP MJPEG stream, not RTSP, so that player has nothing to open - it aborted the app
    // when the Play button was pressed. The camera reaches the user through the Device (Web) tab
    // instead; see Flashforge::get_print_host_webui.

    return nlohmann::json{{"print", std::move(print)}};
}

nlohmann::json make_credentials_payload(const std::string& serial, const std::string& check_code)
{
    return nlohmann::json{{"serialNumber", serial}, {"checkCode", check_code}};
}

nlohmann::json make_control_payload(const std::string& serial, const std::string& check_code, const std::string& cmd, const nlohmann::json& args)
{
    nlohmann::json payload = make_credentials_payload(serial, check_code);
    payload["payload"] = nlohmann::json{{"cmd", cmd}, {"args", args}};
    return payload;
}

nlohmann::json make_temperature_args(std::optional<double> bed, std::optional<double> chamber, const std::vector<std::optional<double>>& nozzles)
{
    std::array<double, 4> padded{kTempNoChange, kTempNoChange, kTempNoChange, kTempNoChange};
    for (size_t i = 0; i < nozzles.size() && i < padded.size(); ++i)
        padded[i] = nozzles[i].value_or(kTempNoChange);

    nlohmann::json args;
    args["platform"]    = bed.value_or(kTempNoChange);
    args["chamber"]     = chamber.value_or(kTempNoChange);
    args["nozzles"]     = nlohmann::json::array({padded[0], padded[1], padded[2], padded[3]});
    args["rightNozzle"] = padded[0]; // legacy single-nozzle field mirrors tool 0
    args["leftNozzle"]  = kTempNoChange;
    return args;
}

nlohmann::json make_print_gcode_payload(const std::string& serial, const std::string& check_code, const std::string& file_name, bool leveling, const nlohmann::json& material_mappings)
{
    const bool has_mappings = material_mappings.is_array() && !material_mappings.empty();

    nlohmann::json payload = make_credentials_payload(serial, check_code);
    payload["fileName"]            = file_name;
    payload["levelingBeforePrint"] = leveling;
    payload["flowCalibration"]     = false;
    payload["useMatlStation"]      = has_mappings;
    payload["gcodeToolCnt"]        = material_mappings.is_array() ? material_mappings.size() : 0;
    payload["materialMappings"]    = material_mappings;
    return payload;
}

}} // namespace Slic3r::FlashforgeApi
