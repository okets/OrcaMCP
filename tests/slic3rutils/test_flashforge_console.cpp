#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <string>

#include "slic3r/GUI/FlashforgeConsoleHandler.hpp"

using json = nlohmann::json;
using Slic3r::GUI::build_console_operation;

namespace {

// The shape the console page's poller caches: exactly what the printer reported, so the fields a
// control is not changing can be carried through untouched. These are the values the bench Creator
// 5 Pro reports when it is sitting idle.
json idle_snapshot()
{
    return json{{"connected", true},
                {"printer",
                 {{"state", "ready"},
                  {"light_on", false},
                  {"raw",
                   {{"internalFanStatus", "close"},
                    {"externalFanStatus", "open"},
                    {"zAxisCompensation", 0.05},
                    {"printSpeedAdjust", 0.0},
                    {"chamberFanSpeed", 30},
                    {"coolingFanSpeed", 70}}}}}};
}

json build(const json& params, const json& snapshot, std::string& error)
{
    json operation;
    error.clear();
    if (!build_console_operation(params, snapshot, operation, error))
        return json();
    return operation;
}

} // namespace

TEST_CASE("Flashforge console commands map onto printer calls", "[flashforge][flashforge-console]")
{
    std::string error;

    SECTION("light carries the requested state")
    {
        const json op = build({{"name", "light"}, {"on", true}}, idle_snapshot(), error);
        CHECK(op == json{{"kind", "light"}, {"on", true}});

        CHECK(build({{"name", "light"}, {"on", false}}, idle_snapshot(), error)["on"] == false);
        CHECK(build({{"name", "light"}}, idle_snapshot(), error).is_null());
        CHECK_FALSE(error.empty());
    }

    // Pause, resume and stop are wired but deliberately never issued against the bench printer:
    // there is no job to interrupt and nobody standing at the machine. Their shaping is verified
    // here instead, which is the whole reason the mapping is a pure function.
    SECTION("job actions map to pause, resume and cancel")
    {
        CHECK(build({{"name", "job"}, {"action", "pause"}}, idle_snapshot(), error) ==
              json{{"kind", "job"}, {"action", "pause"}});
        CHECK(build({{"name", "job"}, {"action", "resume"}}, idle_snapshot(), error) ==
              json{{"kind", "job"}, {"action", "resume"}});
        CHECK(build({{"name", "job"}, {"action", "stop"}}, idle_snapshot(), error) ==
              json{{"kind", "job"}, {"action", "stop"}});

        CHECK(build({{"name", "job"}, {"action", "eject"}}, idle_snapshot(), error).is_null());
        CHECK(error.find("eject") != std::string::npos);
    }

    SECTION("a temperature target of 0 means off, and an absent one means leave alone")
    {
        const json op = build({{"name", "temperature"}, {"nozzles", {nullptr, 40, nullptr, nullptr}}},
                              idle_snapshot(), error);
        CHECK(op["kind"] == "temperature");
        CHECK(op["bed"].is_null());
        CHECK(op["chamber"].is_null());
        CHECK(op["nozzles"] == json({nullptr, 40, nullptr, nullptr}));

        // Cool down: every target explicitly zero, which is an instruction, not a no-change.
        const json cool = build({{"name", "temperature"}, {"bed", 0}, {"chamber", 0}, {"nozzles", {0, 0, 0, 0}}},
                                idle_snapshot(), error);
        CHECK(cool["bed"] == 0);
        CHECK(cool["chamber"] == 0);
        CHECK(cool["nozzles"] == json({0, 0, 0, 0}));

        // The nozzle array is always four long, whatever the page sent.
        CHECK(build({{"name", "temperature"}, {"nozzles", {55}}}, idle_snapshot(), error)["nozzles"] ==
              json({55, nullptr, nullptr, nullptr}));
    }

    SECTION("temperature targets outside the machine's range are refused")
    {
        CHECK(build({{"name", "temperature"}, {"bed", 400}}, idle_snapshot(), error).is_null());
        CHECK(error.find("Bed target") != std::string::npos);
        CHECK(build({{"name", "temperature"}, {"nozzles", {900}}}, idle_snapshot(), error).is_null());
        CHECK(build({{"name", "temperature"}, {"chamber", -5}}, idle_snapshot(), error).is_null());
    }

    SECTION("filtration keeps the side it was not asked to change")
    {
        const json op = build({{"name", "filtration"}, {"internal", "open"}}, idle_snapshot(), error);
        CHECK(op["cmd"] == "circulateCtl_cmd");
        // The snapshot has the exhaust open; toggling recirculation must not close it.
        CHECK(op["args"] == json{{"internal", "open"}, {"external", "open"}});

        CHECK(build({{"name", "filtration"}, {"external", "close"}}, idle_snapshot(), error)["args"] ==
              json({{"internal", "close"}, {"external", "close"}}));
    }

    SECTION("printerCtl_cmd carries every field, so one control cannot clobber another")
    {
        const json op = build({{"name", "printer_ctl"}, {"speed", 125}}, idle_snapshot(), error);
        CHECK(op["cmd"] == "printerCtl_cmd");
        CHECK(op["args"]["speed"] == 125);
        CHECK(op["args"]["zAxisCompensation"] == 0.05); // the printer's own value, untouched
        CHECK(op["args"]["chamberFan"] == 30);
        CHECK(op["args"]["coolingFan"] == 70);
        CHECK(op["args"]["coolingLeftFan"] == 0);

        // Nudging Z leaves the fans alone, and does not send back the 0 % speed an idle printer
        // reports - which the machine would read as "stop moving".
        const json z = build({{"name", "printer_ctl"}, {"zAxisCompensation", 0.075}}, idle_snapshot(), error);
        CHECK(z["args"]["zAxisCompensation"] == 0.075);
        CHECK(z["args"]["speed"] == 100);
        CHECK(z["args"]["chamberFan"] == 30);
        CHECK(z["args"]["coolingFan"] == 70);
    }

    SECTION("a printing machine's own speed is what a Z nudge carries")
    {
        json snapshot = idle_snapshot();
        snapshot["printer"]["raw"]["printSpeedAdjust"] = 166;
        CHECK(build({{"name", "printer_ctl"}, {"zAxisCompensation", 0}}, snapshot, error)["args"]["speed"] == 166);
    }

    SECTION("out-of-range control values are refused before they reach the printer")
    {
        CHECK(build({{"name", "printer_ctl"}, {"speed", 200}}, idle_snapshot(), error).is_null());
        CHECK(error.find("50") != std::string::npos);
        CHECK(build({{"name", "printer_ctl"}, {"zAxisCompensation", 4}}, idle_snapshot(), error).is_null());
    }

    SECTION("a control that needs the printer's current settings waits for a status")
    {
        const json nothing_yet = json{{"connected", false}, {"connecting", true}};
        CHECK(build({{"name", "printer_ctl"}, {"speed", 100}}, nothing_yet, error).is_null());
        CHECK_FALSE(error.empty());
        CHECK(build({{"name", "filtration"}, {"internal", "open"}}, nothing_yet, error).is_null());

        // Light, job control and temperatures need nothing carried over, so they still work.
        CHECK_FALSE(build({{"name", "light"}, {"on", true}}, nothing_yet, error).is_null());
        CHECK_FALSE(build({{"name", "job"}, {"action", "pause"}}, nothing_yet, error).is_null());
        CHECK_FALSE(build({{"name", "temperature"}, {"bed", 0}}, nothing_yet, error).is_null());
    }

    SECTION("an unknown command is refused by name")
    {
        CHECK(build({{"name", "reboot"}}, idle_snapshot(), error).is_null());
        CHECK(error.find("reboot") != std::string::npos);
    }
}
