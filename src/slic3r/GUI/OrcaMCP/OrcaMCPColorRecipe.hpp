// src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp
#pragma once
#include <string>
#include <vector>

#include "libslic3r/ColorDecomposeRecipe.hpp"

// The colour decisions behind suggest_color_mix and get_color_palette that need no printer, no
// preset bundle and no wx -- deliberately, so Catch2 can drive them. Everything here is pure.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A recipe as the MCP response describes it: which slots, in what percentages.
struct ColorMixRecipe
{
    std::vector<unsigned int> components;         // 1-based filament slots
    std::vector<int>          ratios;             // percent, sums to 100
    std::vector<std::string>  hexes;              // each component's own colour
    bool                      exact_match = false;
    bool                      valid = false;
};

// What recommend_from_physical_filaments returned, read as a recipe. A one-component result is not
// a failure: it means the target is already loaded, so it comes back as that slot at 100% with
// exact_match set. Reporting it as an error is what stopped an agent walking a set of targets.
ColorMixRecipe color_mix_recipe_from_result(const ColorDecomposeRecipeResult& result);

// A mixed slot alternates layers of its components, so the result is close to a weighted average
// of the component RGB values -- not subtractive pigment mixing. Cyan, magenta and yellow
// therefore cannot make red or blue, and the recipe engine will still hand back its closest
// attempt. This threshold is where "closest attempt" stops being an answer: CIE76 delta E 20.
// The session that prompted it saw usable mixes at 3.6-14 and pure red at 54.
constexpr double kGamutDeltaEThreshold = 20.0;

// "inside" below the threshold, "outside" at or above it.
const char* gamut_label(double delta_e);

// Which 30-degree hue sectors (0, 30, ... 330; 0 is red) no colour in `hues` falls into. A palette
// that reaches no colour in a sector cannot mix one: with cyan, magenta and yellow loaded, the red
// and orange sectors come back empty, which is what "out of gamut" means for that machine.
// `hues` are degrees in [0, 360); anything outside is wrapped.
std::vector<int> unreachable_hue_sectors(const std::vector<double>& hues);

// A name for one of the sector boundaries unreachable_hue_sectors returns (0, 30, ... 330).
// "the red and orange sectors are unreachable" is actionable; "sectors 0 and 30" is not.
const char* hue_sector_name(int sector_degrees);

}}} // namespace Slic3r::GUI::OrcaMCP
