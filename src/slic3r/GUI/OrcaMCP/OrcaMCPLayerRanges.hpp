// src/slic3r/GUI/OrcaMCP/OrcaMCPLayerRanges.hpp
#pragma once
#include <optional>
#include <string>

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI { namespace OrcaMCP {

// The layer heights the printer can print with one extruder (1-based; 0, "the object's", reads the
// first), as the object list's range editor bounds them (GUI_ObjectList.cpp, get_min_layer_height and
// get_max_layer_height): min_layer_height and max_layer_height, a zero maximum meaning three quarters
// of the nozzle.
struct LayerHeightLimits
{
    double min = 0.;
    double max = 0.;
};
LayerHeightLimits layer_height_limits(const DynamicPrintConfig& printer, int extruder);

// Why a range cannot print at `layer_height`, or nullopt when it can: it must be above 0 and within
// the limits.
std::optional<std::string> layer_range_height_error(double layer_height, const LayerHeightLimits& limits);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
