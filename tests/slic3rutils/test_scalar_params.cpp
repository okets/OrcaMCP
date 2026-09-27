#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "libslic3r/Model.hpp"

// The scalar guards every MCP tool reads its parameters through. They exist because a client whose
// cached tool schema predates a parameter sends it as a string, and because the standard library's
// own string-to-number conversions are lenient in ways that turn a caller's mistake into a
// different number rather than an error.

using Slic3r::GUI::OrcaMCP::parse_double_param;
using Slic3r::GUI::OrcaMCP::parse_integer_param;
using Slic3r::GUI::OrcaMCP::parse_object_param;
using Slic3r::GUI::OrcaMCP::parse_settings_param;
using Slic3r::GUI::OrcaMCP::resolve_object_id;
using Slic3r::GUI::OrcaMCP::error_response;

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

// The one place a tool turns object_id into an object, so every tool refuses a missing, mistyped or
// out-of-range id with the same three messages.
TEST_CASE("parse_object_param takes a JSON object or the text of one", "[orcamcp][params]")
{
    nlohmann::json out;
    REQUIRE(parse_object_param(nlohmann::json{{"layer", 5}}, out));
    CHECK(out == nlohmann::json{{"layer", 5}});
    // A client whose cached schema still says the parameter is a string sends the object as its text.
    REQUIRE(parse_object_param(nlohmann::json(R"({"z": 10.2, "features": ["support"]})"), out));
    CHECK(out == nlohmann::json{{"z", 10.2}, {"features", {"support"}}});
    REQUIRE(parse_object_param(nlohmann::json("  {\"layer\": 1}  "), out));
    CHECK(out == nlohmann::json{{"layer", 1}});
}

TEST_CASE("parse_object_param refuses what is not an object", "[orcamcp][params]")
{
    nlohmann::json out = "untouched";
    CHECK_FALSE(parse_object_param(nlohmann::json("first_layer"), out));
    CHECK_FALSE(parse_object_param(nlohmann::json("[1, 2]"), out));
    CHECK_FALSE(parse_object_param(nlohmann::json("{not json"), out));
    CHECK_FALSE(parse_object_param(nlohmann::json(5), out));
    CHECK_FALSE(parse_object_param(nlohmann::json::array({1}), out));
    CHECK(out == "untouched");
}

TEST_CASE("resolve_object_id finds the object an object_id names, or says why it cannot", "[orcamcp][params]")
{
    Slic3r::Model model;
    model.add_object()->name = "first";
    model.add_object()->name = "second";

    int         object_id = -1;
    std::string error;
    Slic3r::ModelObject* found = resolve_object_id(nlohmann::json{{"object_id", "1"}}, model, object_id, error);
    REQUIRE(found == model.objects[1]);
    CHECK(object_id == 1);
    CHECK(error.empty());

    CHECK(resolve_object_id(nlohmann::json::object(), model, object_id, error) == nullptr);
    CHECK(error == "object_id is required: the 0-based object_index get_scene_info reports");

    CHECK(resolve_object_id(nlohmann::json{{"object_id", 1.5}}, model, object_id, error) == nullptr);
    CHECK(error == "object_id must be a whole number");

    CHECK(resolve_object_id(nlohmann::json{{"object_id", 2}}, model, object_id, error) == nullptr);
    CHECK(error == "Invalid object_id 2: the scene has 2 objects");
    CHECK(resolve_object_id(nlohmann::json{{"object_id", -1}}, model, object_id, error) == nullptr);
    CHECK(error == "Invalid object_id -1: the scene has 2 objects");
}

TEST_CASE("error_response is the status and message every refusal carries", "[orcamcp][params]")
{
    CHECK(error_response("no") == nlohmann::json{{"status", "error"}, {"message", "no"}});
}

// set_object_config, set_object_layer_range and apply_config take `settings` as a list of
// {key, value} (apply_config's items also carry a type). A caller that sent an object instead --
// {"wall_loops": 3} -- got "Internal error: ... type_error.305" out of the JSON library rather than
// a word about what the tool takes.
TEST_CASE("parse_settings_param takes a list of {key, value}, or its JSON text", "[orcamcp][params]")
{
    nlohmann::json out;
    std::string    error;
    const nlohmann::json list = nlohmann::json::array({{{"key", "wall_loops"}, {"value", 3}}});
    REQUIRE(parse_settings_param(list, out, error));
    CHECK(out == list);
    REQUIRE(parse_settings_param(nlohmann::json(list.dump()), out, error));
    CHECK(out == list);
    REQUIRE(parse_settings_param(nlohmann::json::array(), out, error));
    CHECK(out.empty());
}

TEST_CASE("parse_settings_param says what a malformed settings list is", "[orcamcp][params]")
{
    nlohmann::json out = "untouched";
    std::string    error;
    CHECK_FALSE(parse_settings_param({{"wall_loops", 3}}, out, error));
    CHECK(error.find("list of {key, value}") != std::string::npos);
    CHECK(out == "untouched");

    CHECK_FALSE(parse_settings_param(nlohmann::json::array({{{"key", "wall_loops"}}}), out, error));
    CHECK(error.find("settings[0]") != std::string::npos);
    CHECK(error.find("value") != std::string::npos);

    CHECK_FALSE(parse_settings_param(nlohmann::json::array({{{"key", 7}, {"value", 3}}}), out, error));
    CHECK(error.find("key") != std::string::npos);

    CHECK_FALSE(parse_settings_param(nlohmann::json::array({"wall_loops"}), out, error));
    CHECK_FALSE(parse_settings_param(nlohmann::json("not json"), out, error));
}

TEST_CASE("parse_settings_param can require each setting's type too", "[orcamcp][params]")
{
    nlohmann::json out;
    std::string    error;
    CHECK_FALSE(parse_settings_param(nlohmann::json::array({{{"key", "wall_loops"}, {"value", 3}}}), out, error, /*with_type=*/true));
    CHECK(error.find("type") != std::string::npos);
    CHECK(parse_settings_param(nlohmann::json::array({{{"type", "print"}, {"key", "wall_loops"}, {"value", 3}}}), out, error, true));
}
