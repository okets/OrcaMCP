// src/slic3r/GUI/OrcaMCP/OrcaMCPPlateTools.hpp
#pragma once

#include <nlohmann/json.hpp>

// set_plate_settings, registered in OrcaMCPPlateTools.cpp (OrcaMCPServer::register_plate_tools), and the
// plate settings get_scene_info reports with each plate.

namespace Slic3r { namespace GUI {
class PartPlate;
namespace OrcaMCP {

// {"settings": the plate's own settings in set_plate_settings' argument shape, "effective": what applies
// on it}. Main thread.
nlohmann::json plate_settings_entry_json(PartPlate& plate);

}}} // namespace Slic3r::GUI::OrcaMCP
