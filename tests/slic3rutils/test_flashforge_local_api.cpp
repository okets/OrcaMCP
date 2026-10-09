#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/FlashforgeLocalApi.hpp"
#include "test_utils.hpp"

#include <boost/nowide/fstream.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// How the Flashforge local API is reached. The printer is never contacted here: every rule is
// decided from plain strings and numbers, and the few tests that need an answer get it from a fake
// printer on this machine (FakePrinter below).

using namespace Slic3r::FlashforgeLocalApi;

TEST_CASE("host_of drops the port from every address shape", "[flashforge]")
{
    CHECK(host_of("10.0.0.100") == "10.0.0.100");
    // The latent bug: a print_host written as ip:port without a scheme kept its port, and the URL
    // came out as http://10.0.0.100:8080:8898/detail.
    CHECK(host_of("10.0.0.100:8080") == "10.0.0.100");
    CHECK(host_of("10.0.0.100:8898/") == "10.0.0.100");
    CHECK(host_of("http://10.0.0.100:8080/api") == "10.0.0.100");
    CHECK(host_of("https://printer.local") == "printer.local");
    CHECK(host_of("printer.local/path") == "printer.local");
    CHECK(host_of("[fe80::1]:80") == "[fe80::1]");
}

TEST_CASE("host_of strips any path, query, credentials and port it is given", "[flashforge]")
{
    CHECK(host_of("10.0.0.5/foo//bar") == "10.0.0.5");
    CHECK(host_of("10.0.0.5:8080//x/../y") == "10.0.0.5");
    CHECK(host_of("http://10.0.0.5:8080/a?b=c#d") == "10.0.0.5");
    CHECK(host_of("10.0.0.5?x=1") == "10.0.0.5");
    CHECK(host_of("10.0.0.5#frag") == "10.0.0.5");
    CHECK(host_of("user:secret@10.0.0.5:80") == "10.0.0.5");
    CHECK(host_of("HTTP://Printer.local") == "Printer.local");
    CHECK(host_of("10.0.0.5:") == "10.0.0.5");
    CHECK(host_of("10.0.0.5:not-a-port") == "10.0.0.5");
    CHECK(host_of("printer name with spaces:80/x") == "printer name with spaces");
}

TEST_CASE("host_of keeps an IPv6 literal in the brackets a URL needs", "[flashforge]")
{
    CHECK(host_of("[fe80::1]") == "[fe80::1]");
    CHECK(host_of("http://[fe80::1]:8080/x") == "[fe80::1]");
    CHECK(host_of("fe80::1") == "[fe80::1]"); // unbracketed: every colon is the address's own
}

TEST_CASE("host_of has nothing to return for an address with no host", "[flashforge]")
{
    CHECK(host_of("http://").empty());
    CHECK(host_of(":8080").empty());
    CHECK(host_of("/path").empty());
}

TEST_CASE("host_of ignores surrounding whitespace and keeps an empty address empty", "[flashforge]")
{
    CHECK(host_of("  10.0.0.100 ") == "10.0.0.100");
    CHECK(host_of("").empty());
    CHECK(host_of("   ").empty());
}

TEST_CASE("url_of always targets the local API port", "[flashforge]")
{
    CHECK(url_of("10.0.0.100", "detail") == "http://10.0.0.100:8898/detail");
    CHECK(url_of(host_of("10.0.0.100:8080"), "detail") == "http://10.0.0.100:8898/detail");
    CHECK(url_of("[fe80::1]", "gcodeList") == "http://[fe80::1]:8898/gcodeList");
}

// What Http hands on_error for a failure before any HTTP response: "curl:<summary>:\n<detail>\n[Error N]".
// curl 7.75 leaves <detail> empty when connect() fails at once on this computer, and fills it when
// the printer itself refuses; that difference is the whole diagnosis of the 2026-09-26 failure.
static const char* kImmediateConnectFailure = "curl:Couldn't connect to server:\n\n[Error 7]";
static const char* kRefused =
    "curl:Couldn't connect to server:\nFailed to connect to 10.0.0.100 port 8898: Connection refused\n[Error 7]";
static const char* kTimeout = "curl:Timeout was reached:\nConnection timed out after 10001 milliseconds\n[Error 28]";
static const char* kUnresolved = "curl:Couldn't resolve host name:\nCould not resolve host: printer.local\n[Error 6]";

TEST_CASE("curl_code_of reads the code Http appends, and 0 when there is none", "[flashforge]")
{
    CHECK(curl_code_of(kImmediateConnectFailure) == 7);
    CHECK(curl_code_of(kTimeout) == 28);
    CHECK(curl_code_of("") == 0);                          // an HTTP error status carries no curl text
    CHECK(curl_code_of("Error reading file for file upload") == 0);
    CHECK(curl_code_of("curl:odd:\n\n[Error x]") == 0);
}

TEST_CASE("curl_detail_of is empty exactly when curl described nothing", "[flashforge]")
{
    CHECK(curl_detail_of(kImmediateConnectFailure).empty());
    CHECK(curl_detail_of(kRefused) == "Failed to connect to 10.0.0.100 port 8898: Connection refused");
    CHECK(curl_detail_of("").empty());
}

TEST_CASE("should_retry retries a connection that was never made, once", "[flashforge]")
{
    CHECK(should_retry(kCurlCouldntConnect, 1, /*may_wait=*/true));
    CHECK_FALSE(should_retry(kCurlCouldntConnect, 2, true));
    // A timeout may have reached the printer, and a resolve failure will not fix itself in 500 ms.
    CHECK_FALSE(should_retry(kCurlOperationTimedout, 1, true));
    CHECK_FALSE(should_retry(kCurlCouldntResolveHost, 1, true));
    CHECK_FALSE(should_retry(0, 1, true));
}

TEST_CASE("should_retry never waits on a caller that may not block", "[flashforge]")
{
    // The GUI thread: the send dialog reads the material station from it.
    CHECK_FALSE(should_retry(kCurlCouldntConnect, 1, /*may_wait=*/false));
}

namespace {

// A fake request: fails with `errors[i]` on attempt i, succeeds once the list runs out.
struct ScriptedRequest
{
    std::vector<std::string> errors;
    int                      calls = 0;
    bool operator()(RequestFailure& failure)
    {
        if (calls >= int(errors.size())) {
            ++calls;
            return true;
        }
        failure       = {};
        failure.error = errors[calls++];
        return false;
    }
};

} // namespace

TEST_CASE("run_with_retry recovers from one refused connection", "[flashforge]")
{
    ScriptedRequest request{{kRefused}};
    std::vector<std::chrono::milliseconds> slept;
    RequestFailure failure;
    int            attempts = 0;

    CHECK(run_with_retry(std::ref(request), [&](std::chrono::milliseconds d) { slept.push_back(d); }, /*may_wait=*/true, failure, attempts));
    CHECK(attempts == 2);
    REQUIRE(slept.size() == 1);
    CHECK(slept.front() == kRetryDelay);
}

TEST_CASE("run_with_retry gives up after the second refused connection", "[flashforge]")
{
    ScriptedRequest request{{kRefused, kImmediateConnectFailure}};
    RequestFailure  failure;
    int             attempts = 0;

    CHECK_FALSE(run_with_retry(std::ref(request), [](std::chrono::milliseconds) {}, /*may_wait=*/true, failure, attempts));
    CHECK(attempts == 2);
    CHECK(failure.error == kImmediateConnectFailure); // the last failure is the one reported
}

TEST_CASE("run_with_retry does not repeat a timeout", "[flashforge]")
{
    ScriptedRequest request{{kTimeout}};
    RequestFailure  failure;
    int             attempts = 0;

    CHECK_FALSE(run_with_retry(std::ref(request), [](std::chrono::milliseconds) { FAIL("slept"); }, /*may_wait=*/true, failure, attempts));
    CHECK(attempts == 1);
}

TEST_CASE("run_with_retry makes one attempt for a caller that may not block", "[flashforge]")
{
    ScriptedRequest request{{kRefused}};
    RequestFailure  failure;
    int             attempts = 0;

    CHECK_FALSE(run_with_retry(std::ref(request), [](std::chrono::milliseconds) { FAIL("slept"); }, /*may_wait=*/false, failure, attempts));
    CHECK(attempts == 1);
    CHECK(request.calls == 1);
}

TEST_CASE("is_connectivity_failure is true only when the printer could not be talked to", "[flashforge]")
{
    CHECK(is_connectivity_failure(RequestFailure{kImmediateConnectFailure}));
    CHECK(is_connectivity_failure(RequestFailure{kRefused}));
    CHECK(is_connectivity_failure(RequestFailure{kTimeout}));
    CHECK(is_connectivity_failure(RequestFailure{kUnresolved}));
    CHECK(is_connectivity_failure(RequestFailure{"curl:Server returned nothing (no headers, no data):\n\n[Error 52]"}));
    CHECK(is_connectivity_failure(RequestFailure{"curl:Failure when receiving data from the peer:\n\n[Error 56]"}));

    // It answered: a wrong check code, an HTTP error status, a body that is not the API's.
    CHECK_FALSE(is_connectivity_failure(RequestFailure{"", 200, 12, "Flashforge local API error 1: check code wrong"}));
    CHECK_FALSE(is_connectivity_failure(RequestFailure{"", 500, 12}));
    CHECK_FALSE(is_connectivity_failure(RequestFailure{"curl:SSL connect error:\n\n[Error 35]"}));
    CHECK_FALSE(is_connectivity_failure(RequestFailure{}));
}

TEST_CASE("describe_failure names the host, the port and a next step", "[flashforge]")
{
    RequestFailure immediate{kImmediateConnectFailure, 0, 3};
    const std::string at_once = describe_failure("10.0.0.100", immediate, 2);
    CHECK(at_once.find("10.0.0.100:8898") != std::string::npos);
    CHECK(at_once.find("3 ms") != std::string::npos);
    CHECK(at_once.find("tried 2 times") != std::string::npos);
    CHECK(at_once.find("not on the network") != std::string::npos);
    CHECK(at_once.find("print_host") != std::string::npos);
#ifdef __APPLE__
    CHECK(at_once.find("System Settings > Privacy & Security > Local Network") != std::string::npos);
#else
    CHECK(at_once.find("Local Network") == std::string::npos);
#endif

    const std::string refused = describe_failure("10.0.0.100", RequestFailure{kRefused, 0, 40}, 2);
    CHECK(refused.find("10.0.0.100:8898") != std::string::npos);
    CHECK(refused.find("Connection refused") != std::string::npos);
    CHECK(refused.find("another device") != std::string::npos);

    const std::string timeout = describe_failure("10.0.0.100", RequestFailure{kTimeout, 0, 15002}, 1);
    CHECK(timeout.find("did not answer within 15 s") != std::string::npos);
    CHECK(timeout.find("tried") == std::string::npos); // one attempt says nothing about retrying

    const std::string unresolved = describe_failure("printer.local", RequestFailure{kUnresolved, 0, 5}, 1);
    CHECK(unresolved.find("printer.local") != std::string::npos);
    CHECK(unresolved.find("IP address") != std::string::npos);
}

TEST_CASE("failure_log_line carries the URL, the curl code and the timing, never a body", "[flashforge]")
{
    RequestFailure failure{kImmediateConnectFailure, 0, 3};
    const std::string line = failure_log_line("http://10.0.0.100:8898/detail", failure, 2);
    CHECK(line.find("http://10.0.0.100:8898/detail") != std::string::npos);
    CHECK(line.find("curl 7") != std::string::npos);
    CHECK(line.find("HTTP 0") != std::string::npos);
    CHECK(line.find("3 ms") != std::string::npos);
    CHECK(line.find("2 attempt") != std::string::npos);

    RequestFailure api{"", 200, 12, "Flashforge local API error 1: check code wrong"};
    CHECK(failure_log_line("http://10.0.0.100:8898/detail", api, 1).find("check code wrong") != std::string::npos);
}

TEST_CASE("should_log_failure logs a streak's first failure and then every 100th", "[flashforge]")
{
    CHECK(should_log_failure(1));
    CHECK_FALSE(should_log_failure(2));
    CHECK_FALSE(should_log_failure(99));
    CHECK(should_log_failure(100));
    CHECK(should_log_failure(200));
    CHECK_FALSE(should_log_failure(0));
}

TEST_CASE("FailureStreaks counts failures per host and reports the streak a success ends", "[flashforge]")
{
    FailureStreaks streaks;
    CHECK(streaks.record_failure("10.0.0.100") == 1);
    CHECK(streaks.record_failure("10.0.0.100") == 2);
    CHECK(streaks.record_failure("10.0.0.101") == 1);
    CHECK(streaks.record_success("10.0.0.100") == 2);
    CHECK(streaks.record_success("10.0.0.100") == 0);
    CHECK(streaks.record_failure("10.0.0.100") == 1);
}

namespace {

Slic3r::FlashforgeApi::PrinterStatus status_with_slot(const std::string& material)
{
    Slic3r::FlashforgeApi::PrinterStatus status;
    status.has_material_station = true;
    status.slots.push_back({1, true, material, "#FF0000"});
    return status;
}

} // namespace

TEST_CASE("StatusCache returns a host's last status with its age", "[flashforge]")
{
    using Clock = StatusCache::Clock;
    StatusCache     cache;
    const Clock::time_point t0 = Clock::now();

    CHECK_FALSE(cache.get("10.0.0.100", t0).has_value());

    cache.put("10.0.0.100", status_with_slot("PLA"), t0);
    cache.put("10.0.0.100", status_with_slot("PETG"), t0 + std::chrono::seconds(5)); // the newer one wins

    const auto cached = cache.get("10.0.0.100", t0 + std::chrono::seconds(47));
    REQUIRE(cached.has_value());
    CHECK(cached->age_s == 42);
    REQUIRE(cached->value.slots.size() == 1);
    CHECK(cached->value.slots.front().material_name == "PETG");

    CHECK_FALSE(cache.get("10.0.0.101", t0).has_value()); // another printer's status is not this one's
}

TEST_CASE("PrintStartLog keeps each printer's last accepted start with its age", "[flashforge]")
{
    using Clock = PrintStartLog::Clock;
    PrintStartLog           log;
    const Clock::time_point t0 = Clock::now();

    CHECK_FALSE(log.get("10.0.0.100", t0).has_value());

    PrintStart first;
    first.file_name = "cube.gcode.3mf";
    PrintStart second;
    second.file_name                = "vase.gcode.3mf";
    second.options.flow_calibration = true;
    second.material_mappings        = nlohmann::json::array({{{"toolId", 0}, {"slotId", 2}, {"materialName", "PLA"}}});

    log.put("10.0.0.100", first, t0);
    log.put("10.0.0.100", second, t0 + std::chrono::seconds(60)); // the newer one wins

    const auto recorded = log.get("10.0.0.100", t0 + std::chrono::seconds(2460));
    REQUIRE(recorded.has_value());
    CHECK(recorded->age_s == 2400);
    CHECK(recorded->value.file_name == "vase.gcode.3mf");
    CHECK(recorded->value.options.flow_calibration);
    CHECK(recorded->value.material_mappings.size() == 1);

    CHECK_FALSE(log.get("10.0.0.101", t0).has_value()); // another printer's start is not this one's
}

namespace {

// A Flashforge on this machine: the local API's own port on 127.0.0.1, answering every request with
// `reply` and keeping what it was sent. The port is the API's, so a machine where something else holds
// it skips the tests that need one.
class FakePrinter
{
public:
    explicit FakePrinter(std::string reply) : m_reply(std::move(reply)), m_server(kPort)
    {
        m_server.set_request_handler(Slic3r::GUI::HttpServer::RequestHandlerFn(
            [this](const std::string&, const std::string& url, const std::string& body, const Slic3r::GUI::http_headers&) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_requests.push_back({url, body});
                return std::make_shared<Slic3r::GUI::HttpServer::ResponseJson>(m_reply);
            }));
        m_listening = m_server.try_start().empty();
    }
    ~FakePrinter()
    {
        if (m_listening)
            m_server.stop();
    }

    bool listening() const { return m_listening; }

    // The body of the last request to `path` ("/printGcode"), empty when none came.
    std::string last_body(const std::string& path) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_requests.rbegin(); it != m_requests.rend(); ++it)
            if (it->first == path)
                return it->second;
        return {};
    }

private:
    std::string                                      m_reply;
    Slic3r::GUI::HttpServer                          m_server;
    bool                                             m_listening = false;
    mutable std::mutex                               m_mutex;
    std::vector<std::pair<std::string, std::string>> m_requests;
};

const char* const kPrinterAccepts = R"({"code":0,"message":"Success"})";
const char* const kPrinterRefuses = R"({"code":1,"message":"Printer is busy"})";

Slic3r::DynamicPrintConfig fake_printer_config()
{
    Slic3r::DynamicPrintConfig config;
    config.set_key_value("print_host", new Slic3r::ConfigOptionString("127.0.0.1"));
    config.set_key_value("flashforge_serial_number", new Slic3r::ConfigOptionString("SN-FAKE"));
    config.set_key_value("printhost_apikey", new Slic3r::ConfigOptionString("CC-FAKE"));
    return config;
}

// Whether the printer's last recorded start is `file_name`.
bool last_start_is(const Slic3r::Flashforge& host, const std::string& file_name)
{
    const auto recorded = host.last_print_start();
    return recorded.has_value() && recorded->value.file_name == file_name;
}

// Uploads a few bytes of G-code as `upload_name`, starting the print or not.
bool upload(const Slic3r::Flashforge& host, const std::string& upload_name, bool start_print,
            const Slic3r::FlashforgeApi::PrintOptions& options)
{
    ScopedTemporaryFile gcode(".gcode");
    {
        boost::nowide::ofstream out(gcode.string());
        out << "G28\n";
    }
    Slic3r::PrintHostUpload upload_data;
    upload_data.source_path   = gcode.path();
    upload_data.upload_path   = upload_name;
    upload_data.post_action   = start_print ? Slic3r::PrintHostPostUploadAction::StartPrint : Slic3r::PrintHostPostUploadAction::None;
    upload_data.extended_info = Slic3r::FlashforgeApi::make_upload_extended_info(options, false, nlohmann::json::array());
    return host.upload(
        std::move(upload_data), [](Slic3r::Http::Progress, bool&) {}, [](wxString) {}, [](wxString, wxString) {});
}

// Uploads `bytes` bytes of G-code as `upload_name` and starts it, carrying a slice table of three
// layers that describes a file of `table_bytes` bytes.
bool upload_sliced(const Slic3r::Flashforge& host, const std::string& upload_name, size_t bytes, std::uint64_t table_bytes)
{
    ScopedTemporaryFile gcode(".gcode");
    {
        boost::nowide::ofstream out(gcode.string());
        out << std::string(bytes, ';');
    }
    Slic3r::FlashforgeJobProgress::SliceTable table;
    table.file_bytes        = table_bytes;
    table.layer_start_bytes = {100, 200, 600};
    table.layer_start_s     = {10, 70, 80};
    table.total_s           = 100;

    Slic3r::PrintHostUpload upload_data;
    upload_data.source_path   = gcode.path();
    upload_data.upload_path   = upload_name;
    upload_data.post_action   = Slic3r::PrintHostPostUploadAction::StartPrint;
    upload_data.extended_info = Slic3r::FlashforgeApi::make_upload_extended_info({}, false, nlohmann::json::array());
    upload_data.extended_info[Slic3r::FlashforgeJobProgress::kExtendedInfoKey] = Slic3r::FlashforgeJobProgress::to_json(table);
    return host.upload(
        std::move(upload_data), [](Slic3r::Http::Progress, bool&) {}, [](wxString) {}, [](wxString, wxString) {});
}

// The printer's answer while it prints `file_name`, having read 80 % of it in 9000 s, by its own count on layer 2 of 3.
std::string printing_detail(const std::string& file_name)
{
    return R"({"code":0,"detail":{"status":"printing","printFileName":")" + file_name +
           R"(","printProgress":0.8,"printDuration":9000,"printLayer":2,"targetPrintLayer":3}})";
}

} // namespace

// One test case, its sections run one after another: the fake printer needs the API's own port, and
// ctest runs each test case in a process of its own, in parallel with -j.
TEST_CASE("A Flashforge on this machine's local API port records the starts it accepts", "[flashforge]")
{
    Slic3r::DynamicPrintConfig config = fake_printer_config();
    const Slic3r::Flashforge   host(&config);

    SECTION("A print the printer accepts is recorded with what it asked for")
    {
        FakePrinter printer(kPrinterAccepts);
        if (!printer.listening())
            SKIP("something else holds 127.0.0.1:" << kPort);

        Slic3r::FlashforgeApi::PrintOptions options;
        options.flow_calibration = true;
        const nlohmann::json mappings = nlohmann::json::array(
            {{{"toolId", 0}, {"slotId", 2}, {"materialName", "PLA"}, {"slotMaterialColor", "#FF0000"}}});
        wxString msg;
        REQUIRE(host.print_gcode_file("accepted-start.gcode", options, mappings, msg));

        const nlohmann::json sent = nlohmann::json::parse(printer.last_body("/printGcode"));
        CHECK(sent["flowCalibration"] == true);
        CHECK(sent["levelingBeforePrint"] == false);

        const auto recorded = host.last_print_start();
        REQUIRE(recorded.has_value());
        CHECK(recorded->value.file_name == "accepted-start.gcode");
        CHECK(recorded->value.options == options);
        CHECK(recorded->value.material_mappings == mappings);
        CHECK(recorded->age_s < 5);
    }

    SECTION("A start the printer refuses is not recorded")
    {
        FakePrinter printer(kPrinterRefuses);
        if (!printer.listening())
            SKIP("something else holds 127.0.0.1:" << kPort);

        wxString msg;
        CHECK_FALSE(host.print_gcode_file("refused-start.gcode", Slic3r::FlashforgeApi::PrintOptions{}, nlohmann::json::array(), msg));
        CHECK_FALSE(last_start_is(host, "refused-start.gcode"));

        CHECK_FALSE(upload(host, "refused-upload.gcode", true, Slic3r::FlashforgeApi::PrintOptions{}));
        CHECK_FALSE(last_start_is(host, "refused-upload.gcode"));
    }

    SECTION("An upload that starts the print is recorded, and an upload alone is not")
    {
        FakePrinter printer(kPrinterAccepts);
        if (!printer.listening())
            SKIP("something else holds 127.0.0.1:" << kPort);

        Slic3r::FlashforgeApi::PrintOptions options;
        options.leveling   = true;
        options.time_lapse = true;
        REQUIRE(upload(host, "upload-and-start.gcode", true, options));
        const auto recorded = host.last_print_start();
        REQUIRE(recorded.has_value());
        CHECK(recorded->value.file_name == "upload-and-start.gcode");
        CHECK(recorded->value.options == options);
        CHECK(recorded->value.material_mappings == nlohmann::json::array());

        REQUIRE(upload(host, "upload-only.gcode", false, options));
        CHECK(last_start_is(host, "upload-and-start.gcode")); // still the start before it
    }

    SECTION("A print sent with its slice is read by it; a file printed from the printer's storage is not")
    {
        Slic3r::FlashforgeApi::PrinterStatus status;
        wxString                             msg;
        {
            FakePrinter printer(kPrinterAccepts);
            if (!printer.listening())
                SKIP("something else holds 127.0.0.1:" << kPort);
            REQUIRE(upload_sliced(host, "sliced-start.gcode", 1000, 1000));
        }
        {
            FakePrinter printer(printing_detail("sliced-start.gcode"));
            REQUIRE(printer.listening());
            REQUIRE(host.fetch_status(status, msg));
        }
        CHECK(status.progress_source == "slice");
        CHECK(status.layer == 3);          // 80 % of the bytes is inside the third layer, whatever the printer counts
        CHECK(status.remaining_s == 1000); // 90 % of the slicer's time done in 9000 s
        CHECK_THAT(status.work_done, Catch::Matchers::WithinAbs(0.9, 1e-9));

        {
            FakePrinter printer(kPrinterAccepts);
            REQUIRE(printer.listening());
            REQUIRE(host.print_gcode_file("sliced-start.gcode", {}, nlohmann::json::array(), msg));
        }
        {
            FakePrinter printer(printing_detail("sliced-start.gcode"));
            REQUIRE(printer.listening());
            REQUIRE(host.fetch_status(status, msg));
        }
        CHECK(status.progress_source == "printer"); // a start from storage carries no slice
        CHECK(status.layer == 2);
    }

    SECTION("A plain G-code upload whose file is not the sliced one keeps the printer's numbers")
    {
        Slic3r::FlashforgeApi::PrinterStatus status;
        wxString                             msg;
        {
            FakePrinter printer(kPrinterAccepts);
            if (!printer.listening())
                SKIP("something else holds 127.0.0.1:" << kPort);
            REQUIRE(upload_sliced(host, "post-processed.gcode", 1200, 1000)); // a script grew the copy
        }
        {
            FakePrinter printer(printing_detail("post-processed.gcode"));
            REQUIRE(printer.listening());
            REQUIRE(host.fetch_status(status, msg));
        }
        CHECK(status.progress_source == "printer");
        CHECK(status.layer == 2);
    }

    SECTION("The material-slot read also says which machine it is")
    {
        FakePrinter printer(R"({"code":0,"detail":{"pid":41,"model":"Creator 5 Pro","hasMatlStation":true,
                                "matlStationInfo":{"slotCnt":1,"slotInfos":[{"slotId":1,"hasFilament":true,"materialName":"PLA"}]}}})");
        if (!printer.listening())
            SKIP("something else holds 127.0.0.1:" << kPort);

        std::vector<Slic3r::FlashforgeMaterialSlot> slots;
        bool                                        station    = false;
        int                                         product_id = 0;
        wxString                                    msg;
        REQUIRE(host.fetch_material_slots(slots, &station, msg, &product_id));
        CHECK(product_id == Slic3r::FlashforgeApi::kPidCreator5Pro);
        CHECK(station);
        CHECK(slots.size() == 1);
    }
}

TEST_CASE("A Flashforge host reads its print_host once, when it is built", "[flashforge]")
{
    Slic3r::DynamicPrintConfig config;
    config.set_key_value("print_host", new Slic3r::ConfigOptionString("http://10.0.0.5:8080/foo//bar"));
    const Slic3r::Flashforge host(&config);
    CHECK(host.local_api_host() == "10.0.0.5");

    // The config is not consulted again: the host a request goes to is fixed at construction.
    config.set_key_value("print_host", new Slic3r::ConfigOptionString("10.0.0.6"));
    CHECK(host.local_api_host() == "10.0.0.5");
}
