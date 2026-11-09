// src/slic3r/GUI/OrcaMCP/OrcaMCPObjectOrder.hpp
#pragma once

#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
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

// A by-object plate's print order as get_scene_info reports it: `by_position` holds each printed instance with
// its 1-based print position (ModelInstance::arrange_order, written by Print::sequential_print_clearance_valid),
// `warning` upstream's words for objects printed later than their place in the object list ("" when none).
// Gives {print_order: [{object_id, instance_id}] in print order, print_order_differs (the order is not the
// object list's, object then instance), print_order_note (the warning, null when empty), print_order_unknown: null}.
// An instance of no object of `model` is left out.
nlohmann::json print_order_json(const std::vector<std::pair<const ModelInstance*, int>>& by_position, const Model& model,
                                const std::string& warning);

// The same four keys for a plate with no order, and why: "not by object", "not the current plate", ...
nlohmann::json print_order_unknown_json(const std::string& why);

}}} // namespace Slic3r::GUI::OrcaMCP
