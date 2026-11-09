// src/slic3r/GUI/OrcaMCP/OrcaMCPObjectOrder.cpp
#include "OrcaMCPObjectOrder.hpp"

#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

std::string object_order_names() { return "name, filament, height or size"; }

std::optional<ObjectOrder> object_order_named(const std::string& by)
{
    if (by == "name")
        return ObjectOrder::Name;
    if (by == "filament")
        return ObjectOrder::Filament;
    if (by == "height")
        return ObjectOrder::Height;
    if (by == "size")
        return ObjectOrder::Size;
    return std::nullopt;
}

bool plate_in_order(std::vector<ModelObject*> plate_objects, ObjectOrder order, bool descending)
{
    const std::vector<ModelObject*> before = plate_objects;
    sort_objects(plate_objects, order, descending);
    return plate_objects == before;
}

}}} // namespace Slic3r::GUI::OrcaMCP
