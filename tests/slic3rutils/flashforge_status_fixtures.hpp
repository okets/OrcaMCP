#ifndef slic3r_tests_flashforge_status_fixtures_hpp_
#define slic3r_tests_flashforge_status_fixtures_hpp_

#include <nlohmann/json.hpp>
#include <string>

// Two `detail` answers of a Creator 5 Pro on firmware 1.9.9, right after OrcaMCP started a print on it
// (2026-09-30), about four seconds apart: the job and control fields are the values an agent captured
// through get_printer_status, the rest is the same printer's documented idle answer
// (docs/printers/flashforge-lan-api.md, "A real response"), which those fields left unchanged.
namespace flashforge_fixtures {

// The fields neither capture changed.
inline nlohmann::json creator_5_pro_detail()
{
    return nlohmann::json::parse(R"({
      "status": "ready", "printFileName": "", "printProgress": 0.0,
      "printDuration": 0, "estimatedTime": 0.0, "printLayer": 0, "targetPrintLayer": 0,
      "errorCode": "", "doorStatus": "close", "lightStatus": "open",
      "model": "Creator 5 Pro", "name": "Creator 5 Pro", "firmwareVersion": "1.9.9",
      "ipAddr": "10.0.0.100", "location": "Den", "measure": "256X256X256",
      "nozzleCnt": 4, "nozzleModel": "0.4mm;0.4mm;0.4mm;0.4mm", "nozzleStyle": 0,
      "nozzleTemps": [28, 29, 29, 29], "nozzleTargetTemps": [0, 0, 0, 0],
      "platTemp": 27, "platTargetTemp": 0, "chamberTemp": 27, "chamberTargetTemp": 0,
      "chamberFanSpeed": 0, "coolingFanSpeed": 0,
      "internalFanStatus": "close", "externalFanStatus": "close",
      "camera": 1, "cameraStreamUrl": "http://10.0.0.100:8080/?action=stream",
      "tvoc": 0, "remainingDiskSpace": 4.91, "cumulativePrintTime": 1314, "cumulativeFilament": 49.77,
      "currentPrintSpeed": 0, "printSpeedAdjust": 0.0, "zAxisCompensation": 0.0,
      "flashRegisterCode": "", "polarRegisterCode": "", "pid": 41, "hasMatlStation": true,
      "matlStationInfo": {
        "currentLoadSlot": 0, "currentSlot": 0, "slotCnt": 4, "stateAction": 0, "stateStep": 0,
        "slotInfos": [
          {"slotId": 1, "hasFilament": true,  "materialName": "PLA", "materialColor": "#FFFFFF"},
          {"slotId": 2, "hasFilament": true,  "materialName": "PLA", "materialColor": "#0A0A0A"},
          {"slotId": 3, "hasFilament": true,  "materialName": "PLA", "materialColor": "#D02020"},
          {"slotId": 4, "hasFilament": false, "materialName": "",    "materialColor": ""}]}
    })");
}

// Within a second of the start, state `unzipping`: every job and control number is integer bits (a fan
// at 0x4F0000, the speed at 0x1007C), and the two floats are integer bits read as floats (0x790FFF,
// 0x3004E).
inline nlohmann::json unzipping_detail()
{
    nlohmann::json detail          = creator_5_pro_detail();
    detail["status"]               = "unzipping";
    detail["printProgress"]        = 1.1117833352328347e-38;
    detail["printDuration"]        = 268369921;
    detail["estimatedTime"]        = 65656;
    detail["printLayer"]           = 65658;
    detail["targetPrintLayer"]     = 8065023;
    detail["printSpeedAdjust"]     = 65660;
    detail["zAxisCompensation"]    = 2.7561578975419097e-40;
    detail["chamberFanSpeed"]      = 5177344;
    detail["coolingFanSpeed"]      = 5111810;
    detail["matlStationInfo"]["stateAction"] = 5;
    return detail;
}

// The same start on 2026-09-30 at 19:55, recorded four times a second through get_printer_status: for
// half a second from state `downloading` into `unzipping`, every job and control number is a small
// integer, all of them in range, and the two floats are integer bits read as floats (11 and 9). The job's
// times were not recorded (get_printer_status left them out); they stay as idle here.
inline nlohmann::json downloading_detail()
{
    nlohmann::json detail          = creator_5_pro_detail();
    detail["status"]               = "downloading";
    detail["printProgress"]        = 1.5414283107572988e-44;
    detail["printLayer"]           = 11;
    detail["targetPrintLayer"]     = 0;
    detail["printSpeedAdjust"]     = 10;
    detail["zAxisCompensation"]    = 1.2611686178923354e-44;
    detail["chamberFanSpeed"]      = 11;
    detail["coolingFanSpeed"]      = 9;
    detail["matlStationInfo"]["stateAction"] = 5;
    return detail;
}

// About four seconds later, printing: every number reads right again.
inline nlohmann::json first_printing_detail()
{
    nlohmann::json detail      = creator_5_pro_detail();
    detail["status"]           = "printing";
    detail["printFileName"]    = "Dragon_Glider_PLA_29m11s.gcode.3mf";
    detail["estimatedTime"]    = 1751;
    detail["targetPrintLayer"] = 545;
    detail["printSpeedAdjust"] = 100;
    return detail;
}

// `detail` as the local API answers it.
inline std::string detail_body(const nlohmann::json& detail)
{
    return nlohmann::json{{"code", 0}, {"message", "Success"}, {"detail", detail}}.dump();
}

} // namespace flashforge_fixtures

#endif
