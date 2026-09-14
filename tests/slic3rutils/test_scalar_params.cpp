#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"

// The scalar guards every MCP tool reads its parameters through. They exist because a client whose
// cached tool schema predates a parameter sends it as a string, and because the standard library's
// own string-to-number conversions are lenient in ways that turn a caller's mistake into a
// different number rather than an error.

using Slic3r::GUI::OrcaMCP::parse_double_param;
using Slic3r::GUI::OrcaMCP::parse_integer_param;

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

// parse_integer_param is the reason a tool accepts the parameter its caller actually sent. Two
// clients in normal use do not send a plain JSON integer: one whose JSON layer widens every number
// to a double sends 3 as 3.0, and one working from a cached tool schema sends "3". A handler that
// tests nlohmann's is_number_integer() directly refuses both -- which is what a newly added
// plate_index parameter did to the very first live call it ever received.
TEST_CASE("parse_integer_param takes the spellings real clients send", "[orcamcp][params]")
{
    int out = -1;
    REQUIRE(parse_integer_param(nlohmann::json(3), out));
    CHECK(out == 3);
    REQUIRE(parse_integer_param(nlohmann::json(3.0), out)); // the number-widening client
    CHECK(out == 3);
    REQUIRE(parse_integer_param(nlohmann::json("3"), out)); // the stale-schema client
    CHECK(out == 3);
    REQUIRE(parse_integer_param(nlohmann::json(0), out));
    CHECK(out == 0);
    REQUIRE(parse_integer_param(nlohmann::json(-7.0), out));
    CHECK(out == -7);
}

// Accepting 3.0 must not slide into accepting 3.5. A fractional value is not a widened integer, it
// is a different value, and silently truncating it would pick a plate or a filament slot the
// caller never named.
TEST_CASE("parse_integer_param refuses a value that is not whole", "[orcamcp][params]")
{
    int out = -1;
    CHECK_FALSE(parse_integer_param(nlohmann::json(3.5), out));
    CHECK_FALSE(parse_integer_param(nlohmann::json("3.5"), out));
    CHECK_FALSE(parse_integer_param(nlohmann::json("3 "), out));  // trailing text is not consumed
    CHECK_FALSE(parse_integer_param(nlohmann::json("3abc"), out));
}

// A whole double far outside int's range would wrap on the cast, turning a nonsense argument into
// a plausible small number rather than an error.
TEST_CASE("parse_integer_param refuses a magnitude int cannot hold", "[orcamcp][params]")
{
    int out = -1;
    CHECK_FALSE(parse_integer_param(nlohmann::json(1e18), out));
    CHECK_FALSE(parse_integer_param(nlohmann::json(-1e18), out));
    CHECK_FALSE(parse_integer_param(nlohmann::json("99999999999999"), out));
}

TEST_CASE("parse_integer_param refuses what is not a number at all", "[orcamcp][params]")
{
    int out = -1;
    CHECK_FALSE(parse_integer_param(nlohmann::json("abc"), out));
    CHECK_FALSE(parse_integer_param(nlohmann::json(""), out));
    CHECK_FALSE(parse_integer_param(nlohmann::json(true), out));
    CHECK_FALSE(parse_integer_param(nlohmann::json(nullptr), out));
    CHECK_FALSE(parse_integer_param(nlohmann::json::array({1}), out));
    CHECK_FALSE(parse_integer_param(nlohmann::json::object(), out));
}
