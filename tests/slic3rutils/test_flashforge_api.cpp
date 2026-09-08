#include <catch2/catch_all.hpp>
#include "slic3r/Utils/FlashforgeApi.hpp"

using namespace Slic3r::FlashforgeApi;

static const char* kDetail = R"({"code":0,"message":"Success","detail":{
  "status":"printing","printFileName":"benchy.gcode","printProgress":0.42,"printDuration":600,"estimatedTime":1400,
  "platTemp":60.2,"platTargetTemp":60,"chamberTemp":35,"chamberTargetTemp":0,
  "nozzleTemps":[215,30,30,210],"nozzleTargetTemps":[215,0,0,210],"nozzleCnt":4,
  "lightStatus":"open","doorStatus":"close","errorCode":"","firmwareVersion":"1.9.2","name":"C5P","model":"Creator 5 Pro","pid":41,
  "ipAddr":"192.168.1.50","cameraStreamUrl":"http://192.168.1.50:8080/?action=stream",
  "hasMatlStation":true,"matlStationInfo":{"slotCnt":4,"slotInfos":[{"slotId":1,"hasFilament":true,"materialName":"PLA","materialColor":"#FF0000"}]}}})";

TEST_CASE("parse_detail reads a Creator 5 Pro status", "[flashforge]") {
    PrinterStatus s; std::string err;
    REQUIRE(parse_detail(kDetail, s, err));
    CHECK(s.state == "printing");
    CHECK(s.print_file == "benchy.gcode");
    CHECK(s.progress == Catch::Approx(0.42));
    CHECK(s.remaining_s == 1400);
    CHECK(s.nozzles.size() == 4);
    CHECK(s.nozzles[3].target == 210);
    CHECK(s.light_on);
    CHECK(s.pid == kPidCreator5Pro);
    CHECK(s.slots.size() == 1);
    CHECK(s.slots[0].material_color == "#FF0000");
    CHECK(s.raw["nozzleCnt"] == 4);
}

TEST_CASE("parse_detail falls back to rightTemp for single nozzle printers", "[flashforge]") {
    PrinterStatus s; std::string err;
    REQUIRE(parse_detail(R"({"code":0,"detail":{"status":"ready","rightTemp":25,"rightTargetTemp":0,"platTemp":24,"platTargetTemp":0}})", s, err));
    REQUIRE(s.nozzles.size() == 1);
    CHECK(s.nozzles[0].current == 25);
}

TEST_CASE("parse_detail rejects error codes and garbage", "[flashforge]") {
    PrinterStatus s; std::string err;
    CHECK_FALSE(parse_detail(R"({"code":401,"message":"check code error"})", s, err));
    CHECK(err.find("401") != std::string::npos);
    CHECK_FALSE(parse_detail("not json", s, err));
}

TEST_CASE("control payload shapes", "[flashforge]") {
    auto p = make_control_payload("SN1", "CC1", "jobCtl_cmd", {{"jobID", ""}, {"action", "pause"}});
    CHECK(p["serialNumber"] == "SN1");
    CHECK(p["payload"]["cmd"] == "jobCtl_cmd");
    CHECK(p["payload"]["args"]["action"] == "pause");

    auto t = make_temperature_args(60.0, std::nullopt, {215.0, std::nullopt, std::nullopt, std::nullopt});
    CHECK(t["platform"] == 60);
    CHECK(t["chamber"] == kTempNoChange);
    CHECK(t["nozzles"] == nlohmann::json({215, kTempNoChange, kTempNoChange, kTempNoChange}));
    CHECK(t["rightNozzle"] == 215);   // first tool mirrored into the legacy field

    auto g = make_print_gcode_payload("SN1", "CC1", "a.gcode", true, nlohmann::json::array({{{"toolId", 0}, {"slotId", 1}}}));
    CHECK(g["fileName"] == "a.gcode");
    CHECK(g["levelingBeforePrint"] == true);
    CHECK(g["useMatlStation"] == true);
    CHECK(g["gcodeToolCnt"] == 1);
}

// ---------------------------------------------------------------------------
// flashforge_status_to_bambu_payload: the Bambu-shaped push_status the Device
// tab (MonitorPanel / MachineObject::parse_json) already knows how to render.
// ---------------------------------------------------------------------------

static PrinterStatus make_status(const std::string& state, double progress = 0.0)
{
    PrinterStatus s;
    s.state    = state;
    s.progress = progress;
    return s;
}

TEST_CASE("flashforge_status_to_bambu_payload maps gcode_state", "[flashforge]") {
    CHECK(flashforge_status_to_bambu_payload(make_status("printing"))["print"]["gcode_state"] == "RUNNING");
    CHECK(flashforge_status_to_bambu_payload(make_status("paused"))["print"]["gcode_state"] == "PAUSE");
    CHECK(flashforge_status_to_bambu_payload(make_status("completed"))["print"]["gcode_state"] == "FINISH");
    CHECK(flashforge_status_to_bambu_payload(make_status("error"))["print"]["gcode_state"] == "FAILED");
    CHECK(flashforge_status_to_bambu_payload(make_status("ready"))["print"]["gcode_state"] == "IDLE");
    CHECK(flashforge_status_to_bambu_payload(make_status("busy"))["print"]["gcode_state"] == "IDLE");
    CHECK(flashforge_status_to_bambu_payload(make_status("heating"))["print"]["gcode_state"] == "IDLE");
    CHECK(flashforge_status_to_bambu_payload(make_status("cancelled"))["print"]["gcode_state"] == "IDLE");
    CHECK(flashforge_status_to_bambu_payload(make_status("unknown"))["print"]["gcode_state"] == "IDLE");
    // The mapping is case-insensitive: the API has been seen to return "Printing".
    CHECK(flashforge_status_to_bambu_payload(make_status("Printing"))["print"]["gcode_state"] == "RUNNING");
}

TEST_CASE("flashforge_status_to_bambu_payload rounds mc_percent", "[flashforge]") {
    // 0..1 fraction scaled to whole percent, never rounding a partial print up to 100.
    CHECK(flashforge_status_to_bambu_payload(make_status("printing", 0.0))["print"]["mc_percent"] == 0);
    CHECK(flashforge_status_to_bambu_payload(make_status("printing", 0.42))["print"]["mc_percent"] == 42);
    CHECK(flashforge_status_to_bambu_payload(make_status("printing", 0.29))["print"]["mc_percent"] == 29);
    CHECK(flashforge_status_to_bambu_payload(make_status("printing", 0.999))["print"]["mc_percent"] == 99);
    CHECK(flashforge_status_to_bambu_payload(make_status("completed", 1.0))["print"]["mc_percent"] == 100);
    // Out-of-range readings are clamped rather than propagated to the progress bar.
    CHECK(flashforge_status_to_bambu_payload(make_status("printing", -0.5))["print"]["mc_percent"] == 0);
    CHECK(flashforge_status_to_bambu_payload(make_status("printing", 1.5))["print"]["mc_percent"] == 100);
}

TEST_CASE("flashforge_status_to_bambu_payload maps the whole Creator 5 Pro detail", "[flashforge]") {
    PrinterStatus s; std::string err;
    REQUIRE(parse_detail(kDetail, s, err));

    const auto p = flashforge_status_to_bambu_payload(s)["print"];

    CHECK(p["command"] == "push_status");
    CHECK(p["gcode_state"] == "RUNNING");
    CHECK(p["mc_percent"] == 42);
    CHECK(p["mc_remaining_time"] == 23);            // 1400 s -> whole minutes
    CHECK(p["subtask_name"] == "benchy.gcode");
    CHECK(p["print_error"] == 0);                   // empty errorCode -> 0

    CHECK(p["bed_temper"] == Catch::Approx(60.2));
    CHECK(p["bed_target_temper"] == Catch::Approx(60));
    CHECK(p["chamber_temper"] == Catch::Approx(35));

    // Active nozzle = the first one with a non-zero target (tool 0 here).
    CHECK(p["nozzle_temper"] == Catch::Approx(215));
    CHECK(p["nozzle_target_temper"] == Catch::Approx(215));
    REQUIRE(p["extruder"].size() == 4);
    CHECK(p["extruder"][3]["temp"] == Catch::Approx(210));
    CHECK(p["extruder"][3]["target"] == Catch::Approx(210));

    REQUIRE(p["lights_report"].size() == 1);
    CHECK(p["lights_report"][0]["node"] == "chamber_light");
    CHECK(p["lights_report"][0]["mode"] == "on");

    CHECK(p["ipcam"]["rtsp_url"] == "http://192.168.1.50:8080/?action=stream");
}

TEST_CASE("flashforge_status_to_bambu_payload picks the first heated nozzle", "[flashforge]") {
    PrinterStatus s = make_status("printing");
    s.nozzles = {{25, 0}, {30, 0}, {240, 245}, {28, 0}};
    auto p = flashforge_status_to_bambu_payload(s)["print"];
    CHECK(p["nozzle_temper"] == Catch::Approx(240));
    CHECK(p["nozzle_target_temper"] == Catch::Approx(245));

    // All idle: fall back to tool 0 rather than reporting nothing.
    s.nozzles = {{25, 0}, {30, 0}};
    p = flashforge_status_to_bambu_payload(s)["print"];
    CHECK(p["nozzle_temper"] == Catch::Approx(25));
    CHECK(p["nozzle_target_temper"] == Catch::Approx(0));

    // No nozzle data at all: omit the keys instead of publishing a bogus 0 C.
    s.nozzles.clear();
    p = flashforge_status_to_bambu_payload(s)["print"];
    CHECK_FALSE(p.contains("nozzle_temper"));
    CHECK_FALSE(p.contains("extruder"));
}

TEST_CASE("flashforge_status_to_bambu_payload forwards numeric error codes only", "[flashforge]") {
    PrinterStatus s = make_status("error");
    s.error_code = "";
    CHECK(flashforge_status_to_bambu_payload(s)["print"]["print_error"] == 0);
    s.error_code = "12345";
    CHECK(flashforge_status_to_bambu_payload(s)["print"]["print_error"] == 12345);
    // Non-numeric codes would be read as an HMS code by the UI, so they stay at 0.
    s.error_code = "E-STOP";
    CHECK(flashforge_status_to_bambu_payload(s)["print"]["print_error"] == 0);
}

TEST_CASE("flashforge_status_to_bambu_payload omits absent optional fields", "[flashforge]") {
    PrinterStatus s = make_status("ready");
    const auto p = flashforge_status_to_bambu_payload(s)["print"];
    CHECK_FALSE(p.contains("ipcam"));       // no camera_stream_url
    CHECK_FALSE(p.contains("subtask_name"));// no print_file
    CHECK(p["mc_remaining_time"] == 0);
    CHECK(p["lights_report"][0]["mode"] == "off");
}
