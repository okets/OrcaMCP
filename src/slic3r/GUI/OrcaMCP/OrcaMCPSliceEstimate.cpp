// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.cpp
#include "OrcaMCPSliceEstimate.hpp"
#include "OrcaMCPExtrusionFeatures.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// A vector of per-filament properties is only as long as the printer had filaments configured when
// the result was produced; anything beyond it (or a nonsensical value) is "not known".
std::optional<double> property_at(const std::vector<float>& values, size_t index, bool must_be_positive)
{
    if (index >= values.size())
        return std::nullopt;
    const double value = static_cast<double>(values[index]);
    if (!std::isfinite(value) || (must_be_positive && value <= 0.0))
        return std::nullopt;
    return value;
}

} // namespace

double normal_mode_print_time_s(const PrintEstimatedStatistics& statistics)
{
    return statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
}

SliceEstimate compute_slice_estimate(const std::map<size_t, double>& volumes,
                                     const std::vector<float>&       diameters,
                                     const std::vector<float>&       densities,
                                     const std::vector<float>&       costs)
{
    SliceEstimate estimate;

    double length_total = 0.0, weight_total = 0.0, cost_total = 0.0;
    bool   length_known = true, weight_known = true, cost_known = true;

    for (const auto& [filament_id, volume_mm3] : volumes) {
        FilamentUsage usage;
        usage.filament_id = filament_id;
        usage.volume_mm3  = volume_mm3;
        estimate.volume_mm3 += volume_mm3;

        // length = volume / cross-section of the filament
        if (const std::optional<double> diameter = property_at(diameters, filament_id, true)) {
            const double section = M_PI * (*diameter * 0.5) * (*diameter * 0.5);
            usage.length_mm     = volume_mm3 / section;
            length_total += *usage.length_mm;
        } else {
            length_known = false;
        }

        // density is g/cm^3 and the volume mm^3, hence the 0.001
        const std::optional<double> density = property_at(densities, filament_id, true);
        if (density) {
            usage.weight_g = volume_mm3 * *density * 0.001;
            weight_total += *usage.weight_g;
        } else {
            weight_known = false;
        }

        // cost is per kg, and the weight in grams
        const std::optional<double> cost_per_kg = property_at(costs, filament_id, false);
        if (density && cost_per_kg) {
            usage.cost = *usage.weight_g * *cost_per_kg * 0.001;
            cost_total += *usage.cost;
        } else {
            cost_known = false;
        }

        estimate.per_filament.push_back(usage);
    }

    if (length_known)
        estimate.length_mm = length_total;
    if (weight_known)
        estimate.weight_g = weight_total;
    if (cost_known)
        estimate.cost = cost_total;

    return estimate;
}

size_t count_distinct_heights(std::vector<double> zs)
{
    // GCode::_do_export's "merge numerically very close Z values", unchanged.
    if (zs.empty())
        return 0;
    std::sort(zs.begin(), zs.end());
    const auto end   = std::unique(zs.begin(), zs.end());
    size_t     count = size_t(end - zs.begin());
    for (auto it = zs.begin(); it + 1 != end; ++it)
        if (std::abs(*it - *(it + 1)) < EPSILON)
            --count;
    return count;
}

namespace {

std::vector<double> print_heights(const PrintObject& object, bool object_layers, bool support_layers)
{
    std::vector<double> zs;
    if (object_layers)
        for (const Layer* layer : object.layers())
            zs.push_back(layer->print_z);
    if (support_layers)
        for (const SupportLayer* layer : object.support_layers())
            zs.push_back(layer->print_z);
    return zs;
}

size_t count_layers(const Print& print, bool object_layers, bool support_layers)
{
    if (print.config().print_sequence == PrintSequence::ByObject) {
        size_t total = 0;
        for (const PrintObject* object : print.objects())
            total += object->instances().size() *
                     count_distinct_heights(print_heights(*object, object_layers, support_layers));
        return total;
    }
    std::vector<double> zs;
    for (const PrintObject* object : print.objects()) {
        const std::vector<double> heights = print_heights(*object, object_layers, support_layers);
        zs.insert(zs.end(), heights.begin(), heights.end());
    }
    return count_distinct_heights(std::move(zs));
}

} // namespace

LayerCounts count_print_layers(const Print& print)
{
    LayerCounts counts;
    counts.printed = count_layers(print, true, true);
    counts.object  = count_layers(print, true, false);
    counts.support = count_layers(print, false, true);
    return counts;
}

nlohmann::json layer_counts_json(const std::optional<LayerCounts>& counts)
{
    auto count_or_null = [&counts](size_t LayerCounts::*field) {
        return counts ? nlohmann::json((*counts).*field) : nlohmann::json(nullptr);
    };
    return {{"printed_layers", count_or_null(&LayerCounts::printed)},
            {"object_layers", count_or_null(&LayerCounts::object)},
            {"support_layers", count_or_null(&LayerCounts::support)},
            {"layer_count", counts ? counts->printed : size_t(0)}};
}

std::optional<size_t> count_profile_layers(const SlicingParameters& params, const std::vector<double>& profile, bool precise_z)
{
    if (profile.size() < 4)
        return std::nullopt;
    // generate_object_layers returns each layer as a bottom/top pair. Precise Z only realigns the
    // last layers, and needs some to realign: cut without it first, and only then with it.
    const std::vector<coordf_t> layers = generate_object_layers(params, profile, false);
    if (!precise_z || layers.empty())
        return layers.size() / 2;
    return generate_object_layers(params, profile, true).size() / 2;
}

namespace {

// A role's time feature: its wall when it is one, else its feature in the shared table.
TimeFeature time_feature_of(ExtrusionRole role)
{
    switch (wall_kind_of(role)) {
    case WallKind::outer_wall: return TimeFeature::outer_wall;
    case WallKind::inner_wall: return TimeFeature::inner_wall;
    case WallKind::overhang_wall: return TimeFeature::overhang_wall;
    case WallKind::gap_fill: return TimeFeature::gap_fill;
    case WallKind::none: break;
    }
    switch (extrusion_feature_of(role)) {
    case ExtrusionFeature::infill: return TimeFeature::infill;
    case ExtrusionFeature::support: return TimeFeature::support;
    case ExtrusionFeature::support_interface: return TimeFeature::support_interface;
    case ExtrusionFeature::brim: return TimeFeature::brim;
    case ExtrusionFeature::skirt: return TimeFeature::skirt;
    case ExtrusionFeature::prime_tower: return TimeFeature::prime_tower;
    case ExtrusionFeature::perimeters: // every perimeters role is a wall, above
    case ExtrusionFeature::other: break;
    }
    return TimeFeature::other;
}

// time_feature_of for every role, worked out once.
const std::array<TimeFeature, size_t(erCount)>& role_time_features()
{
    static const std::array<TimeFeature, size_t(erCount)> table = [] {
        std::array<TimeFeature, size_t(erCount)> features{};
        for (size_t role = 0; role < features.size(); ++role)
            features[role] = time_feature_of(ExtrusionRole(role));
        return features;
    }();
    return table;
}

// The feature one move's time is counted under.
TimeFeature time_feature_of(const GCodeProcessorResult::MoveVertex& move)
{
    switch (move.type) {
    case EMoveType::Travel: return TimeFeature::travel;
    case EMoveType::Tool_change: return TimeFeature::tool_changes;
    case EMoveType::Extrude:
        return size_t(move.extrusion_role) < size_t(erCount) ? role_time_features()[size_t(move.extrusion_role)] : TimeFeature::other;
    default: return TimeFeature::other;
    }
}

} // namespace

const char* time_feature_key(TimeFeature feature)
{
    switch (feature) {
    case TimeFeature::outer_wall: return "outer_wall";
    case TimeFeature::inner_wall: return "inner_wall";
    case TimeFeature::overhang_wall: return "overhang_wall";
    case TimeFeature::gap_fill: return "gap_fill";
    case TimeFeature::infill: return extrusion_feature_key(ExtrusionFeature::infill);
    case TimeFeature::support: return extrusion_feature_key(ExtrusionFeature::support);
    case TimeFeature::support_interface: return extrusion_feature_key(ExtrusionFeature::support_interface);
    case TimeFeature::brim: return extrusion_feature_key(ExtrusionFeature::brim);
    case TimeFeature::skirt: return extrusion_feature_key(ExtrusionFeature::skirt);
    case TimeFeature::prime_tower: return extrusion_feature_key(ExtrusionFeature::prime_tower);
    case TimeFeature::travel: return "travel";
    case TimeFeature::tool_changes: return "tool_changes";
    case TimeFeature::other: return extrusion_feature_key(ExtrusionFeature::other);
    case TimeFeature::unattributed: return "unattributed";
    case TimeFeature::count: break;
    }
    return "other";
}

FeatureTimes compute_time_by_feature(const std::vector<GCodeProcessorResult::MoveVertex>& moves,
                                     PrintEstimatedStatistics::ETimeMode       mode,
                                     double                                    total_seconds)
{
    FeatureTimes times;
    double       attributed = 0.0;
    const size_t mode_index = static_cast<size_t>(mode);
    for (const GCodeProcessorResult::MoveVertex& move : moves) {
        const double time = move.time[mode_index];
        times.seconds[size_t(time_feature_of(move))] += time;
        attributed += time;
    }
    times.seconds[size_t(TimeFeature::unattributed)] = total_seconds - attributed;
    return times;
}

nlohmann::json time_by_feature_json(const FeatureTimes& times)
{
    nlohmann::json out = nlohmann::json::object();
    for (size_t feature = 0; feature < times.seconds.size(); ++feature)
        out[time_feature_key(TimeFeature(feature))] = std::round(times.seconds[feature] * 10.0) / 10.0 + 0.0; // + 0.0: -0 reads 0
    return out;
}

}}} // namespace Slic3r::GUI::OrcaMCP
