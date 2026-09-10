// src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.cpp
#include "OrcaMCPColorRecipe.hpp"

#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {
constexpr int kHueSectorDegrees = 30;
constexpr int kHueSectorCount   = 360 / kHueSectorDegrees;
} // namespace

ColorMixRecipe color_mix_recipe_from_result(const ColorDecomposeRecipeResult& result)
{
    ColorMixRecipe recipe;
    if (!result.valid || result.components.empty())
        return recipe;

    recipe.valid = true;
    recipe.exact_match = result.components.size() == 1;
    for (const auto& component : result.components) {
        recipe.components.push_back(component.filament_index);
        recipe.hexes.push_back(component.color_hex);
        // One component is all of it, whatever ratio the recipe table happened to carry.
        recipe.ratios.push_back(recipe.exact_match ? 100 : component.ratio);
    }
    return recipe;
}

const char* gamut_label(double delta_e)
{
    return delta_e < kGamutDeltaEThreshold ? "inside" : "outside";
}

std::vector<int> unreachable_hue_sectors(const std::vector<double>& hues)
{
    bool reached[kHueSectorCount] = {false};
    for (double hue : hues) {
        double wrapped = std::fmod(hue, 360.0);
        if (wrapped < 0.0)
            wrapped += 360.0;
        reached[int(wrapped) / kHueSectorDegrees] = true;
    }

    std::vector<int> unreachable;
    for (int sector = 0; sector < kHueSectorCount; ++sector)
        if (!reached[sector])
            unreachable.push_back(sector * kHueSectorDegrees);
    return unreachable;
}

const char* hue_sector_name(int sector_degrees)
{
    switch (((sector_degrees % 360) + 360) % 360) {
    case 0:   return "red";
    case 30:  return "orange";
    case 60:  return "yellow";
    case 90:  return "yellow-green";
    case 120: return "green";
    case 150: return "spring green";
    case 180: return "cyan";
    case 210: return "azure";
    case 240: return "blue";
    case 270: return "violet";
    case 300: return "magenta";
    case 330: return "rose";
    default:  return "off-sector";
    }
}

}}} // namespace Slic3r::GUI::OrcaMCP
