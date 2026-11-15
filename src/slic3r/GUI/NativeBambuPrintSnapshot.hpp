#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace Slic3r {
// The raw LAN payload, before diff reconstruction. A previous connection's base
// must never make a delta look like a newly received full snapshot.
inline bool native_bambu_print_snapshot_complete(const nlohmann::json& raw)
{
    if (!raw.is_object()) return false;
    // Native protocol: msg 0 is full, msg 1 is a delta. Legacy full LAN
    // packets omit msg; every other discriminator is refused.
    if (raw.contains("msg") && (!raw["msg"].is_number_integer() || raw["msg"] != 0)) return false;
    return raw.is_object() && raw.contains("command") && raw["command"].is_string() && raw["command"] == "push_status" &&
        raw.contains("gcode_state") && raw["gcode_state"].is_string() &&
        raw.contains("ams_status") && raw["ams_status"].is_number_integer() &&
        raw.contains("ams") && raw["ams"].is_object() &&
        raw["ams"].contains("ams_exist_bits") && raw["ams"]["ams_exist_bits"].is_string() &&
        raw["ams"].contains("tray_exist_bits") && raw["ams"]["tray_exist_bits"].is_string() &&
        raw["ams"].contains("tray_reading_bits") && raw["ams"]["tray_reading_bits"].is_string() &&
        raw["ams"].contains("ams") && raw["ams"]["ams"].is_array();
}
}
