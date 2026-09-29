// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.hpp
#pragma once
#include <array>
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

// A sliced plate's estimated print time, in seconds: the normal mode, as get_print_estimate's
// estimated_time_seconds and the G-code's "estimated printing time (normal mode)" give it. The
// silent mode is its own figure.
double normal_mode_print_time_s(const PrintEstimatedStatistics& statistics);

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

// The keys of get_print_estimate's time_by_feature: OrcaMCPExtrusionFeatures.hpp's features, with
// perimeters split into its walls; then travel; tool_changes, the time filament and tool changes cost,
// which the processor puts on their Tool_change moves; other (retracts, wipes, seams, pauses, custom
// G-code, custom extrusions); and unattributed, what the processor adds to its total without a move.
enum class TimeFeature : size_t
{
    outer_wall, inner_wall, overhang_wall, gap_fill, infill, support, support_interface, brim, skirt, prime_tower,
    travel, tool_changes, other, unattributed,
    count
};

const char* time_feature_key(TimeFeature feature);

// Where a plate's estimated print time goes, in seconds of one time mode, indexed by TimeFeature.
struct FeatureTimes
{
    std::array<double, size_t(TimeFeature::count)> seconds{};

    double at(TimeFeature feature) const { return seconds[size_t(feature)]; }
};

// time_by_feature for `moves`, whose values sum to `total_seconds`: unattributed is total_seconds less
// every move's time. The per-move times are the ones the preview legend sums by feature (libvgcode
// ViewerImpl::load), a dwell's included: the processor adds it to the move after it
// (GCodeProcessor::TimeMachine::calculate_time). Runs on the GUI thread over every move of a plate, so
// a move costs an array index, never a string.
FeatureTimes compute_time_by_feature(const std::vector<GCodeProcessorResult::MoveVertex>& moves,
                                     PrintEstimatedStatistics::ETimeMode       mode,
                                     double                                    total_seconds);

// time_by_feature as the response carries it: every key, each value in seconds, to a tenth.
nlohmann::json time_by_feature_json(const FeatureTimes& times);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
