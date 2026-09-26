#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/FlashforgeLocalApi.hpp"

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

TEST_CASE("host_of ignores surrounding whitespace and keeps an empty address empty", "[flashforge]")
{
    CHECK(host_of("  10.0.0.100 ") == "10.0.0.100");
    CHECK(host_of("").empty());
    CHECK(host_of("   ").empty());
}

TEST_CASE("url_of always targets the local API port", "[flashforge]")
{
    CHECK(url_of("10.0.0.100", "detail") == "http://10.0.0.100:8898/detail");
    CHECK(url_of("10.0.0.100:8080", "detail") == "http://10.0.0.100:8898/detail");
    CHECK(url_of("http://printer.local:80/", "gcodeList") == "http://printer.local:8898/gcodeList");
}
