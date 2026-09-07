// src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.hpp
#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include "slic3r/GUI/MixedFilamentDialog.hpp"   // MixedFilamentResult
#include "libslic3r/ColorDecomposeRecipe.hpp"   // ColorDecomposePhysicalFilament

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Snapshot of every filament slot (physical first, then mixed). Main thread only.
nlohmann::json describe_filaments();

// Builds a MixedFilamentResult from tool params; returns false and fills `error` on bad input.
// Only shape/parse checks (array sizes, numeric types, ratio sum) live here -- domain rules
// (component range, physical-only components, minimum printer filament count, ...) are left to
// Sidebar::apply_mixed_filament so they are validated in exactly one place.
bool mixed_result_from_params(const nlohmann::json& params, MixedFilamentResult& out, std::string& error);

// 1-based slot -> 0-based config index, validating range. Returns -1 and sets `error` when invalid.
int slot_to_config_index(int slot, std::string& error);

// Assigns a filament slot to an object or one of its volumes. Returns false and sets `error` on failure.
// Main thread only.
bool set_object_filament(int object_id, int volume_id /* -1 = object */, int slot, std::string& error);

// Snapshot of the per-extruder flush-volume matrices and the flush multiplier. Main thread only.
nlohmann::json describe_flush_volumes();

// Overwrites the full NxN flush-volume matrix for one extruder (0-based, default 0) and,
// if `flush_multiplier` is set, that extruder's flush multiplier. Both inputs are validated
// before anything is written (extruder out of range, matrix not NxN, multiplier extruder out
// of range), and RefreshAfterProjectConfigChange() is called exactly once at the end, after
// both writes. Returns false and fills `error` on bad input. Main thread only.
bool set_flush_volumes(const nlohmann::json& matrix, int extruder, std::optional<double> flush_multiplier, std::string& error);

// Recalculates every filament's flush-volume row/column for every extruder automatically
// (Sidebar::auto_calc_flushing_volumes with its "all" defaults; skips mixed/virtual slots
// itself). Main thread only.
void auto_calc_flush_volumes();

// Current values of every toolchanger_keys/project_keys config key (OrcaMCPConfigKeys.hpp),
// grouped by {"printer": {...}, "print": {...}, "project": {...}}, each serialized with
// opt_serialize, plus the printer's extruder_count. Main thread only.
nlohmann::json describe_toolchanger_config();

// Physical (non-mixed) filament slots as ColorDecomposeRecipe input: color, name, type,
// and 1-based slot index (matching the "slot" numbering used elsewhere, e.g. describe_filaments).
// Main thread only.
std::vector<ColorDecomposePhysicalFilament> physical_filaments_for_recipe();

// Predicted blend color for a set of component hex colors mixed at the given percent
// ratios (same order, sum == 100). Prefers a measured/interpolated color from the standard
// recipe table (lookup_measured_blend_color); falls back to the pigment-mixing model
// (FilamentMixer::blend_color_multi) when no measured entry exists -- the same call the
// sidebar itself uses to color a mixed slot (Plater.cpp's blend_mixed_color).
struct MixColorPrediction {
    std::string hex;
    bool        measured{false};
};
MixColorPrediction predicted_mix_color(const std::vector<std::string>& hexes, const std::vector<int>& ratios);

// CIE76 perceptual distance between two "#RRGGBB" colors. Wraps
// color_decompose_delta_e(rgb_to_lab(...)) so callers here never touch RGB structs or Lab
// math directly. Returns a very large value (colors incomparable) if either hex is invalid.
double color_delta_e_hex(const std::string& hex_a, const std::string& hex_b);

// Enumerates an achievable color palette: every same-type pair (and, when max_components == 3,
// triple) of physical filaments at fixed ratio presets (pairs: 70/30, 50/50, 30/70; triples:
// 50/25/25 with each component taking the dominant 50% role in turn -- the same grouping and
// triple-rotation rule as MixedFilamentDialog::rebuild_recommendation_items, minus its widgets).
// Candidates within delta_e < 5 of a physical filament or an already-accepted candidate are
// dropped, the rest are sorted by hue and truncated to max_count. Returns a JSON array of
// {"components": [1-based...], "ratios": [...], "predicted_color": "#RRGGBB", "measured": bool}.
// Main thread only.
nlohmann::json enumerate_mix_palette(int max_count, int max_components, const std::string& material_type);

}}} // namespace Slic3r::GUI::OrcaMCP
