// src/slic3r/GUI/OrcaMCP/OrcaMCPObjectOrder.cpp
#include "OrcaMCPObjectOrder.hpp"

#include "libslic3r/Model.hpp"

#include <algorithm>

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

nlohmann::json print_order_json(const std::vector<PrintedInstance>& by_position, const Model& model,
                                const std::string& warning)
{
    std::vector<PrintedInstance> sorted = by_position;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.position < b.position; });

    std::vector<std::pair<int, int>> order;
    for (const PrintedInstance& entry : sorted) {
        const auto found = std::find_if(model.objects.begin(), model.objects.end(),
                                        [&](const ModelObject* object) { return object->id() == entry.object_id; });
        if (found == model.objects.end())
            continue;
        const auto& instances = (*found)->instances;
        const auto  instance  = std::find_if(instances.begin(), instances.end(),
                                             [&](const ModelInstance* i) { return i->id() == entry.instance_id; });
        if (instance == instances.end())
            continue;
        order.emplace_back(int(found - model.objects.begin()), int(instance - instances.begin()));
    }

    nlohmann::json list = nlohmann::json::array();
    for (const auto& [object_id, instance_id] : order)
        list.push_back({{"object_id", object_id}, {"instance_id", instance_id}});
    return {{"print_order", std::move(list)},
            {"print_order_differs", !std::is_sorted(order.begin(), order.end())},
            {"print_order_note", warning.empty() ? nlohmann::json(nullptr) : nlohmann::json(warning)},
            {"print_order_unknown", nullptr}};
}

nlohmann::json print_order_unknown_json(const std::string& why)
{
    return {{"print_order", nullptr}, {"print_order_differs", nullptr}, {"print_order_note", nullptr}, {"print_order_unknown", why}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
