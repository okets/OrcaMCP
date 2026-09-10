// src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.cpp
#include "OrcaMCPColorRecipe.hpp"

#include <algorithm>
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

std::optional<double> chromatic_hue_degrees(const std::string& hex)
{
    ColorDecomposeRgb rgb;
    if (!color_decompose_hex_to_rgb(hex, rgb))
        return std::nullopt;
    const double r = rgb.r / 255.0, g = rgb.g / 255.0, b = rgb.b / 255.0;
    const double max_c = std::max({r, g, b}), min_c = std::min({r, g, b});
    const double delta = max_c - min_c;
    if (delta < 1e-9)
        return std::nullopt; // gray/black/white: no hue to report

    double h;
    if (max_c == r)      h = std::fmod((g - b) / delta, 6.0);
    else if (max_c == g) h = (b - r) / delta + 2.0;
    else                 h = (r - g) / delta + 4.0;
    h *= 60.0;
    return h < 0.0 ? h + 360.0 : h;
}

std::vector<double> reachable_hues(const std::vector<std::string>& physical_hexes,
                                   const std::vector<std::string>& candidate_hexes)
{
    std::vector<double> hues;
    hues.reserve(physical_hexes.size() + candidate_hexes.size());
    for (const std::vector<std::string>* list : {&physical_hexes, &candidate_hexes})
        for (const std::string& hex : *list)
            if (const auto hue = chromatic_hue_degrees(hex))
                hues.push_back(*hue);
    return hues;
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
