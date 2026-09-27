// src/slic3r/GUI/OrcaMCP/OrcaMCPLayerRanges.hpp
#pragma once
#include <optional>
#include <string>

namespace Slic3r {
class DynamicPrintConfig;
class ModelConfig;
class ModelObject;
namespace GUI { namespace OrcaMCP {

// A layer range as set_object_layer_range leaves it. The GUI's object list gives every range a
// layer_height and an extruder when it creates one, and the slicer reads a range's layer_height
// unconditionally: a range the tool wrote with only other settings had none, and the next slice
// crashed the app. `defaults` is default_layer_config's; a key the range already has is kept.
void complete_layer_range(ModelConfig& range, const DynamicPrintConfig& defaults);

// What a new range starts with, mirroring ObjectList::get_default_layer_config: the object's own
// layer height, else the process preset's (`print_preset`), and extruder 0, the object's. Mirrored,
// not called: that function also reads the preset's "extruder" as a float, which is null for an
// object whose config has no extruder (after reset_object_config), and crashed the app.
DynamicPrintConfig default_layer_config(const ModelObject& object, const DynamicPrintConfig& print_preset);

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
