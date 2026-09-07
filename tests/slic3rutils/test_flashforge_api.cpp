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
