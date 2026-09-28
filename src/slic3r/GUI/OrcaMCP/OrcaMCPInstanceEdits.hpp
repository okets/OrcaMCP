// src/slic3r/GUI/OrcaMCP/OrcaMCPInstanceEdits.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"

// The decisions of the instance tools -- set_instance_count, fill_bed_with_instances and delete_object
// with instance_id -- apart from the app, so they are tested without it
// (tests/slic3rutils/test_instance_edits.cpp). An object's instances are its linked copies: one mesh,
// each copy placed on its own. Each refusal is what the GUI's own action refuses, said before anything
// changes, with what to send instead. The tools are OrcaMCPArrangeTools.cpp and OrcaMCPPartTools.cpp.

namespace Slic3r {
class ModelObject;
namespace GUI { namespace OrcaMCP {

// The most instances "Set number of instances" takes (its number dialog's maximum).
constexpr int k_max_instance_count = 1000;

// The instance_id a call gives: nothing when it leaves instance_id out, else a whole number 0 or more.
// Given as anything else -- null, text, a fraction, a negative number -- `error` says so: it is never
// read as left out.
std::optional<int> read_instance_id(const nlohmann::json& params, std::string& error);

// What is wrong with `instance_id` for object `object_id`, or nothing: it must be one of its instances,
// as get_object_info's instance_placement lists them.
std::optional<std::string> instance_id_error(int object_id, const ModelObject& object, int instance_id);

// Why instances of the object cannot be added or counted -- set_instance_count, fill_bed_with_instances
// -- or nothing: the GUI's menu items are off (Plater::can_increase_instances) for an object with an
// unprintable instance, and for a piece of a cut (`cut_siblings`, its cut's pieces).
std::optional<std::string> instance_edit_refusal(int object_id, const ModelObject& object, const std::vector<int>& cut_siblings,
                                                 const std::string& tool);

// The count set_instance_count is given, or why it is not one: a whole number from 1 to 1000 (0 would
// delete the object: that is delete_object).
std::optional<int> read_instance_count(const nlohmann::json& params, std::string& error);

// Why delete_object cannot delete instance `instance_id` of the object, or nothing: its only instance is
// the object (the object list's "Last instance of an object cannot be deleted"), and instance_id and
// volume_id together name two things.
std::optional<std::string> instance_delete_refusal(int object_id, const ModelObject& object, int instance_id, bool volume_id_given);

// Of `boxes` (every instance on a plate, in plate millimetres), those among `candidates` (indices into
// `boxes`) whose footprint overlaps another one's: copies a GUI "Add instance" put down a small step
// from the last one, where they overlap it.
std::vector<int> overlapping_boxes(const std::vector<BoundingBoxf3>& boxes, const std::vector<int>& candidates);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
