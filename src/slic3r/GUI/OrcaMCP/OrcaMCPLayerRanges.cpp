// src/slic3r/GUI/OrcaMCP/OrcaMCPLayerRanges.cpp
#include "OrcaMCPLayerRanges.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// One extruder's value of a per-extruder printer key; 0 when the printer has none.
double extruder_value(const DynamicPrintConfig& printer, const char* key, int index)
{
    const auto* values = printer.option<ConfigOptionFloats>(key);
    return values != nullptr && !values->values.empty() ? values->get_at(size_t(index)) : 0.;
}

} // namespace

LayerHeightLimits layer_height_limits(const DynamicPrintConfig& printer, int extruder)
{
    const int index = std::max(0, extruder - 1);
    LayerHeightLimits limits;
    limits.min = extruder_value(printer, "min_layer_height", index);
    limits.max = extruder_value(printer, "max_layer_height", index);
    if (limits.max < EPSILON)
        limits.max = 0.75 * extruder_value(printer, "nozzle_diameter", index);
    return limits;
}

std::optional<std::string> layer_range_height_error(double layer_height, const LayerHeightLimits& limits)
{
    if (layer_height > 0. && layer_height >= limits.min - EPSILON && layer_height <= limits.max + EPSILON)
        return std::nullopt;
    std::ostringstream reason;
    reason << std::setprecision(6) << "a layer height of " << layer_height << " mm is outside what this printer's extruder "
           << "prints: above 0, from min_layer_height " << limits.min << " mm to max_layer_height " << limits.max << " mm";
    return reason.str();
}

}}} // namespace Slic3r::GUI::OrcaMCP
