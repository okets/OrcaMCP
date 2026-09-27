// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.hpp
#pragma once
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"

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

// get_print_estimate's layer fields: printed_layers, object_layers and support_layers, null when the
// plate has no sliced objects to count (`counts` unset), and layer_count, the deprecated name, which
// has always been an integer and stays one: the printed count, or 0.
nlohmann::json layer_counts_json(const std::optional<LayerCounts>& counts);

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

// get_print_estimate's time_by_feature: where a plate's estimated print time goes, in seconds of one
// time mode. The features are OrcaMCPExtrusionFeatures.hpp's, perimeters split into outer_wall,
// inner_wall, overhang_wall and gap_fill; then travel; tool_changes, the time filament and tool
// changes cost, which the processor puts on their Tool_change moves; other (retracts, wipes, seams,
// pauses, custom G-code, custom extrusions); and unattributed, `total_seconds` less every move's
// time -- what the processor adds to its total without a move. So the values sum to total_seconds.
// Every key is present, 0 when a plate has none of it, so two slices compare key by key. The
// per-move times are the ones the preview legend sums by feature (libvgcode ViewerImpl::load), a
// dwell's included: the processor adds it to the move after it (GCodeProcessor::TimeMachine::calculate_time).
std::map<std::string, double> compute_time_by_feature(const std::vector<GCodeProcessorResult::MoveVertex>& moves,
                                                      PrintEstimatedStatistics::ETimeMode       mode,
                                                      double                                    total_seconds);

// time_by_feature as the response carries it: each value in seconds, to a tenth.
nlohmann::json time_by_feature_json(const std::map<std::string, double>& seconds_by_feature);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
