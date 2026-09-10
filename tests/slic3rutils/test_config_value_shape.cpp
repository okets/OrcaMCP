#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp"

// apply_config and set_object_config both take a JSON value and hand text to
// ConfigOption::deserialize. get_valid_config_keys advertises list-typed keys as "strings" /
// "ints" / "bools" / "floats", so an array is the obvious thing for a caller to send -- and the
// separator deserialize wants is NOT the same for all of them: ConfigOptionStrings splits on ';'
// (Config.cpp:149) while ConfigOptionInts, ConfigOptionFloats and ConfigOptionBools split on ','
// (Config.hpp:1089 / 911 / 1959). This is the one place that knows which.

using Slic3r::GUI::config_value_expected_shape;
using Slic3r::GUI::config_value_to_string;
using Slic3r::coBool;
using Slic3r::coBools;
using Slic3r::coFloat;
using Slic3r::coFloats;
using Slic3r::coInts;
using Slic3r::coString;
using Slic3r::coStrings;

TEST_CASE("a string is passed through untouched", "[orcamcp][config]")
{
    // The joined form keeps working: it is what the session had to fall back to.
    const auto joined = config_value_to_string(nlohmann::json("#00FFFF;#FF00FF"), coStrings);
    CHECK(joined.ok);
    CHECK(joined.text == "#00FFFF;#FF00FF");

    const auto scalar = config_value_to_string(nlohmann::json("0.2"), coFloat);
    CHECK(scalar.ok);
    CHECK(scalar.text == "0.2");
}

TEST_CASE("an array of strings is joined the way ConfigOptionStrings reads it", "[orcamcp][config]")
{
    // The T1 reproduction, verbatim.
    const nlohmann::json value = {"#00FFFF", "#FF00FF", "#FFFF00", "#808080"};
    const auto shaped = config_value_to_string(value, coStrings);

    CHECK(shaped.ok);
    CHECK(shaped.text == "#00FFFF;#FF00FF;#FFFF00;#808080");
}

TEST_CASE("a string element that contains the separator is quoted", "[orcamcp][config]")
{
    // ';' inside a value would otherwise split it into two. escape_strings_cstyle quotes only
    // the elements that need it, which is why the plain case above stays readable.
    const nlohmann::json value = {"plain", "has;semicolon"};
    const auto shaped = config_value_to_string(value, coStrings);

    CHECK(shaped.ok);
    CHECK(shaped.text == "plain;\"has;semicolon\"");
}

TEST_CASE("numeric and boolean arrays join with a comma, not a semicolon", "[orcamcp][config]")
{
    const auto ints = config_value_to_string(nlohmann::json({1, 2, 3}), coInts);
    CHECK(ints.ok);
    CHECK(ints.text == "1,2,3");

    const auto floats = config_value_to_string(nlohmann::json({0, 140, 140.5, 0}), coFloats);
    CHECK(floats.ok);
    CHECK(floats.text == "0,140,140.5,0");

    // ConfigOptionBools::deserialize only accepts "1" and "0" without a substitution.
    const auto bools = config_value_to_string(nlohmann::json({true, false, true}), coBools);
    CHECK(bools.ok);
    CHECK(bools.text == "1,0,1");
}

TEST_CASE("a scalar boolean becomes 1 or 0", "[orcamcp][config]")
{
    CHECK(config_value_to_string(nlohmann::json(true), coBool).text == "1");
    CHECK(config_value_to_string(nlohmann::json(false), coBool).text == "0");
}

TEST_CASE("a shape that cannot work is refused with a reason", "[orcamcp][config]")
{
    const auto array_for_scalar = config_value_to_string(nlohmann::json({1, 2}), coFloat);
    CHECK_FALSE(array_for_scalar.ok);
    CHECK(array_for_scalar.reason.find("not a list") != std::string::npos);

    const auto nested = config_value_to_string(nlohmann::json({{1, 2}, {3, 4}}), coFloats);
    CHECK_FALSE(nested.ok);

    const auto object = config_value_to_string(nlohmann::json::object({{"r", 1}}), coString);
    CHECK_FALSE(object.ok);

    const auto null_value = config_value_to_string(nlohmann::json(nullptr), coString);
    CHECK_FALSE(null_value.ok);

    // "" means "one empty value" to ConfigOptionFloats (it pushes 0), never "no values", so an
    // empty array has no honest text form.
    const auto empty = config_value_to_string(nlohmann::json::array(), coFloats);
    CHECK_FALSE(empty.ok);
}

TEST_CASE("the expected shape names what the caller should have sent", "[orcamcp][config]")
{
    CHECK(config_value_expected_shape(coStrings) == "a string, or an array of strings");
    CHECK(config_value_expected_shape(coFloats) == "a number, or an array of numbers");
    CHECK(config_value_expected_shape(coInts) == "a whole number, or an array of whole numbers");
    CHECK(config_value_expected_shape(coBools) == "true or false, or an array of them");
    CHECK(config_value_expected_shape(coFloat) == "a number");
    CHECK(config_value_expected_shape(coString) == "a string");
}

TEST_CASE("the rejection message tells a caller both halves", "[orcamcp][config]")
{
    // apply_config pairs the specific complaint with the shape that would have worked; a caller
    // that only sees "invalid_keys" cannot tell a typo'd key from a wrong-shaped value.
    const auto shaped = config_value_to_string(nlohmann::json({1, 2}), Slic3r::coFloat);
    REQUIRE_FALSE(shaped.ok);
    CHECK(shaped.reason == "an array was given for a key that is not a list");
    CHECK(config_value_expected_shape(Slic3r::coFloat) == "a number");
}
