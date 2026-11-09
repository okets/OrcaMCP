// src/slic3r/GUI/OrcaMCP/OrcaMCPObjectOrder.hpp
#pragma once

#include "slic3r/GUI/GUI_ObjectList.hpp"

#include <optional>
#include <string>
#include <vector>

// order_objects' decisions apart from the app: which sort a name asks for, and whether a plate's objects
// already stand in it. The tool is in OrcaMCPArrangeTools.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// "name", "filament", "height" or "size" as the plate menu's Order By offers them; nothing for any other.
std::optional<ObjectOrder> object_order_named(const std::string& by);

// The four names, as "name, filament, height or size", for the refusal.
std::string object_order_names();

// Whether `plate_objects`, in their list order, already stand sorted by `order` (descending when asked):
// sort_objects, the app's own sort, would leave them as they are.
bool plate_in_order(std::vector<ModelObject*> plate_objects, ObjectOrder order, bool descending);

}}} // namespace Slic3r::GUI::OrcaMCP
