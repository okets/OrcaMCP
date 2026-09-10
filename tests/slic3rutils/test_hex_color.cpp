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

// Whether a colour "moved" is not a string comparison. apply_config gives every slot whose colour
// changed the colour picker's three-key treatment, and that replaces a gradient with one flat
// colour -- so re-submitting a slot's own colour in a different case used to cost that slot its
// second colour.

using Slic3r::GUI::OrcaMCP::color_changed;

TEST_CASE("the same hex colour in a different case is not a change", "[orcamcp][color]")
{
    CHECK_FALSE(color_changed("#ff0000", "#FF0000"));
    CHECK_FALSE(color_changed("#B17C38", "#b17c38"));
    CHECK_FALSE(color_changed("#B17C38FF", "#b17c38ff")); // with an alpha byte
    CHECK_FALSE(color_changed("#B17C38", "#B17C38"));
    CHECK_FALSE(color_changed("", ""));
}

TEST_CASE("a different colour is a change", "[orcamcp][color]")
{
    CHECK(color_changed("#FF0000", "#FF0001"));
    CHECK(color_changed("", "#FF0000"));            // a slot that had no colour
    CHECK(color_changed("#FF0000", ""));            // a colour being cleared
    CHECK(color_changed("#B17C38", "#B17C38FF"));   // same RGB, but now carrying an alpha byte
}

TEST_CASE("a value that is not a hex colour is compared exactly", "[orcamcp][color]")
{
    // Nothing here can say what "Bronze" means, so it is text: two spellings of it are two values.
    CHECK(color_changed("Bronze", "bronze"));
    CHECK_FALSE(color_changed("Bronze", "Bronze"));
    CHECK(color_changed("#GGGGGG", "#gggggg")); // right shape, not hex - never treated as a colour
}
