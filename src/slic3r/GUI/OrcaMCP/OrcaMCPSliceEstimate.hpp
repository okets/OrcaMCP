// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.hpp
#pragma once
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

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

}}} // namespace Slic3r::GUI::OrcaMCP
