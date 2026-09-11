#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"

// The scalar guards every MCP tool reads its parameters through. They exist because a client whose
// cached tool schema predates a parameter sends it as a string, and because the standard library's
// own string-to-number conversions are lenient in ways that turn a caller's mistake into a
// different number rather than an error.

using Slic3r::GUI::OrcaMCP::parse_double_param;

TEST_CASE("parse_double_param takes a JSON number or its string spelling", "[orcamcp][params]")
{
    double out = -1.0;
    REQUIRE(parse_double_param(nlohmann::json(12.5), out));
    CHECK(out == 12.5);
    REQUIRE(parse_double_param(nlohmann::json(12), out)); // an integer literal is a number too
    CHECK(out == 12.0);
    REQUIRE(parse_double_param(nlohmann::json("110"), out)); // the stale-schema client's spelling
    CHECK(out == 110.0);
    REQUIRE(parse_double_param(nlohmann::json("-0.5"), out));
    CHECK(out == -0.5);
    REQUIRE(parse_double_param(nlohmann::json("1e2"), out));
    CHECK(out == 100.0);
}

// std::stod reads "0x10" as 16 and "0x1p4" as 16 as well, consuming the whole string in both
// cases, so neither the fully-consumed check nor isfinite can tell them from a deliberate value.
// A coordinate written in base 16 is never what a caller meant.
TEST_CASE("parse_double_param refuses a hexadecimal spelling", "[orcamcp][params]")
{
    double out = 99.0;
    CHECK_FALSE(parse_double_param(nlohmann::json("0x10"), out));
    CHECK_FALSE(parse_double_param(nlohmann::json("0X10"), out));
    CHECK_FALSE(parse_double_param(nlohmann::json("0x1p4"), out));
    CHECK(out == 99.0); // a rejected parse leaves the caller's value alone
}

// A NaN defeats every range check written as a comparison -- each one is false against NaN -- and
// reaches the coord_t conversion in Brim.cpp as undefined behaviour.
TEST_CASE("parse_double_param refuses a non-finite value", "[orcamcp][params]")
{
    double out = 0.0;
    CHECK_FALSE(parse_double_param(nlohmann::json("nan"), out));
    CHECK_FALSE(parse_double_param(nlohmann::json("NaN"), out));
    CHECK_FALSE(parse_double_param(nlohmann::json("inf"), out));
    CHECK_FALSE(parse_double_param(nlohmann::json("-Infinity"), out));
}

TEST_CASE("parse_double_param refuses what is not a number at all", "[orcamcp][params]")
{
    double out = 0.0;
    CHECK_FALSE(parse_double_param(nlohmann::json("12mm"), out)); // trailing garbage
    CHECK_FALSE(parse_double_param(nlohmann::json(""), out));
    CHECK_FALSE(parse_double_param(nlohmann::json(" "), out));
    CHECK_FALSE(parse_double_param(nlohmann::json(true), out));
    CHECK_FALSE(parse_double_param(nlohmann::json(nullptr), out));
    CHECK_FALSE(parse_double_param(nlohmann::json::array({1, 2}), out));
    CHECK_FALSE(parse_double_param(nlohmann::json::object(), out));
}
