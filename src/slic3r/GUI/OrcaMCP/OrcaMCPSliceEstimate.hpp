// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.hpp
#pragma once
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class Print;
struct SlicingParameters;
namespace GUI { namespace OrcaMCP {

// What one filament (one extruder index of the sliced result) is going to consume.
// The optional fields stay empty when the slicer did not record the property this filament
// needed for them -- a missing density means the weight is unknown, not zero.
struct FilamentUsage
{
    size_t                filament_id = 0; // 0-based, as the G-code result indexes extruders
    double                volume_mm3  = 0.0;
    std::optional<double> length_mm;       // needs the filament diameter
    std::optional<double> weight_g;        // needs the filament density
    std::optional<double> cost;            // needs density and cost per kg
};

// The per-plate totals. A total is present only when every used filament contributed to it, so a
// partially-known result reports the parts it knows per filament and nothing misleading overall.
struct SliceEstimate
{
    double                     volume_mm3 = 0.0;
    std::optional<double>      length_mm;
    std::optional<double>      weight_g;
    std::optional<double>      cost;
    std::vector<FilamentUsage> per_filament;
};

// `volumes` maps a 0-based extruder index to the volume it extrudes, in mm^3
// (GCodeProcessorResult::print_statistics::total_volumes_per_extruder).
// `diameters` is in mm, `densities` in g/cm^3 and `costs` in currency per kg, all indexed by the
// same extruder index (GCodeProcessorResult::filament_diameters / _densities / _costs).
// Mirrors DoExport::update_print_estimated_stats in libslic3r/GCode.cpp, which is what the
// G-code's own "total filament used" comments are computed from.
SliceEstimate compute_slice_estimate(const std::map<size_t, double>& volumes,
                                     const std::vector<float>&       diameters,
                                     const std::vector<float>&       densities,
                                     const std::vector<float>&       costs);

// How many layers a sliced plate prints. `printed` is the number the G-code states as its total
// layer count ("; total layers count", the total_layer_count placeholder): distinct print heights,
// object and support layers together. `object` and `support` count the same way over one kind of
// layer each, so with synchronised support `printed` equals `object`, not their sum.
struct LayerCounts
{
    size_t printed = 0;
    size_t object  = 0;
    size_t support = 0;
};

// The number of distinct heights in `zs`, counting neighbours closer than EPSILON once: the merge
// GCode::_do_export applies before it counts layers.
size_t count_distinct_heights(std::vector<double> zs);

// The layer counts of a sliced `print`, by GCode::_do_export's rule: one set of heights for the
// whole plate, or -- printing by object -- each object's heights counted once per instance and
// summed, since every copy is printed from the bed up again.
LayerCounts count_print_layers(const Print& print);

// The object layers a variable-layer-height `profile` (z, height pairs) is cut into, by the same
// generate_object_layers call PrintObject::slice makes. Unset when the profile has fewer than two
// points: generate_object_layers asserts against an empty one, and with `precise_z` it reads the
// last of the layers it cut, of which there may be none.
std::optional<size_t> count_profile_layers(const SlicingParameters& params, const std::vector<double>& profile, bool precise_z);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
