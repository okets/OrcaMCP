// src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp
#pragma once
#include <optional>
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
//
// Callers must exclude achromatic colours from `hues` -- see chromatic_hue_degrees below. A gray
// entry has no hue, and treating it as hue 0 would falsely mark the red sector "reached".
std::vector<int> unreachable_hue_sectors(const std::vector<double>& hues);

// Hue in degrees [0, 360) for a "#RRGGBB" colour, or no value if the string is unparsable or the
// colour is achromatic (gray, black or white: max and min channel are equal). Achromatic colours
// have no hue at all, so they must not be reported as "reaching" any sector -- a gray mixed
// candidate must not hide a genuinely unreachable red from unreachable_hue_sectors. This is
// deliberately a separate function from any hue-for-sort-order helper elsewhere, which may still
// give gray a stable fallback hue for display ordering; that fallback is wrong for gamut decisions.
std::optional<double> chromatic_hue_degrees(const std::string& hex);

// Every hue this filament set can actually put on the plate, ready for unreachable_hue_sectors.
// Two things belong in it, and leaving either out names a hue the set can reach:
//
//  - the loaded filaments' own hues (`physical_hexes`), because a colour already in the machine is
//    printable directly -- and the mix enumerator drops any candidate within delta E 5 of a loaded
//    filament as redundant, so a red spool's hue appears in NO candidate at all;
//  - every enumerated mix (`candidate_hexes`), which must be the whole enumeration, not the page
//    of it a response happens to carry: a per-response count cap is presentation, not gamut.
//
// Achromatic entries (gray, black, white) and unparsable strings contribute nothing, per
// chromatic_hue_degrees.
std::vector<double> reachable_hues(const std::vector<std::string>& physical_hexes,
                                   const std::vector<std::string>& candidate_hexes);

// A name for one of the sector boundaries unreachable_hue_sectors returns (0, 30, ... 330).
// "the red and orange sectors are unreachable" is actionable; "sectors 0 and 30" is not.
const char* hue_sector_name(int sector_degrees);

}}} // namespace Slic3r::GUI::OrcaMCP
