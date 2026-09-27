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

int layer_range_filament(int range_extruder, int object_extruder)
{
    return range_extruder > 0 ? range_extruder : std::max(1, object_extruder);
}

LayerHeightLimits layer_height_limits(const DynamicPrintConfig& printer, int filament)
{
    const int index = std::max(0, filament - 1);
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

std::optional<LayerRangeRejection> layer_range_rejection(double layer_height, const LayerHeightLimits& limits,
                                                         bool wrote_layer_height, bool wrote_extruder)
{
    const std::optional<std::string> error = layer_range_height_error(layer_height, limits);
    if (!error)
        return std::nullopt;
    if (wrote_layer_height)
        return LayerRangeRejection{"layer_height", *error};
    if (wrote_extruder) {
        std::ostringstream reason;
        reason << std::setprecision(6) << "the range's layer height of " << layer_height << " mm is outside what this extruder's "
               << "nozzle prints (min_layer_height " << limits.min << " mm to max_layer_height " << limits.max
               << " mm): give the range a layer_height it can print too";
        return LayerRangeRejection{"extruder", reason.str()};
    }
    return std::nullopt;
}

LayerRangeWriteStatus layer_range_write_status(size_t given, size_t applied)
{
    if (given == 0)
        return {"error", "no settings were given, so nothing was set and the object's ranges are unchanged: pass at least "
                         "one {key, value} (a layer_height alone adds a range at that height)"};
    if (applied == given)
        return {"success", {}};
    return {applied == 0 ? "error" : "partial", {}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
