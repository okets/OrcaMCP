#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <nlohmann/json.hpp>
#include <string>

#include "slic3r/GUI/FlashforgeConsoleHandler.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPrinterControl.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"
#include "slic3r/Utils/FlashforgeApi.hpp"

// printer_control, from a call's arguments to the exact request the printer's local API would receive,
// with no printer: every step but the POST itself (Flashforge::send_control, which light, temperatures
// and pause already take) is a pure function. No test here may reach a printer.

using json = nlohmann::json;
using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Slic3r::GUI::OrcaMCPServer;
using Catch::Matchers::WithinAbs;

namespace {

// The bench Creator 5 Pro's own values, idle: the fields a control does not change are sent back as these.
FlashforgeApi::PrinterStatus idle_printer()
{
    FlashforgeApi::PrinterStatus status;
    status.state = "ready";
    status.raw   = json{{"internalFanStatus", "close"}, {"externalFanStatus", "open"}, {"zAxisCompensation", 0.05},
                        {"printSpeedAdjust", 0}, {"chamberFanSpeed", 30}, {"coolingFanSpeed", 70}};
    return status;
}

FlashforgeApi::PrinterStatus printing_printer()
{
    FlashforgeApi::PrinterStatus status = idle_printer();
    status.state                        = "printing";
    status.print_file                   = "cube.gcode";
    status.raw["printSpeedAdjust"]      = 100;
    return status;
}

// What printer_control would send the printer for `arguments`, as the local API's /control body with a
// placeholder serial number and check code; or the refusal, as {"refused": message}.
json control_body(const json& arguments, const FlashforgeApi::PrinterStatus& printer)
{
    PrinterControlRequest request;
    if (const auto refusal = printer_control_request(arguments, request))
        return {{"refused", *refusal}};
    const json snapshot = request.reads_status ? GUI::console_snapshot(printer) : json::object();
    if (request.reads_status)
        if (const auto missing = status_refusal(request, snapshot))
            return {{"refused", *missing}};
    json        operation;
    std::string error;
    if (!GUI::build_console_operation(request.console_params, snapshot, operation, error))
        return {{"refused", error}};
    if (operation.value("kind", "") != "control")
        return {{"operation", operation}};
    return FlashforgeApi::make_control_payload("SN-TEST", "CC-TEST", operation["cmd"], operation["args"]);
}

json printer_ctl_body(const json& args)
{
    return {{"serialNumber", "SN-TEST"}, {"checkCode", "CC-TEST"}, {"payload", {{"cmd", "printerCtl_cmd"}, {"args", args}}}};
}

// Whether `body` is `expected`: every string and integer the same, every other number within 1e-9 (a
// Z offset or a fan speed read back from the printer's JSON).
bool same_body(const json& body, const json& expected)
{
    if (body.is_object() && expected.is_object()) {
        if (body.size() != expected.size())
            return false;
        for (const auto& [key, value] : expected.items())
            if (!body.contains(key) || !same_body(body.at(key), value))
                return false;
        return true;
    }
    if (body.is_number_float() || expected.is_number_float())
        return body.is_number() && expected.is_number() && std::abs(body.get<double>() - expected.get<double>()) < 1e-9;
    return body == expected;
}

std::string refusal_of(const json& body) { return body.value("refused", std::string()); }

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

TEST_CASE("each new printer control sends the Device page's own command", "[McpPrinterControl][orcamcp]")
{
    SECTION("set_print_speed changes the running job's speed and carries the rest as the printer reports it")
    {
        CHECK(same_body(control_body({{"action", "set_print_speed"}, {"speed", 125}}, printing_printer()),
                        printer_ctl_body({{"zAxisCompensation", 0.05}, {"speed", 125}, {"chamberFan", 30}, {"coolingFan", 70}})));
    }

    SECTION("set_z_offset on an idle printer sends 100 % for the speed an idle printer reports as 0")
    {
        CHECK(same_body(control_body({{"action", "set_z_offset"}, {"z_offset", -0.075}}, idle_printer()),
                        printer_ctl_body({{"zAxisCompensation", -0.075}, {"speed", 100}, {"chamberFan", 30}, {"coolingFan", 70}})));
    }

    SECTION("set_fans changes only the fans it names")
    {
        CHECK(same_body(control_body({{"action", "set_fans"}, {"chamber_fan", 50}}, idle_printer()),
                        printer_ctl_body({{"zAxisCompensation", 0.05}, {"speed", 100}, {"chamberFan", 50}, {"coolingFan", 70}})));
        CHECK(same_body(control_body({{"action", "set_fans"}, {"chamber_fan", 0}, {"cooling_fan", 100}}, idle_printer()),
                        printer_ctl_body({{"zAxisCompensation", 0.05}, {"speed", 100}, {"chamberFan", 0}, {"coolingFan", 100}})));
    }

    SECTION("set_filtration switches one fan and keeps the other as it is")
    {
        const json circulate = {{"serialNumber", "SN-TEST"},
                                {"checkCode", "CC-TEST"},
                                {"payload", {{"cmd", "circulateCtl_cmd"}, {"args", {{"internal", "open"}, {"external", "open"}}}}}};
        CHECK(control_body({{"action", "set_filtration"}, {"recirculation", true}}, idle_printer()) == circulate);
        CHECK(control_body({{"action", "set_filtration"}, {"exhaust", false}}, idle_printer())["payload"]["args"] ==
              json{{"internal", "close"}, {"external", "close"}});
    }
}

TEST_CASE("the job, light and temperature actions build what the Device page builds", "[McpPrinterControl][orcamcp]")
{
    const FlashforgeApi::PrinterStatus printer = idle_printer();
    CHECK(control_body({{"action", "pause"}}, printer)["operation"] == json{{"kind", "job"}, {"action", "pause"}});
    CHECK(control_body({{"action", "resume"}}, printer)["operation"] == json{{"kind", "job"}, {"action", "resume"}});
    CHECK(control_body({{"action", "cancel"}}, printer)["operation"] == json{{"kind", "job"}, {"action", "stop"}});
    CHECK(control_body({{"action", "light_on"}}, printer)["operation"] == json{{"kind", "light"}, {"on", true}});
    CHECK(control_body({{"action", "light_off"}}, printer)["operation"] == json{{"kind", "light"}, {"on", false}});
    const json temperature = control_body({{"action", "set_temperature"}, {"bed", 60}, {"nozzles", {{{"tool", 1}, {"temp", 210}}}}}, printer)["operation"];
    CHECK(temperature["kind"] == "temperature");
    CHECK(temperature["chamber"].is_null());
    CHECK_THAT(temperature["bed"].get<double>(), WithinAbs(60.0, 1e-9));
    REQUIRE(temperature["nozzles"].size() == 4);
    CHECK(temperature["nozzles"][0].is_null());
    CHECK_THAT(temperature["nozzles"][1].get<double>(), WithinAbs(210.0, 1e-9));
    CHECK(temperature["nozzles"][2].is_null());
    CHECK(temperature["nozzles"][3].is_null());
    // 0 is an instruction to switch off, never "leave alone".
    CHECK_THAT(control_body({{"action", "set_temperature"}, {"chamber", 0}}, printer)["operation"]["chamber"].get<double>(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("printer_control refuses what the printer cannot or should not do, before sending anything", "[McpPrinterControl][orcamcp]")
{
    const FlashforgeApi::PrinterStatus idle     = idle_printer();
    const FlashforgeApi::PrinterStatus printing = printing_printer();

    SECTION("values the Device page never sends")
    {
        CHECK(mentions(refusal_of(control_body({{"action", "set_print_speed"}, {"speed", 200}}, printing)), "50, 100, 125 or 166"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_z_offset"}, {"z_offset", 0.03}}, idle)), "0.025"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_z_offset"}, {"z_offset", 1.5}}, idle)), "1 mm"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_fans"}, {"cooling_fan", 120}}, idle)), "between 0 and 100"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_temperature"}, {"bed", 200}}, idle)), "Bed target"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_temperature"}, {"nozzles", {{{"tool", 0}, {"temp", 400}}}}}, idle)),
                       "Nozzle target"));
    }

    SECTION("a Z offset off the printer's 0.025 mm steps, which an agent never needs")
    {
        CHECK(mentions(refusal_of(control_body({{"action", "set_z_offset"}, {"z_offset", 0.013}}, idle)), "0.025"));
        CHECK_FALSE(control_body({{"action", "set_z_offset"}, {"z_offset", 0.025}}, idle).contains("refused"));
    }

    SECTION("a status that lacks a field the command sends back, which would go out as 0")
    {
        // A reply that parsed without a detail object: no Z offset, no fans. Sent on, a Z nudge would stop
        // the part-cooling fan and the chamber fan.
        FlashforgeApi::PrinterStatus bare = idle;
        bare.raw                          = json{{"code", 0}};
        const std::string refusal         = refusal_of(control_body({{"action", "set_z_offset"}, {"z_offset", 0.05}}, bare));
        CHECK(mentions(refusal, "coolingFanSpeed"));
        CHECK(mentions(refusal, "Nothing was sent"));

        FlashforgeApi::PrinterStatus no_cooling = idle;
        no_cooling.raw.erase("coolingFanSpeed");
        CHECK(mentions(refusal_of(control_body({{"action", "set_fans"}, {"chamber_fan", 20}}, no_cooling)), "coolingFanSpeed"));

        // The Pro (pid 41) has a chamber fan, so its speed is one to send back; a printer without one sends 0,
        // as the page does.
        FlashforgeApi::PrinterStatus pro_without_chamber = idle;
        pro_without_chamber.pid                          = 41;
        pro_without_chamber.raw.erase("chamberFanSpeed");
        CHECK(mentions(refusal_of(control_body({{"action", "set_z_offset"}, {"z_offset", 0.05}}, pro_without_chamber)), "chamberFanSpeed"));
        FlashforgeApi::PrinterStatus no_chamber = idle;
        no_chamber.raw.erase("chamberFanSpeed");
        CHECK_FALSE(control_body({{"action", "set_z_offset"}, {"z_offset", 0.05}}, no_chamber).contains("refused"));

        // One filtration fan reported and not the other: the other would go out as "close".
        FlashforgeApi::PrinterStatus one_fan = idle;
        one_fan.raw.erase("externalFanStatus");
        CHECK(mentions(refusal_of(control_body({{"action", "set_filtration"}, {"recirculation", true}}, one_fan)), "externalFanStatus"));
    }

    SECTION("what the printer's status says it cannot do now")
    {
        CHECK(mentions(refusal_of(control_body({{"action", "set_print_speed"}, {"speed", 125}}, idle)), "Nothing is printing"));

        FlashforgeApi::PrinterStatus no_filtration = idle;
        no_filtration.raw.erase("internalFanStatus");
        no_filtration.raw.erase("externalFanStatus");
        CHECK(mentions(refusal_of(control_body({{"action", "set_filtration"}, {"exhaust", true}}, no_filtration)), "no filtration fans"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_fans"}, {"cooling_left_fan", 40}}, idle)), "no left cooling fan"));
    }

    SECTION("a call that asks for nothing, or names an argument of another action")
    {
        CHECK(mentions(refusal_of(control_body({{"action", "set_fans"}}, idle)), "set_fans needs something to set"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_filtration"}}, idle)), "set_filtration needs something to set"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_print_speed"}}, printing)), "speed is required"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_z_offset"}}, idle)), "z_offset is required"));
        CHECK(refusal_of(control_body({{"action", "set_fans"}, {"speed", 125}}, printing)) ==
              "speed goes with action set_print_speed, not set_fans: leave it out, or send it with set_print_speed");
        CHECK(mentions(refusal_of(control_body({{"action", "pause"}, {"bed", 60}}, idle)), "bed goes with action set_temperature"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_filtration"}, {"recirculation", "sometimes"}}, idle)), "true (on) or false"));
        CHECK(mentions(refusal_of(control_body({{"action", "set_z_offset"}, {"z_offset", "up"}}, idle)), "z_offset must be a finite number"));
        CHECK(mentions(refusal_of(control_body({{"action", "reboot"}}, idle)), "Unknown action 'reboot'"));
    }

    SECTION("the tool itself refuses them before it looks the printer up")
    {
        // A call that got past its refusal would need the app, which this test does not have.
        const json answer = json::parse(OrcaMCPServer::handle_tools_call({{"name", "printer_control"}, {"arguments", {{"action", "set_fans"}}}})
                                            .at("content").at(0).at("text").get<std::string>());
        CHECK(answer["status"] == "error");
        CHECK(mentions(answer["message"], "set_fans needs something to set"));
    }
}

TEST_CASE("a status names the controls printer_control changes", "[McpPrinterControl][orcamcp]")
{
    const json controls = printer_controls_json(GUI::console_raw_detail(idle_printer().raw));
    CHECK(same_body(controls, json{{"print_speed_percent", nullptr}, // an idle printer reports 0
                           {"z_offset_mm", 0.05},
                           {"chamber_fan_percent", 30},
                           {"cooling_fan_percent", 70},
                           {"recirculation", false},
                           {"exhaust", true}}));

    CHECK(printer_controls_json(printing_printer().raw)["print_speed_percent"] == 100);

    json dual_head                   = idle_printer().raw;
    dual_head["coolingLeftFanSpeed"] = 25;
    CHECK(printer_controls_json(dual_head)["cooling_left_fan_percent"] == 25);

    json bare = json{{"coolingFanSpeed", 70}};
    CHECK(printer_controls_json(bare)["recirculation"].is_null());
    CHECK(printer_controls_json(json()) == json::object());

    // get_printer_status carries them in its printer object.
    CHECK(status_to_json(printing_printer())["controls"]["print_speed_percent"] == 100);
}
