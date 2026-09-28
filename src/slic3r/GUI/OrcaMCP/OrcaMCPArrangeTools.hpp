// src/slic3r/GUI/OrcaMCP/OrcaMCPArrangeTools.hpp
#pragma once

#include <nlohmann/json.hpp>

// arrange_objects -- one plate, or every plate as the A key arranges them, with the arrange menu's
// options -- and the instance tools set_instance_count and fill_bed_with_instances, registered in
// OrcaMCPArrangeTools.cpp (OrcaMCPServer::register_arrange_tools). The decisions are
// OrcaMCPArrangeOptions.hpp and OrcaMCPInstanceEdits.hpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// arrange_objects: the plate's Arrange (a plate, made current first) or the A key (every plate), with
// the options the call gives saved as the arrange menu saves them, answered once the arrange has been
// applied. HTTP thread.
nlohmann::json arrange_objects(const nlohmann::json& params);

// The arrange schema's own properties: all_plates, plate_index and the arrange menu's options.
nlohmann::json arrange_objects_properties();

}}} // namespace Slic3r::GUI::OrcaMCP
