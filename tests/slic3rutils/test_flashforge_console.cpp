#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <string>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/FlashforgeConsoleHandler.hpp"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/FlashforgeApi.hpp"

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

// The same printer mid-job, where a print speed applies.
json printing_snapshot()
{
    json snapshot                          = idle_snapshot();
    snapshot["printer"]["state"]           = "printing";
    snapshot["printer"]["print_file"]      = "cube.gcode";
    snapshot["printer"]["raw"]["printSpeedAdjust"] = 100;
    return snapshot;
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
        const json op = build({{"name", "printer_ctl"}, {"speed", 125}}, printing_snapshot(), error);
        CHECK(op["cmd"] == "printerCtl_cmd");
        CHECK(op["args"]["speed"] == 125);
        CHECK(op["args"]["zAxisCompensation"] == 0.05); // the printer's own value, untouched
        CHECK(op["args"]["chamberFan"] == 30);
        CHECK(op["args"]["coolingFan"] == 70);
        // This machine reports no left cooling fan, so it is never told what to do with one:
        // a 0 we invented would stop the left part cooling on a printer that does have it.
        CHECK_FALSE(op["args"].contains("coolingLeftFan"));

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

    SECTION("a machine that does report a left cooling fan keeps its speed")
    {
        json snapshot = idle_snapshot();
        snapshot["printer"]["raw"]["coolingLeftFanSpeed"] = 55;

        const json op = build({{"name", "printer_ctl"}, {"zAxisCompensation", 0.1}}, snapshot, error);
        CHECK(op["args"]["coolingLeftFan"] == 55);
        CHECK(op["args"]["coolingFan"] == 70);

        // And it can still be set on purpose.
        CHECK(build({{"name", "printer_ctl"}, {"coolingLeftFan", 0}}, snapshot, error)["args"]["coolingLeftFan"] == 0);
    }

    SECTION("out-of-range control values are refused before they reach the printer")
    {
        CHECK(build({{"name", "printer_ctl"}, {"speed", 200}}, idle_snapshot(), error).is_null());
        CHECK(error.find("50") != std::string::npos);
        CHECK(build({{"name", "printer_ctl"}, {"zAxisCompensation", 4}}, idle_snapshot(), error).is_null());

        // A field that is present but not a number is a caller bug. Quietly keeping the printer's
        // current value would look like the command worked.
        CHECK(build({{"name", "printer_ctl"}, {"speed", "125"}}, idle_snapshot(), error).is_null());
        CHECK(error.find("speed") != std::string::npos);
        CHECK(build({{"name", "printer_ctl"}, {"zAxisCompensation", true}}, idle_snapshot(), error).is_null());
        CHECK(build({{"name", "filtration"}, {"internal", "on"}}, idle_snapshot(), error).is_null());
        CHECK(build({{"name", "filtration"}, {"external", 1}}, idle_snapshot(), error).is_null());
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

// ── What of the printer's own `detail` object reaches the page ───────────────────────────────────

namespace {

// A Creator 5 Pro's `detail`, trimmed to one field per group the page reads plus the vendor
// identifiers that come with it. The two register codes are the printer's cloud credentials and
// macAddr identifies the machine on the LAN; none of the three is any of the page's business.
json detail_snapshot()
{
    return json{{"nozzleCnt", 4},
                {"nozzleModel", "0.4mm;0.4mm;0.4mm;0.4mm"},
                {"measure", "256X256X256"},
                {"location", "Studio"},
                {"camera", 1},
                {"coolingFanSpeed", 78},
                {"internalFanStatus", "open"},
                {"externalFanStatus", "close"},
                {"tvoc", 0.12},
                {"remainingDiskSpace", 4.96},
                {"cumulativePrintTime", 19122},
                {"cumulativeFilament", 4820.5},
                {"printLayer", 412},
                {"targetPrintLayer", 1180},
                {"printSpeedAdjust", 100},
                {"zAxisCompensation", 0.02},
                {"chamberFanSpeed", 40},
                {"coolingLeftFanSpeed", 0},
                {"matlStationInfo", {{"slotCnt", 4}, {"currentSlot", 1}, {"stateAction", 1}, {"stateStep", 3}}},
                {"flashRegisterCode", "SECRET-FLASH"},
                {"polarRegisterCode", "SECRET-POLAR"},
                {"macAddr", "AA:BB:CC:DD:EE:FF"},
                {"ownerName", "someone"}};
}

} // namespace

TEST_CASE("only the fields the console reads leave the printer's detail object", "[flashforge][flashforge-console]")
{
    const json raw = Slic3r::GUI::console_raw_detail(detail_snapshot());

    SECTION("the vendor's identifiers do not reach the page")
    {
        CHECK_FALSE(raw.contains("flashRegisterCode"));
        CHECK_FALSE(raw.contains("polarRegisterCode"));
        CHECK_FALSE(raw.contains("macAddr"));
    }

    SECTION("a field nobody reads is dropped rather than passed through")
    {
        // The point of the allowlist: this key is not a known secret and was never on any deny
        // list, and it still does not reach the page, because nothing on the page reads it.
        CHECK_FALSE(raw.contains("ownerName"));
    }

    SECTION("every field the page renders survives, with its value")
    {
        for (const char* key : {"nozzleCnt", "nozzleModel", "measure", "location", "camera", "coolingFanSpeed",
                                "internalFanStatus", "externalFanStatus", "tvoc", "remainingDiskSpace",
                                "cumulativePrintTime", "cumulativeFilament", "printLayer", "targetPrintLayer",
                                "printSpeedAdjust", "zAxisCompensation", "matlStationInfo"})
            CHECK(raw.contains(key));
        CHECK(raw["nozzleModel"] == "0.4mm;0.4mm;0.4mm;0.4mm");
        // The station object goes through whole: the load strip is drawn from its stateAction and
        // stateStep, and the toolhead cards from currentSlot.
        CHECK(raw["matlStationInfo"]["stateStep"] == 3);
    }

    SECTION("every field a command carries survives, so one control cannot clobber another")
    {
        // printerCtl_cmd and circulateCtl_cmd send every field they own, read back out of this
        // same snapshot: a Z nudge that lost the fan speeds would send zeros for them.
        std::string error;
        const json  snapshot = json{{"connected", true}, {"printer", {{"raw", raw}}}};
        const json  op       = build({{"name", "printer_ctl"}, {"zAxisCompensation", 0.1}}, snapshot, error);
        CHECK(op["args"]["speed"] == 100);
        CHECK(op["args"]["chamberFan"] == 40);
        CHECK(op["args"]["coolingFan"] == 78);
        CHECK(op["args"]["coolingLeftFan"] == 0);

        const json fans = build({{"name", "filtration"}, {"external", "open"}}, snapshot, error);
        CHECK(fans["args"]["internal"] == "open");
    }

    SECTION("anything but an object answers with an empty object")
    {
        CHECK(Slic3r::GUI::console_raw_detail(json()) == json::object());
        CHECK(Slic3r::GUI::console_raw_detail(json::array({1, 2})) == json::object());
    }
}

// ── The limits every console command is held to, the page's and printer_control's alike ─────────

TEST_CASE("a print speed is sent only while the printer runs a job", "[flashforge][flashforge-console]")
{
    std::string error;

    CHECK(build({{"name", "printer_ctl"}, {"speed", 125}}, idle_snapshot(), error).is_null());
    CHECK(error.find("Nothing is printing") != std::string::npos);

    CHECK(build({{"name", "printer_ctl"}, {"speed", 125}}, printing_snapshot(), error)["args"]["speed"] == 125);

    json paused                        = printing_snapshot();
    paused["printer"]["state"]         = "paused";
    CHECK(build({{"name", "printer_ctl"}, {"speed", 50}}, paused, error)["args"]["speed"] == 50);

    // Heating up for a job counts as the job, as on the page; heating with no file does not.
    json heating                       = printing_snapshot();
    heating["printer"]["state"]        = "heating";
    CHECK_FALSE(build({{"name", "printer_ctl"}, {"speed", 166}}, heating, error).is_null());
    heating["printer"]["print_file"]   = "";
    CHECK(build({{"name", "printer_ctl"}, {"speed", 166}}, heating, error).is_null());

    // The Z offset and the fans work on an idle printer, as the page offers them there.
    CHECK_FALSE(build({{"name", "printer_ctl"}, {"zAxisCompensation", 0.05}}, idle_snapshot(), error).is_null());
    CHECK_FALSE(build({{"name", "printer_ctl"}, {"coolingFan", 20}}, idle_snapshot(), error).is_null());
}

TEST_CASE("a Z offset is sent only on the printer's 0.025 mm steps", "[flashforge][flashforge-console]")
{
    std::string error;

    CHECK(build({{"name", "printer_ctl"}, {"zAxisCompensation", -0.075}}, idle_snapshot(), error)["args"]["zAxisCompensation"] == -0.075);
    CHECK(build({{"name", "printer_ctl"}, {"zAxisCompensation", 1}}, idle_snapshot(), error)["args"]["zAxisCompensation"] == 1);

    CHECK(build({{"name", "printer_ctl"}, {"zAxisCompensation", 0.03}}, idle_snapshot(), error).is_null());
    CHECK(error.find("0.025") != std::string::npos);
    CHECK(build({{"name", "printer_ctl"}, {"zAxisCompensation", 1.025}}, idle_snapshot(), error).is_null());

    // The printer's own value is carried as it reports it, on a step or not: only a value sent is held
    // to the steps.
    json snapshot                                   = idle_snapshot();
    snapshot["printer"]["raw"]["zAxisCompensation"] = 0.013;
    CHECK(build({{"name", "printer_ctl"}, {"coolingFan", 50}}, snapshot, error)["args"]["zAxisCompensation"] == 0.013);
}

TEST_CASE("a fan speed is a percentage, for a fan the printer reports", "[flashforge][flashforge-console]")
{
    std::string error;

    const json op = build({{"name", "printer_ctl"}, {"chamberFan", 100}, {"coolingFan", 0}}, idle_snapshot(), error);
    CHECK(op["args"]["chamberFan"] == 100);
    CHECK(op["args"]["coolingFan"] == 0);

    CHECK(build({{"name", "printer_ctl"}, {"chamberFan", 101}}, idle_snapshot(), error).is_null());
    CHECK(error.find("chamberFan") != std::string::npos);
    CHECK(build({{"name", "printer_ctl"}, {"coolingFan", -1}}, idle_snapshot(), error).is_null());

    json no_chamber = idle_snapshot();
    no_chamber["printer"]["raw"].erase("chamberFanSpeed");
    CHECK(build({{"name", "printer_ctl"}, {"chamberFan", 30}}, no_chamber, error).is_null());
    CHECK(error.find("chamber fan") != std::string::npos);

    // A left cooling fan is set only on a printer that reports one.
    CHECK(build({{"name", "printer_ctl"}, {"coolingLeftFan", 40}}, idle_snapshot(), error).is_null());
    CHECK(error.find("left cooling fan") != std::string::npos);
    json dual_head                                     = idle_snapshot();
    dual_head["printer"]["raw"]["coolingLeftFanSpeed"] = 10;
    CHECK(build({{"name", "printer_ctl"}, {"coolingLeftFan", 40}}, dual_head, error)["args"]["coolingLeftFan"] == 40);
    CHECK(build({{"name", "printer_ctl"}, {"coolingLeftFan", 140}}, dual_head, error).is_null());
}

TEST_CASE("filtration is switched only on a printer that reports its filtration fans", "[flashforge][flashforge-console]")
{
    std::string error;
    json        no_filtration = idle_snapshot();
    no_filtration["printer"]["raw"].erase("internalFanStatus");
    no_filtration["printer"]["raw"].erase("externalFanStatus");

    CHECK(build({{"name", "filtration"}, {"internal", "open"}}, no_filtration, error).is_null());
    CHECK(error.find("filtration") != std::string::npos);
    CHECK_FALSE(build({{"name", "filtration"}, {"internal", "open"}}, idle_snapshot(), error).is_null());
}

TEST_CASE("the snapshot a status makes is the one the page's poller caches", "[flashforge][flashforge-console]")
{
    Slic3r::FlashforgeApi::PrinterStatus status;
    status.state      = "printing";
    status.print_file = "cube.gcode";
    status.raw        = json{{"printSpeedAdjust", 100}, {"zAxisCompensation", 0.05}, {"chamberFanSpeed", 30},
                             {"coolingFanSpeed", 70}, {"flashRegisterCode", "not for the page"}};

    const json snapshot = Slic3r::GUI::console_snapshot(status);
    CHECK(snapshot["connected"] == true);
    CHECK(snapshot["printer"]["state"] == "printing");
    CHECK_FALSE(snapshot["printer"]["raw"].contains("flashRegisterCode"));

    // Enough to build a command that carries the printer's own values.
    std::string error;
    const json  op = build({{"name", "printer_ctl"}, {"speed", 125}}, snapshot, error);
    CHECK(op["args"] == json{{"zAxisCompensation", 0.05}, {"speed", 125}, {"chamberFan", 30}, {"coolingFan", 70}});
}

TEST_CASE("an operation nobody built never reaches the printer", "[flashforge][flashforge-console]")
{
    // A host without the local API's credentials: a call that got as far as the network would fail on
    // those first, with its own message.
    Slic3r::DynamicPrintConfig config;
    config.set_key_value("print_host", new Slic3r::ConfigOptionString("192.0.2.1"));
    const Slic3r::Flashforge host(&config);

    wxString msg;
    CHECK_FALSE(Slic3r::GUI::run_console_operation(host, json{{"kind", "reboot"}}, msg));
    CHECK(msg.ToStdString().find("Unknown command") != std::string::npos);
    CHECK_FALSE(Slic3r::GUI::run_console_operation(host, json{{"kind", "job"}, {"action", "eject"}}, msg));
    CHECK(msg.ToStdString().find("Unknown job action") != std::string::npos);
}
