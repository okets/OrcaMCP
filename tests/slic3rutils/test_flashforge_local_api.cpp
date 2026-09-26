#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/FlashforgeLocalApi.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

// How the Flashforge local API is reached. The printer is never contacted here: every rule is
// decided from plain strings and numbers.

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
    CHECK(should_retry(kCurlCouldntConnect, 1));
    CHECK_FALSE(should_retry(kCurlCouldntConnect, 2));
    // A timeout may have reached the printer, and a resolve failure will not fix itself in 500 ms.
    CHECK_FALSE(should_retry(kCurlOperationTimedout, 1));
    CHECK_FALSE(should_retry(kCurlCouldntResolveHost, 1));
    CHECK_FALSE(should_retry(0, 1));
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

    CHECK(run_with_retry(std::ref(request), [&](std::chrono::milliseconds d) { slept.push_back(d); }, failure, attempts));
    CHECK(attempts == 2);
    REQUIRE(slept.size() == 1);
    CHECK(slept.front() == kRetryDelay);
}

TEST_CASE("run_with_retry gives up after the second refused connection", "[flashforge]")
{
    ScriptedRequest request{{kRefused, kImmediateConnectFailure}};
    RequestFailure  failure;
    int             attempts = 0;

    CHECK_FALSE(run_with_retry(std::ref(request), [](std::chrono::milliseconds) {}, failure, attempts));
    CHECK(attempts == 2);
    CHECK(failure.error == kImmediateConnectFailure); // the last failure is the one reported
}

TEST_CASE("run_with_retry does not repeat a timeout", "[flashforge]")
{
    ScriptedRequest request{{kTimeout}};
    RequestFailure  failure;
    int             attempts = 0;

    CHECK_FALSE(run_with_retry(std::ref(request), [](std::chrono::milliseconds) { FAIL("slept"); }, failure, attempts));
    CHECK(attempts == 1);
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
    REQUIRE(cached->status.slots.size() == 1);
    CHECK(cached->status.slots.front().material_name == "PETG");

    CHECK_FALSE(cache.get("10.0.0.101", t0).has_value()); // another printer's status is not this one's
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
