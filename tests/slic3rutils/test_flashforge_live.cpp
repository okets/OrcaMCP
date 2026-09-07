#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
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

    // pause_job/resume_job/cancel_job/print_gcode_file are intentionally not exercised here:
    // there is no job running on the bench printer, and issuing job control or starting a
    // print with no operator present would be unsafe. Their compilation is still verified by
    // the rest of the codebase (Flashforge.cpp) building successfully.
}
