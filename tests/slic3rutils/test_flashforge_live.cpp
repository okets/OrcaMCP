#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/FlashforgeApi.hpp"

using namespace Slic3r;

namespace {

// Reads an environment variable, returning an empty optional if it is unset or empty.
// NEVER log or print the value of FF_CHECK_CODE - it is a printer access credential.
std::optional<std::string> env(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
        return std::nullopt;
    return std::string(value);
}

} // namespace

// This test talks to a real, physical Flashforge printer over the local network and is only
// runnable with credentials supplied via environment variables (FF_HOST, FF_SERIAL,
// FF_CHECK_CODE). It is excluded from normal CI/unit-test runs by its absence from any
// default tag filter; run it explicitly with the "[flashforge-live]" tag.
TEST_CASE("Flashforge host talks to a real printer", "[flashforge-live]")
{
    const auto host        = env("FF_HOST");
    const auto serial      = env("FF_SERIAL");
    const auto check_code  = env("FF_CHECK_CODE");

    if (!host || !serial || !check_code)
        SKIP("no printer credentials in environment");

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("print_host", new ConfigOptionString(*host));
    config.set_key_value("flashforge_serial_number", new ConfigOptionString(*serial));
    config.set_key_value("printhost_apikey", new ConfigOptionString(*check_code));
    config.set_deserialize_strict("host_type", "flashforge");

    Flashforge printer_host(&config);

    SECTION("fetch_status reports a Creator 5 with 4 nozzles")
    {
        FlashforgeApi::PrinterStatus status;
        wxString                     msg;
        REQUIRE(printer_host.fetch_status(status, msg));
        CHECK(status.nozzles.size() == 4);
        CHECK(status.model.find("Creator 5") != std::string::npos);
    }

    SECTION("set_light toggles the chamber light on and off")
    {
        wxString msg;
        CHECK(printer_host.set_light(true, msg));
        CHECK(printer_host.set_light(false, msg));
    }

    SECTION("list_gcode_files succeeds (files may be empty)")
    {
        std::vector<std::string> files;
        wxString                 msg;
        CHECK(printer_host.list_gcode_files(files, msg));
    }

    SECTION("set_temperatures with no changes requested succeeds")
    {
        wxString msg;
        const std::vector<std::optional<double>> no_change_nozzles;
        CHECK(printer_host.set_temperatures(std::nullopt, std::nullopt, no_change_nozzles, msg));
    }

    // The Flashforge console page holds every control in a pending state until the printer's own
    // next status confirms the change, so what actually matters is that a command comes back in
    // /detail. These two do, within one poll.
    SECTION("the light the printer reports follows the light that was set")
    {
        wxString msg;
        FlashforgeApi::PrinterStatus status;
        REQUIRE(printer_host.fetch_status(status, msg));
        const bool was_on = status.light_on;

        REQUIRE(printer_host.set_light(!was_on, msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        REQUIRE(printer_host.fetch_status(status, msg));
        CHECK(status.light_on == !was_on);

        REQUIRE(printer_host.set_light(was_on, msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        REQUIRE(printer_host.fetch_status(status, msg));
        CHECK(status.light_on == was_on);
    }

    SECTION("a nozzle target the printer reports follows the target that was set")
    {
        wxString msg;
        FlashforgeApi::PrinterStatus status;

        // 40 C is below every filament's softening point, so nothing melts and nothing extrudes.
        REQUIRE(printer_host.set_temperatures(std::nullopt, std::nullopt, {40.0}, msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        REQUIRE(printer_host.fetch_status(status, msg));
        REQUIRE(status.nozzles.size() >= 1);
        CHECK(status.nozzles[0].target == 40);

        // An empty optional really is "leave alone": setting the bed while naming no nozzle must
        // not cool the one that is already holding 40. This is the distinction the console page
        // depends on for every one-field-at-a-time control it offers.
        REQUIRE(printer_host.set_temperatures(30.0, std::nullopt, {}, msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        REQUIRE(printer_host.fetch_status(status, msg));
        CHECK(status.bed_target == 30);
        CHECK(status.nozzles[0].target == 40);

        // Cool down: an explicit 0 is "off", not "leave alone" - the sentinel for that is -200.
        REQUIRE(printer_host.set_temperatures(0.0, 0.0, {0.0, 0.0, 0.0, 0.0}, msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        REQUIRE(printer_host.fetch_status(status, msg));
        CHECK(status.nozzles[0].target == 0);
        CHECK(status.bed_target == 0);
    }

    // pause_job/resume_job/cancel_job/print_gcode_file are intentionally not exercised here:
    // there is no job running on the bench printer, and issuing job control or starting a
    // print with no operator present would be unsafe. Their compilation is still verified by
    // the rest of the codebase (Flashforge.cpp) building successfully, and the console page's
    // mapping onto them by test_flashforge_console.cpp.
    //
    // printerCtl_cmd (print speed, Z compensation, the fans) and circulateCtl_cmd (recirculate,
    // exhaust) are not exercised either, and for a different reason: on firmware 1.9.9 an idle
    // machine answers both with {"code":0,"message":"Success"} and then changes nothing -
    // printSpeedAdjust, zAxisCompensation, chamberFanSpeed, coolingFanSpeed, internalFanStatus
    // and externalFanStatus all stay where they were, even for a chamber fan commanded to 100 %.
    // They are print-time controls, which is why the console page disables them unless a job is
    // running. A test that asserted that negative would be slow and would still be guessing.
}
