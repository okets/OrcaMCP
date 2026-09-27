// src/slic3r/GUI/OrcaMCP/OrcaMCPLayerRanges.hpp
#pragma once
#include <optional>
#include <string>

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI { namespace OrcaMCP {

// The filament that prints a range, 1-based: the range's own "extruder", else the object's, else the
// first. 0 in either means "the one above": the object's for a range, the first for an object.
int layer_range_filament(int range_extruder, int object_extruder);

// The layer heights the printer can print with the nozzle `filament` (1-based) prints from, as the
// object list's range editor bounds them (GUI_ObjectList.cpp, get_min_layer_height and
// get_max_layer_height): min_layer_height and max_layer_height, a zero maximum meaning three quarters
// of the nozzle. The per-extruder values are indexed by filament, as the slicer indexes them
// (Slicing.cpp, min_layer_height_from_nozzle): one per tool on a toolchanger, the first for a
// filament beyond them.
struct LayerHeightLimits
{
    double min = 0.;
    double max = 0.;
};
LayerHeightLimits layer_height_limits(const DynamicPrintConfig& printer, int filament);

// Why a range cannot print at `layer_height`, or nullopt when it can: it must be above 0 and within
// the limits.
std::optional<std::string> layer_range_height_error(double layer_height, const LayerHeightLimits& limits);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
