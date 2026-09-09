#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"

// The colour guard every MCP entry point puts in front of upstream's parsers, which are lenient in
// ways that turn a typo into a wrong colour instead of an error: can_decode_color only checks the
// length and the '#', and color_decompose_hex_to_rgb ignores anything after the sixth digit.

using Slic3r::GUI::OrcaMCP::is_hex_color;

TEST_CASE("is_hex_color accepts exactly #RRGGBB", "[orcamcp][color]")
{
    CHECK(is_hex_color("#B17C38"));
    CHECK(is_hex_color("#000000"));
    CHECK(is_hex_color("#ffffff"));
    CHECK(is_hex_color("#AbCdEf"));
}

TEST_CASE("is_hex_color rejects what upstream would silently accept", "[orcamcp][color]")
{
    CHECK_FALSE(is_hex_color("#B17C38ff"));         // an alpha byte, unless the caller allows it
    CHECK_FALSE(is_hex_color("#B17C38 (bronze)"));  // trailing garbage
    CHECK_FALSE(is_hex_color("#GGGGGG"));           // right length, not hex - decodes as black
    CHECK_FALSE(is_hex_color("B17C38"));            // no '#'
    CHECK_FALSE(is_hex_color("#B17C3"));            // too short
    CHECK_FALSE(is_hex_color("#B17C388"));          // eight digits is neither RGB nor RGBA
    CHECK_FALSE(is_hex_color(""));
    CHECK_FALSE(is_hex_color(" #B17C38"));          // untrimmed
}

TEST_CASE("is_hex_color takes an alpha byte only when asked", "[orcamcp][color]")
{
    CHECK(is_hex_color("#B17C38FF", /*allow_alpha=*/true));
    CHECK(is_hex_color("#B17C38", /*allow_alpha=*/true));
    CHECK_FALSE(is_hex_color("#B17C38FFF", /*allow_alpha=*/true));
    CHECK_FALSE(is_hex_color("#B17C38Fz", /*allow_alpha=*/true));
}
