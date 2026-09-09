// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.cpp
#include "OrcaMCPSliceEstimate.hpp"

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

}}} // namespace Slic3r::GUI::OrcaMCP
