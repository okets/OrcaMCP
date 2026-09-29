#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

// How send_to_printer and print_printer_file decide leveling, flow calibration and time-lapse:
// the agent's explicit choice, else the print-time gate; and when a send refuses an explicit true
// it would drop.

using namespace Slic3r::GUI::OrcaMCP;

namespace {

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// What the gate reads about a print on a Creator 5 Pro, the machine it was checked on.
GateFacts creator(std::optional<double> estimated_print_s)
{
    GateFacts facts;
    facts.estimated_print_s        = estimated_print_s;
    facts.model_offers_calibration = true;
    facts.printer_model            = "Creator 5 Pro";
    return facts;
}

// The same print on a Flashforge whose start screen nobody has checked.
GateFacts unchecked_model(std::optional<double> estimated_print_s)
{
    GateFacts facts;
    facts.estimated_print_s        = estimated_print_s;
    facts.model_offers_calibration = false;
    facts.printer_model            = "Adventurer 5M";
    return facts;
}

} // namespace

TEST_CASE("An explicit choice wins over the print-time gate", "[McpPrintOptions][orcamcp]")
{
    PrintOptionRequest request;
    request.leveling         = false;
    request.flow_calibration = true;
    request.time_lapse       = true;
    const auto choices = choose_print_options(request, creator(3 * 24 * 3600.0)); // a three-day print

    CHECK_FALSE(choices.leveling.on);
    CHECK(choices.leveling.decided_by == "caller");
    CHECK(choices.flow_calibration.on);
    CHECK(choices.flow_calibration.decided_by == "caller");
    CHECK(choices.time_lapse.on);
    CHECK(choices.time_lapse.decided_by == "caller");
}

TEST_CASE("Left to the tool, calibration runs from four hours", "[McpPrintOptions][orcamcp]")
{
    const auto at_gate = choose_print_options(PrintOptionRequest{}, creator(kCalibrationGateSeconds));
    CHECK(at_gate.leveling.on);
    CHECK(at_gate.flow_calibration.on);
    CHECK(at_gate.leveling.decided_by == "print_time_gate");

    const auto under = choose_print_options(PrintOptionRequest{}, creator(kCalibrationGateSeconds - 1));
    CHECK_FALSE(under.leveling.on);
    CHECK_FALSE(under.flow_calibration.on);
    CHECK(under.flow_calibration.decided_by == "print_time_gate");
}

TEST_CASE("Without an estimate, calibration stays off and says why", "[McpPrintOptions][orcamcp]")
{
    for (const std::optional<double> estimate : {std::optional<double>{}, std::optional<double>{0.0}, std::optional<double>{-5.0}}) {
        const auto choices = choose_print_options(PrintOptionRequest{}, creator(estimate));
        CHECK_FALSE(choices.leveling.on);
        CHECK(choices.leveling.decided_by == "print_time_unknown");
        CHECK_FALSE(choices.flow_calibration.on);
        CHECK(choices.flow_calibration.decided_by == "print_time_unknown");
    }
}

TEST_CASE("Time-lapse is off unless asked for, whatever the print's length", "[McpPrintOptions][orcamcp]")
{
    const auto choices = choose_print_options(PrintOptionRequest{}, creator(3 * 24 * 3600.0));
    CHECK_FALSE(choices.time_lapse.on);
    CHECK(choices.time_lapse.decided_by == "default");
}

TEST_CASE("read_print_option_request reads the three booleans", "[McpPrintOptions][orcamcp]")
{
    PrintOptionRequest request;
    std::string        error;
    REQUIRE(read_print_option_request(nlohmann::json{{"flow_calibration", true}, {"time_lapse", false}}, request, error));
    CHECK_FALSE(request.leveling.has_value());
    CHECK(request.flow_calibration == true);
    CHECK(request.time_lapse == false);
}

TEST_CASE("read_print_option_request refuses a value that is not a boolean", "[McpPrintOptions][orcamcp]")
{
    const std::vector<std::pair<std::string, nlohmann::json>> bad = {
        {"leveling_before_print", "true"}, {"flow_calibration", 1}, {"time_lapse", nullptr}};
    for (const auto& [name, value] : bad) {
        PrintOptionRequest request;
        std::string        error;
        CHECK_FALSE(read_print_option_request(nlohmann::json{{name, value}}, request, error));
        CHECK(error == name + " must be a boolean");
    }
}

TEST_CASE("needs_print_time only when calibration is left to the gate", "[McpPrintOptions][orcamcp]")
{
    CHECK(needs_print_time(PrintOptionRequest{}));

    PrintOptionRequest one;
    one.leveling = true;
    CHECK(needs_print_time(one));

    PrintOptionRequest both;
    both.leveling         = true;
    both.flow_calibration = false;
    CHECK_FALSE(needs_print_time(both));
}

TEST_CASE("print_options_json says what was sent and why", "[McpPrintOptions][orcamcp]")
{
    PrintOptionRequest request;
    request.flow_calibration = false;
    const GateFacts      facts = creator(5 * 3600.0);
    const nlohmann::json j     = print_options_json(choose_print_options(request, facts), facts);

    CHECK(j["leveling"] == nlohmann::json{{"on", true}, {"decided_by", "print_time_gate"}});
    CHECK(j["flow_calibration"] == nlohmann::json{{"on", false}, {"decided_by", "caller"}});
    CHECK(j["time_lapse"] == nlohmann::json{{"on", false}, {"decided_by", "default"}});
    CHECK(j["estimated_print_s"] == 18000);
    CHECK(j["gate_s"] == 14400);
    CHECK(j["printer_model"] == "Creator 5 Pro");

    const GateFacts unknown = creator(std::nullopt);
    CHECK(print_options_json(choose_print_options(PrintOptionRequest{}, unknown), unknown)["estimated_print_s"].is_null());

    // Every option given and no mapping to make: the printer's status was never read.
    PrintOptionRequest all_given;
    all_given.leveling = all_given.flow_calibration = all_given.time_lapse = false;
    const GateFacts not_read;
    CHECK(print_options_json(choose_print_options(all_given, not_read), not_read)["printer_model"].is_null());
}

TEST_CASE("to_print_options carries the choices to the printer", "[McpPrintOptions][orcamcp]")
{
    PrintOptionRequest request;
    request.time_lapse = true;
    const auto options = to_print_options(choose_print_options(request, creator(kCalibrationGateSeconds)));
    CHECK(options.leveling);
    CHECK(options.flow_calibration);
    CHECK(options.time_lapse);
}

TEST_CASE("On a model nobody checked, calibration left to the tool stays off, whatever the print's length", "[McpPrintOptions][orcamcp]")
{
    const GateFacts facts   = unchecked_model(3 * 24 * 3600.0);
    const auto      choices = choose_print_options(PrintOptionRequest{}, facts);
    CHECK_FALSE(choices.leveling.on);
    CHECK(choices.leveling.decided_by == "not_offered_by_model");
    CHECK_FALSE(choices.flow_calibration.on);
    CHECK(choices.flow_calibration.decided_by == "not_offered_by_model");
    CHECK_FALSE(choices.time_lapse.on);
    CHECK(choices.time_lapse.decided_by == "default");

    const nlohmann::json j = print_options_json(choices, facts);
    CHECK(j["printer_model"] == "Adventurer 5M");
    CHECK(j["leveling"] == nlohmann::json{{"on", false}, {"decided_by", "not_offered_by_model"}});
}

TEST_CASE("On a model nobody checked, an explicit choice still goes through", "[McpPrintOptions][orcamcp]")
{
    PrintOptionRequest request;
    request.leveling         = true;
    request.flow_calibration = true;
    request.time_lapse       = true;
    const auto choices = choose_print_options(request, unchecked_model(60.0));
    CHECK(choices.leveling.on);
    CHECK(choices.leveling.decided_by == "caller");
    CHECK(choices.flow_calibration.on);
    CHECK(choices.flow_calibration.decided_by == "caller");
    CHECK(choices.time_lapse.on);
}

TEST_CASE("The tool descriptions state the gate from the constant", "[McpPrintOptions][orcamcp]")
{
    CHECK(calibration_gate_text() == "4 h");
}

TEST_CASE("A send that carries the options refuses none", "[McpPrintOptions][orcamcp]")
{
    PrintOptionRequest all_on;
    all_on.leveling = all_on.flow_calibration = all_on.time_lapse = true;
    CHECK_FALSE(ignored_print_options_refusal(all_on, PrintOptionsReach::Honoured, "flashforge").has_value());
}

TEST_CASE("A send that would drop the options refuses an explicit true, and only that", "[McpPrintOptions][orcamcp]")
{
    PrintOptionRequest all_off;
    all_off.leveling = all_off.flow_calibration = all_off.time_lapse = false;
    PrintOptionRequest two_on;
    two_on.leveling   = true;
    two_on.time_lapse = true;

    for (const PrintOptionsReach reach : {PrintOptionsReach::SendDialog, PrintOptionsReach::UploadOnly,
                                          PrintOptionsReach::NotFlashforge, PrintOptionsReach::FlashforgeWithoutLocalApi}) {
        // Off is what happens anyway, and nothing asked is nothing to drop.
        CHECK_FALSE(ignored_print_options_refusal(all_off, reach, "moonraker").has_value());
        CHECK_FALSE(ignored_print_options_refusal(PrintOptionRequest{}, reach, "moonraker").has_value());

        const std::optional<std::string> refusal = ignored_print_options_refusal(two_on, reach, "moonraker");
        REQUIRE(refusal.has_value());
        CHECK(mentions(*refusal, "Nothing was sent"));
        CHECK(mentions(*refusal, "leveling_before_print: true"));
        CHECK(mentions(*refusal, "time_lapse: true"));
        CHECK_FALSE(mentions(*refusal, "flow_calibration"));
    }
}

TEST_CASE("The refusal says why the options would be dropped", "[McpPrintOptions][orcamcp]")
{
    PrintOptionRequest flow;
    flow.flow_calibration = true;

    const std::string dialog = ignored_print_options_refusal(flow, PrintOptionsReach::SendDialog, "flashforge").value();
    CHECK(mentions(dialog, "direct: false"));
    CHECK(mentions(dialog, "direct: true"));

    const std::string upload_only = ignored_print_options_refusal(flow, PrintOptionsReach::UploadOnly, "flashforge").value();
    CHECK(mentions(upload_only, "start_print: false"));
    CHECK(mentions(upload_only, "print_printer_file"));

    const std::string other_host = ignored_print_options_refusal(flow, PrintOptionsReach::NotFlashforge, "octoprint").value();
    CHECK(mentions(other_host, "'octoprint'"));
    CHECK(mentions(other_host, "Flashforge"));

    const std::string no_api = ignored_print_options_refusal(flow, PrintOptionsReach::FlashforgeWithoutLocalApi, "flashforge").value();
    CHECK(mentions(no_api, "serial number and check code"));
    CHECK(mentions(no_api, "add_physical_printer"));
}
