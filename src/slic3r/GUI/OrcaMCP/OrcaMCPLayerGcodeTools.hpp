// src/slic3r/GUI/OrcaMCP/OrcaMCPLayerGcodeTools.hpp
#pragma once

#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

// add_layer_gcode and delete_layer_gcode (OrcaMCPServer::register_layer_gcode_tools), and the plate's
// layer G-code get_scene_info lists. Their decisions are OrcaMCPLayerGcode.hpp.

namespace Slic3r { namespace GUI {
class PartPlate;
class Plater;
namespace OrcaMCP {

struct LayerGcodeRules;

// The heights of plate `plate`'s printed layers, as the Preview's layer slider numbers them: from its
// G-code while it has a result, and after an edit of its layer G-code took that away (the next slice
// writes the G-code again), the ones read last, while its Print's layers are still the same. nullopt:
// the plate's layers are not known now (never sliced, or an edit changed them since). Main thread.
std::optional<std::vector<double>> plate_layer_zs(PartPlate& plate);

// Whether plate_layer_zs can know `plate`'s layers once it is current: it has a slice result, or they were read from
// one before. A plate with neither is refused before the layer tools make it current.
bool plate_layers_may_be_known(PartPlate& plate);

// What decides what the Preview's layer slider offers on `plate`: the project's slots and colours, the plate's
// filaments, vase mode (its own, else the print preset's) and print sequence, and the filaments its objects print as
// the slicer counts them when its Print is current. Main thread.
LayerGcodeRules layer_gcode_rules(Plater& plater, PartPlate& plate);

// get_scene_info's layer_gcodes of plate `plate`: its G-code at a layer, each with its layer number when
// the plate's layers are known (plate_layer_zs), null otherwise, and a filament change with whether the slicer
// takes it (layer_gcode_json). Main thread.
nlohmann::json plate_layer_gcodes_json(PartPlate& plate);

}}} // namespace Slic3r::GUI::OrcaMCP
