#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp"

// The decisions behind suggest_color_mix and get_color_palette that do not need a printer: what a
// recommend_from_physical_filaments result means, how far is too far, and which colours the loaded
// filaments cannot reach at all.

using Slic3r::ColorDecomposeRecipeComponent;
using Slic3r::ColorDecomposeRecipeResult;
using Slic3r::GUI::OrcaMCP::color_mix_recipe_from_result;
using Slic3r::GUI::OrcaMCP::gamut_label;
using Slic3r::GUI::OrcaMCP::kGamutDeltaEThreshold;
using Slic3r::GUI::OrcaMCP::unreachable_hue_sectors;

namespace {

ColorDecomposeRecipeComponent component(unsigned int slot, const char* hex, int ratio)
{
    ColorDecomposeRecipeComponent c;
    c.filament_index = slot;
    c.color_hex = hex;
    c.ratio = ratio;
    return c;
}

} // namespace

TEST_CASE("a two-component result is an ordinary mix", "[orcamcp][colorrecipe]")
{
    ColorDecomposeRecipeResult result;
    result.valid = true;
    result.components = {component(2, "#FF00FF", 40), component(3, "#FFFF00", 60)};

    const auto recipe = color_mix_recipe_from_result(result);

    CHECK(recipe.valid);
    CHECK_FALSE(recipe.exact_match);
    CHECK(recipe.components == std::vector<unsigned int>{2, 3});
    CHECK(recipe.ratios == std::vector<int>{40, 60});
    CHECK(recipe.hexes == std::vector<std::string>{"#FF00FF", "#FFFF00"});
}

TEST_CASE("a one-component result is an exact match, not a failure", "[orcamcp][colorrecipe]")
{
    // T4: "target_color already matches physical filament 1" is the useful answer "load slot 1",
    // and it was being returned as status: error, which stops an agent walking a palette.
    ColorDecomposeRecipeResult result;
    result.valid = true;
    result.components = {component(1, "#00FFFF", 100)};

    const auto recipe = color_mix_recipe_from_result(result);

    CHECK(recipe.valid);
    CHECK(recipe.exact_match);
    CHECK(recipe.components == std::vector<unsigned int>{1});
    // The ratio is forced to 100 whatever the recipe table said: one component is all of it.
    CHECK(recipe.ratios == std::vector<int>{100});
    CHECK(recipe.hexes == std::vector<std::string>{"#00FFFF"});
}

TEST_CASE("an invalid or empty result stays invalid", "[orcamcp][colorrecipe]")
{
    ColorDecomposeRecipeResult not_valid;
    CHECK_FALSE(color_mix_recipe_from_result(not_valid).valid);

    ColorDecomposeRecipeResult valid_but_empty;
    valid_but_empty.valid = true;
    CHECK_FALSE(color_mix_recipe_from_result(valid_but_empty).valid);
}

TEST_CASE("the gamut label has a documented threshold", "[orcamcp][colorrecipe]")
{
    CHECK(kGamutDeltaEThreshold == 20.0);
    // The session's own numbers: good mixes landed at 3.6-14, pure red at 54.
    CHECK(std::string(gamut_label(0.0)) == "inside");
    CHECK(std::string(gamut_label(3.6)) == "inside");
    CHECK(std::string(gamut_label(14.0)) == "inside");
    CHECK(std::string(gamut_label(19.9)) == "inside");
    CHECK(std::string(gamut_label(20.0)) == "outside");
    CHECK(std::string(gamut_label(54.0)) == "outside");
}

TEST_CASE("empty hue sectors name the colours a palette cannot reach", "[orcamcp][colorrecipe]")
{
    // Cyan (180), magenta (300) and yellow (60) alternate layers, so mixes land BETWEEN their
    // hues -- never at red (0) or blue (240). That is the whole finding, expressed as a sector.
    const std::vector<double> cmy_palette = {60, 90, 120, 150, 180, 210, 240 - 30, 300, 330};
    const auto unreachable = unreachable_hue_sectors(cmy_palette);

    CHECK(std::find(unreachable.begin(), unreachable.end(), 0) != unreachable.end());
    CHECK(std::find(unreachable.begin(), unreachable.end(), 30) != unreachable.end());
    CHECK(std::find(unreachable.begin(), unreachable.end(), 180) == unreachable.end());
}

TEST_CASE("a palette that covers the circle leaves nothing unreachable", "[orcamcp][colorrecipe]")
{
    std::vector<double> every_sector;
    for (int degrees = 0; degrees < 360; degrees += 30)
        every_sector.push_back(degrees + 5.0);

    CHECK(unreachable_hue_sectors(every_sector).empty());
}

TEST_CASE("an empty palette cannot reach anything", "[orcamcp][colorrecipe]")
{
    CHECK(unreachable_hue_sectors({}).size() == 12);
}

TEST_CASE("hues past 360 or below 0 wrap onto the same circle", "[orcamcp][colorrecipe]")
{
    // 370 is really 10 (sector 0, red); -10 is really 350 (sector 330).
    const auto unreachable = unreachable_hue_sectors({370.0, -10.0});

    CHECK(std::find(unreachable.begin(), unreachable.end(), 0) == unreachable.end());
    CHECK(std::find(unreachable.begin(), unreachable.end(), 330) == unreachable.end());
    CHECK(unreachable.size() == 10);
}
