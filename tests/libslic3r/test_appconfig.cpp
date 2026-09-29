#include <catch2/catch_all.hpp>

#include "libslic3r/AppConfig.hpp"

using namespace Slic3r;

TEST_CASE("AppConfig network version helpers", "[AppConfig]") {
    AppConfig config;

    SECTION("skipped versions starts empty") {
        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.empty());
    }

    SECTION("add and check skipped version") {
        config.add_skipped_network_version("02.01.01.52");
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE_FALSE(config.is_network_version_skipped("02.03.00.62"));
    }

    SECTION("multiple skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.00.02.50");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 2);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE(config.is_network_version_skipped("02.00.02.50"));
    }

    SECTION("clear skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.clear_skipped_network_versions();
        REQUIRE_FALSE(config.is_network_version_skipped("02.01.01.52"));
    }

    SECTION("duplicate add is idempotent") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.01.01.52");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 1);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
    }
}

TEST_CASE("A text value is stored as the text it is", "[AppConfig]") {
    // A const char* converts to bool before it converts to std::string, so without an overload of its
    // own set(section, key, "0") took the bool overload and stored "true".
    AppConfig config;
    config.set("recent", "literal", "1");
    CHECK(config.get("recent", "literal") == "1");

    for (const bool on : {true, false}) {
        config.set("recent", "ternary", on ? "1" : "0");
        CHECK(config.get("recent", "ternary") == (on ? "1" : "0"));
    }

    const char* const none = nullptr;
    config.set("recent", "null", none);
    CHECK(config.get("recent", "null").empty());
}

TEST_CASE("A bool is still stored as true or false", "[AppConfig]") {
    AppConfig config;
    config.set("recent", "flag", true);
    CHECK(config.get("recent", "flag") == "true");
    config.set("recent", "flag", false);
    CHECK(config.get("recent", "flag") == "false");
}

TEST_CASE("get_bool reads its own section", "[AppConfig]") {
    AppConfig config;
    config.set("firstguide", "finish", std::string("1"));
    CHECK(config.get_bool("firstguide", "finish"));
    config.set("firstguide", "finish", std::string("true"));
    CHECK(config.get_bool("firstguide", "finish"));
    config.set("firstguide", "finish", std::string("false"));
    CHECK_FALSE(config.get_bool("firstguide", "finish"));

    // A key of the same name in the app section says nothing about this one.
    config.set("app", "finish", std::string("1"));
    CHECK_FALSE(config.get_bool("firstguide", "finish"));
}
