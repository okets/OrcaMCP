// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.cpp
#include "OrcaMCPSliceEstimate.hpp"
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

}}} // namespace Slic3r::GUI::OrcaMCP
