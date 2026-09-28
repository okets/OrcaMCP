#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/FlashforgeConsoleHandler.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp"
#include "slic3r/Utils/ObicoLink.hpp"

using json = nlohmann::json;
using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;

// What the printer tools answer with, built from everything a printer or a print host preset holds. Every
// secret below carries the same marker, so one search of the answer's text finds any of them.

namespace {

const std::string SECRET = "SECRET-";

// A Flashforge `detail` object with every field the Device page reads, and beside them the printer's cloud
// register codes, its local-API credentials and identifiers, and a field no firmware has sent yet.
json detail_with_secrets()
{
    return json{{"nozzleCnt", 4},
                {"nozzleModel", "0.4mm;0.4mm;0.4mm;0.4mm"},
                {"coolingFanSpeed", 78},
                {"chamberFanSpeed", 40},
                {"internalFanStatus", "open"},
                {"externalFanStatus", "close"},
                {"printSpeedAdjust", 100},
                {"zAxisCompensation", 0.025},
                {"matlStationInfo", {{"slotCnt", 4}, {"currentSlot", 1}}},
                {"flashRegisterCode", SECRET + "flash-register-code"},
                {"polarRegisterCode", SECRET + "polar-register-code"},
                {"checkCode", SECRET + "check-code"},
                {"serialNumber", SECRET + "serial-number"},
                {"macAddr", SECRET + "mac-address"},
                {"ownerName", SECRET + "owner"},
                {"aFieldNoFirmwareSentYet", SECRET + "future-field"}};
}

FlashforgeApi::PrinterStatus status_with_secrets()
{
    FlashforgeApi::PrinterStatus status;
    status.state                = "ready";
    status.name                 = "Studio C5P";
    status.has_material_station = true;
    status.slots                = {{1, true, "PLA", "#FF0000"}, {2, false, "", ""}};
    status.raw                  = detail_with_secrets();
    return status;
}

// A Flashforge print host preset with every credential a print host can hold set.
DynamicPrintConfig print_host_with_secrets()
{
    DynamicPrintConfig config;
    config.set_key_value("print_host", new ConfigOptionString("192.168.1.50"));
    config.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(htFlashforge));
    config.set_key_value("printhost_apikey", new ConfigOptionString(SECRET + "access-code"));
    config.set_key_value("printhost_user", new ConfigOptionString(SECRET + "user"));
    config.set_key_value("printhost_password", new ConfigOptionString(SECRET + "password"));
    config.set_key_value("flashforge_serial_number", new ConfigOptionString(SECRET + "serial-number"));
    config.set_key_value(OBICO_URL_KEY, new ConfigOptionString("https://obico.example"));
    config.set_key_value(OBICO_TOKEN_KEY, new ConfigOptionString(SECRET + "obico-token"));
    return config;
}

bool carries_a_secret(const json& answer) { return answer.dump().find(SECRET) != std::string::npos; }

} // namespace

TEST_CASE("a printer's status reaches an agent without its credentials or identifiers", "[McpPrinterAnswers]")
{
    const FlashforgeApi::PrinterStatus status = status_with_secrets();

    SECTION("get_printer_status's printer object")
    {
        const json printer = status_to_json(status);
        CHECK_FALSE(carries_a_secret(printer));
        // Still what the page and a control need: the allowlist is what decides, not a list of secrets.
        CHECK(printer["raw"]["internalFanStatus"] == "open");
        CHECK(printer["raw"]["zAxisCompensation"] == 0.025);
        CHECK(printer["material_station"]["slots"].size() == 2);
    }

    SECTION("its raw is exactly the Device page's allowlist of the printer's detail")
    {
        const json raw = status_to_json(status)["raw"];
        CHECK(raw == GUI::console_raw_detail(status.raw));
        CHECK(GUI::console_raw_detail(raw) == raw);
    }

    SECTION("get_printer_status's cached station, for a printer that could not be reached")
    {
        FlashforgeLocalApi::CachedStatus cached;
        cached.status = status;
        cached.age_s  = 42;
        const json answer = cached_station_json(cached);
        CHECK_FALSE(carries_a_secret(answer));
        CHECK(answer["age_s"] == 42);
        CHECK(answer["material_station"]["slots"].size() == 2);
    }
}

TEST_CASE("a print host preset's credentials never reach a printer tool's answer", "[McpPrinterAnswers]")
{
    const DynamicPrintConfig config = print_host_with_secrets();

    SECTION("get_printers' physical printer entry says only whether it has credentials")
    {
        const json entry = print_host_preset_json("C5P", config, true);
        CHECK_FALSE(carries_a_secret(entry));
        CHECK(entry["has_credentials"] == true);
        CHECK(entry["print_host"] == "192.168.1.50");
    }

    SECTION("add_physical_printer and select_printer answer without them")
    {
        const json answer = print_host_response_json("C5P", config);
        CHECK_FALSE(carries_a_secret(answer));
        CHECK(answer["host_type"] == "flashforge");
    }

    SECTION("get_printer_status's obico object gives the address alone")
    {
        const json obico = obico_status_json(config);
        CHECK_FALSE(carries_a_secret(obico));
        CHECK(obico["configured"] == true);
    }
}
