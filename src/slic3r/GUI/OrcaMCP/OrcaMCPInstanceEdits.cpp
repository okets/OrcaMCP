// src/slic3r/GUI/OrcaMCP/OrcaMCPInstanceEdits.cpp
#include "OrcaMCPInstanceEdits.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartEdits.hpp"

#include "libslic3r/Model.hpp"

#include <algorithm>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

std::string object_text(int object_id) { return "Object " + std::to_string(object_id); }

bool footprints_overlap(const BoundingBoxf3& a, const BoundingBoxf3& b)
{
    return a.min.x() < b.max.x() && b.min.x() < a.max.x() && a.min.y() < b.max.y() && b.min.y() < a.max.y();
}

} // namespace

std::optional<int> read_instance_id(const nlohmann::json& params, std::string& error)
{
    if (!params.contains("instance_id"))
        return std::nullopt;
    int instance_id = -1;
    if (!parse_integer_param(params.at("instance_id"), instance_id) || instance_id < 0) {
        error = "instance_id must be a whole number 0 or more, as get_object_info's instance_placement lists the instances; got " +
                params.at("instance_id").dump();
        return std::nullopt;
    }
    return instance_id;
}

std::optional<std::string> instance_id_error(int object_id, const ModelObject& object, int instance_id)
{
    if (instance_id >= 0 && std::size_t(instance_id) < object.instances.size())
        return std::nullopt;
    return object_text(object_id) + " has no instance " + std::to_string(instance_id) + ": its instances are 0 to " +
           std::to_string(int(object.instances.size()) - 1) + ", as get_object_info's instance_placement lists them";
}

std::optional<std::string> instance_edit_refusal(int object_id, const ModelObject& object, const std::vector<int>& cut_siblings,
                                                 const std::string& tool)
{
    if (!cut_siblings.empty())
        return cut_refusal(object_id, cut_siblings, "its instances are not added or counted (the menu's instance items are off)");
    std::vector<int> unprintable;
    for (std::size_t i = 0; i < object.instances.size(); ++i)
        if (!object.instances[i]->printable)
            unprintable.push_back(int(i));
    if (unprintable.empty())
        return std::nullopt;
    return object_text(object_id) + " has an unprintable instance (" + listed_ids(unprintable) + "), and the GUI adds instances only to an "
           "object whose every instance prints: set_object_printable {\"object_id\": " + std::to_string(object_id) +
           ", \"printable\": true} first, then call " + tool + " again";
}

std::optional<int> read_instance_count(const nlohmann::json& params, std::string& error)
{
    int count = 0;
    if (!params.contains("count") || !parse_integer_param(params.at("count"), count)) {
        error = "count must be a whole number of instances, 1 to " + std::to_string(k_max_instance_count) + "; got " +
                (params.contains("count") ? params.at("count").dump() : std::string("none"));
        return std::nullopt;
    }
    if (count < 1) {
        error = "count must be 1 or more: an object has at least one instance. To remove the object, call delete_object";
        return std::nullopt;
    }
    if (count > k_max_instance_count) {
        error = "count must be at most " + std::to_string(k_max_instance_count) + ", as the GUI's Set number of instances takes";
        return std::nullopt;
    }
    return count;
}

std::optional<std::string> instance_delete_refusal(int object_id, const ModelObject& object, int instance_id, bool volume_id_given)
{
    if (volume_id_given)
        return std::string("give instance_id or volume_id, not both: a volume is shared by every instance, so deleting one deletes it "
                           "from each copy");
    if (const auto error = instance_id_error(object_id, object, instance_id))
        return error;
    if (object.instances.size() == 1)
        return object_text(object_id) + " has one instance, which is the object itself (the object list's \"Last instance of an object "
               "cannot be deleted\"): call delete_object without instance_id to remove the object";
    return std::nullopt;
}

std::vector<int> overlapping_boxes(const std::vector<BoundingBoxf3>& boxes, const std::vector<int>& candidates)
{
    std::vector<int> overlapping;
    for (int candidate : candidates) {
        if (candidate < 0 || std::size_t(candidate) >= boxes.size() || !boxes[std::size_t(candidate)].defined)
            continue;
        for (std::size_t other = 0; other < boxes.size(); ++other)
            if (int(other) != candidate && boxes[other].defined && footprints_overlap(boxes[std::size_t(candidate)], boxes[other])) {
                overlapping.push_back(candidate);
                break;
            }
    }
    return overlapping;
}

}}} // namespace Slic3r::GUI::OrcaMCP
