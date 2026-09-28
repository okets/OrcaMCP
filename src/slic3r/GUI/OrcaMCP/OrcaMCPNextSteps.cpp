// src/slic3r/GUI/OrcaMCP/OrcaMCPNextSteps.cpp
#include "OrcaMCPNextSteps.hpp"

#include "libslic3r/Model.hpp"

#include <algorithm>
#include <optional>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

nlohmann::json next_step_json(const NextStep& step)
{
    nlohmann::json out = {{"tool", step.tool}};
    if (!step.arguments.is_null())
        out["arguments"] = step.arguments;
    out["why"] = step.why;
    return out;
}

void add_next_steps(nlohmann::json& response, const std::vector<NextStep>& steps)
{
    if (steps.empty())
        return;
    nlohmann::json listed = nlohmann::json::array();
    for (const NextStep& step : steps)
        listed.push_back(next_step_json(step));
    response["next_steps"] = std::move(listed);
}

std::string listed_ids(const std::vector<int>& ids, size_t max_listed)
{
    const size_t shown = std::min(ids.size(), max_listed);
    std::string  out;
    for (size_t i = 0; i < shown; ++i) {
        const bool last_named = i + 1 == shown && ids.size() <= max_listed;
        out += (i == 0 ? "" : last_named ? " and " : ", ") + std::to_string(ids[i]);
    }
    if (ids.size() > max_listed)
        out += " and " + std::to_string(ids.size() - max_listed) + " more";
    return out;
}

namespace {

// A model part of an object whose mesh is more than one shell.
struct MultiShellPart
{
    std::string name;
    int         shells = 0;
};

std::vector<MultiShellPart> multi_shell_parts(const ModelObject& object)
{
    std::vector<MultiShellPart> parts;
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume& volume = *object.volumes[i];
        if (!volume.is_model_part())
            continue;
        const int shells = volume.mesh().stats().number_of_parts;
        if (shells > 1)
            parts.push_back({volume.name, shells});
    }
    return parts;
}

std::string object_label(const ModelObject& object, int index) { return "object " + std::to_string(index) + " (\"" + object.name + "\")"; }

// get_mesh_health for the objects whose row shows the warning icon.
std::optional<NextStep> mesh_health_step(const Model& model, const std::vector<int>& flagged, const std::vector<MeshHealth>& health)
{
    if (flagged.empty())
        return std::nullopt;
    const int   first = flagged.front();
    std::string why   = flagged.size() == 1 ? object_label(*model.objects[first], first) + " shows the mesh warning icon: " +
                                                mesh_warning_reason(health[first])
                                            : "objects " + listed_ids(flagged) +
                                                " show the mesh warning icon (open edges or recorded repairs): call it for each";
    return NextStep{"get_mesh_health", std::move(why), {{"object_id", first}}};
}

// get_object_components for the objects with a part of more than one shell.
std::optional<NextStep> components_step(const Model& model, const std::vector<int>& with_shells)
{
    if (with_shells.empty())
        return std::nullopt;
    const int   first = with_shells.front();
    std::string why;
    if (with_shells.size() > 1) {
        why = "objects " + listed_ids(with_shells) +
              " each have a part made of several shells, loose parts or stray fragments: call it for each";
    } else {
        const std::vector<MultiShellPart> parts = multi_shell_parts(*model.objects[first]);
        why = parts.size() == 1 ? "part \"" + parts.front().name + "\" of " + object_label(*model.objects[first], first) + " has " +
                                      std::to_string(parts.front().shells) + " shells: a loose part or stray fragment may be one of them"
                                : object_label(*model.objects[first], first) + " has " + std::to_string(parts.size()) +
                                      " parts made of several shells: loose parts or stray fragments may be among them";
    }
    return NextStep{"get_object_components", std::move(why), {{"object_id", first}}};
}

} // namespace

std::vector<NextStep> mesh_next_steps(const Model& model, const std::vector<int>& object_indices, const std::vector<MeshHealth>& health)
{
    std::vector<int> flagged, with_shells;
    for (int index : object_indices) {
        if (index < 0 || size_t(index) >= model.objects.size())
            continue;
        if (size_t(index) < health.size() && health[size_t(index)].warning)
            flagged.push_back(index);
        if (!multi_shell_parts(*model.objects[size_t(index)]).empty())
            with_shells.push_back(index);
    }
    std::vector<NextStep> steps;
    for (std::optional<NextStep> step : {mesh_health_step(model, flagged, health), components_step(model, with_shells)})
        if (step)
            steps.push_back(std::move(*step));
    return steps;
}

std::vector<NextStep> mesh_next_steps(const Model& model, const std::vector<MeshHealth>& health)
{
    std::vector<int> every_object(model.objects.size());
    for (size_t i = 0; i < every_object.size(); ++i)
        every_object[i] = int(i);
    return mesh_next_steps(model, every_object, health);
}

std::vector<NextStep> mesh_repair_next_steps(const ModelObject& object, int object_id, const MeshHealth& health)
{
    if (health.manifold())
        return {};
    const int   edges = health.open_edges();
    std::string why   = object_label(object, object_id) + " has " + std::to_string(edges) + (edges == 1 ? " open edge" : " open edges") +
                      ": repair_mesh closes its holes, after splitting each volume into its shells and dropping those with no "
                      "volume; painting is cleared unless keep_painting keeps it";
    return {{"repair_mesh", std::move(why), {{"object_id", object_id}}}};
}

std::vector<NextStep> split_parts_next_steps(int object_id, int volume_id, const std::vector<int>& pieces)
{
    if (pieces.size() < 2)
        return {};
    return {{"get_object_components",
             "volume " + std::to_string(volume_id) + " of object " + std::to_string(object_id) + " is now " +
                 std::to_string(pieces.size()) + " parts (volumes " + listed_ids(pieces) +
                 "): this gives each part's facets and box, to find a fragment; delete_object with volume_id deletes one",
             {{"object_id", object_id}}}};
}

std::vector<NextStep> new_volume_next_steps(int object_id, int volume_id, const std::string& type_name, bool beside_object)
{
    std::vector<NextStep> steps;
    const nlohmann::json  target = {{"object_id", object_id}, {"volume_id", volume_id}};
    if (beside_object)
        steps.push_back({"move_object",
                         "the new " + type_name + " (volume " + std::to_string(volume_id) +
                             ") stands beside object " + std::to_string(object_id) +
                             ", at its right-front corner: move_object and scale_object with volume_id place and size it",
                         target});
    if (type_name == "modifier")
        steps.push_back({"set_object_config",
                         "a modifier changes only the settings it is given: set_object_config with volume_id " +
                             std::to_string(volume_id) + " gives them, for the part of the object inside it",
                         target});
    return steps;
}

std::vector<NextStep> assembled_next_steps(int object_id)
{
    return {{"get_object_info", "object " + std::to_string(object_id) + " is the assembly: this lists its volumes, with their boxes",
             {{"object_id", object_id}}}};
}

std::vector<NextStep> slice_start_next_steps(const SliceStartReport& report, std::optional<int> sliced_plate)
{
    if (report.status == SliceStart::started)
        return {{"wait_for_slice", "the slice runs in the background: wait_for_slice returns once it is over, with each plate's result",
                 nullptr}};
    if (report.reason == "busy_slicing")
        return {{"wait_for_slice", "the slicing pipeline is busy, so nothing was started: wait for it, then call slice_all again",
                 nullptr}};
    if (report.reason == "busy_job")
        return {{"get_slicing_status",
                 "an arrange, an orient or a bed fill holds the app, which wait_for_slice does not wait for: call slice_all again once "
                 "get_slicing_status's ui_job is null",
                 nullptr}};
    if (report.reason == "already_sliced" && sliced_plate)
        return {{"get_print_estimate",
                 "the plates are already sliced: it reads plate " + std::to_string(*sliced_plate) + "'s time and filament",
                 {{"plate_index", *sliced_plate}}}};
    return {};
}

std::vector<NextStep> export_next_steps(bool export_started)
{
    if (!export_started)
        return {};
    return {{"wait_for_slice",
             "the export has started, not failed: the G-code is still being written in the background, past how long "
             "export_gcode waits, and wait_for_slice returns once it is written",
             nullptr}};
}

std::vector<NextStep> cancel_slice_next_steps(bool still_stopping)
{
    if (!still_stopping)
        return {};
    return {{"wait_for_slice", "the cancelled slice is still stopping: wait_for_slice returns once the app has taken in how it ended",
             nullptr}};
}

std::vector<NextStep> layer_gcode_next_steps(int plate_index, bool changed)
{
    if (!changed)
        return {};
    return {{"slice_all",
             "plate " + std::to_string(plate_index) + "'s layer G-code changed, so it lost its slice: slice_all slices it again, with the "
             "change in its G-code (plates still sliced are kept)",
             nullptr}};
}

std::vector<NextStep> show_view_next_steps(bool slice_started)
{
    if (!slice_started)
        return {};
    return {{"wait_for_slice",
             "the Preview tab started slicing the selected plate, as it does for the user: wait_for_slice returns once it is over",
             nullptr}};
}

std::vector<NextStep> uniform_image_next_steps(size_t model_volumes, size_t drawn, int plate_index)
{
    const std::string plate = "plate " + std::to_string(plate_index);
    if (model_volumes == 0)
        return {{"get_scene_info", "the 3D view holds no model, so " + plate + " drew nothing: it shows what the scene holds", nullptr}};
    if (drawn == 0)
        return {{"get_scene_info", "nothing printable stands on " + plate + ": it lists the plate each object is on", nullptr}};
    return {{"render_plate_view",
             plate + "'s objects were drawn, but outside this view: without views it renders a contact sheet fitted to the plate",
             {{"plate_index", plate_index}, {"save_to_file", true}}}};
}

namespace {

// The (manual) support type of the same style: support only where enforcers are painted.
std::string manual_support_type(const std::string& support_type)
{
    return support_type.rfind("tree", 0) == 0 ? "tree(manual)" : "normal(manual)";
}

} // namespace

std::vector<NextStep> support_paint_next_steps(int object_id, bool support_enabled, bool enforcers_painted,
                                               const std::string& support_type)
{
    if (support_enabled || !enforcers_painted)
        return {};
    const std::string manual = manual_support_type(support_type);
    return {{"set_object_config",
             "painted support enforcers do nothing while enable_support is off: this turns it on for object " +
                 std::to_string(object_id) + " with support_type " + manual +
                 ", so support is generated only where painted (an (auto) type would also support every other overhang)",
             {{"object_id", object_id},
              {"settings", {{{"key", "enable_support"}, {"value", "1"}}, {{"key", "support_type"}, {"value", manual}}}}}}};
}

namespace {

// "instance 3" / "instances 3 and 4".
std::string named_ids(const std::string& noun, const std::vector<int>& ids)
{
    return noun + (ids.size() == 1 ? " " : "s ") + listed_ids(ids);
}

} // namespace

std::vector<NextStep> added_instances_next_steps(int object_id, int plate_index, const std::vector<int>& crowded,
                                                 const std::vector<int>& partly_off, const std::vector<int>& on_no_plate)
{
    const std::string step = "the GUI's Add instance puts each new instance a small step from the last one, not where there is room: ";
    const std::string object = " of object " + std::to_string(object_id);
    if (!on_no_plate.empty())
        return {{"arrange_objects",
                 step + named_ids("instance", on_no_plate) + object + (on_no_plate.size() == 1 ? " stands" : " stand") +
                     " on no plate, and a plate's arrange takes only the instances it holds: arranging every plate places them (it may "
                     "add plates)",
                 {{"all_plates", true}}}};
    if (crowded.empty() && partly_off.empty())
        return {};
    std::string why = step;
    if (!crowded.empty())
        why += named_ids("instance", crowded) + object + (crowded.size() == 1 ? " overlaps" : " overlap") + " another";
    if (!partly_off.empty())
        why += std::string(crowded.empty() ? "" : ", and ") + named_ids("instance", partly_off) + (partly_off.size() == 1 ? " stands" : " stand") +
               " partly off the plate";
    return {{"arrange_objects", why + ": it spaces plate " + std::to_string(plate_index) + "'s objects", {{"plate_index", plate_index}}}};
}

std::vector<NextStep> unplaced_instances_next_steps(int object_id, const std::vector<int>& on_no_plate, int instance_count)
{
    if (on_no_plate.empty())
        return {};
    const std::string what = named_ids("instance", on_no_plate) + " of object " + std::to_string(object_id) +
                             (on_no_plate.size() == 1 ? " stands" : " stand") + " on no plate: the fill's estimate added more than its plate's arrange fit";
    std::vector<NextStep> steps = {{"arrange_objects", what + "; arranging every plate puts them on plates of their own (it may add plates)",
                                    {{"all_plates", true}}}};
    const int  kept     = instance_count - int(on_no_plate.size());
    bool       the_last = true;
    for (std::size_t i = 0; i < on_no_plate.size(); ++i)
        the_last = the_last && on_no_plate[i] == kept + int(i);
    if (the_last)
        steps.push_back({"set_instance_count", what + "; this removes them, the last " + std::to_string(on_no_plate.size()),
                         {{"object_id", object_id}, {"count", kept}}});
    else
        steps.push_back({"delete_object", what + "; this removes the highest, and the ids after a deleted one shift down: call it for "
                                                 "each, highest first",
                         {{"object_id", object_id}, {"instance_id", on_no_plate.back()}}});
    return steps;
}

std::vector<NextStep> print_by_object_next_steps(int plate_index)
{
    return {{"arrange_objects",
             "plate " + std::to_string(plate_index) + " now prints by object, each object whole before the next, so the print head must "
             "clear the ones already printed: an arrange of the plate spaces them for it, as the app's notice suggests",
             {{"plate_index", plate_index}}}};
}

std::vector<NextStep> vase_settings_next_steps(const std::vector<int>& object_ids, const std::vector<std::string>& first_keys)
{
    if (object_ids.empty() || first_keys.empty())
        return {};
    return {{"reset_object_config",
             named_ids("object", object_ids) + (object_ids.size() == 1 ? " still carries" : " still carry") +
                 " the object settings spiral vase gives (one wall, no top shell, no infill), which print thin and open without "
                 "the vase: this removes them from object " + std::to_string(object_ids.front()) +
                 (object_ids.size() > 1 ? "; call it for each" : ""),
             {{"object_id", object_ids.front()}, {"keys", first_keys}}}};
}

std::vector<NextStep> installed_printer_next_steps(const std::vector<std::string>& printers)
{
    if (printers.empty())
        return {};
    std::string names;
    for (size_t i = 0; i < printers.size(); ++i)
        names += (i == 0 ? "'" : i + 1 == printers.size() ? " and '" : ", '") + printers[i] + "'";
    return {{"select_preset",
             names + (printers.size() == 1 ? " is" : " are") +
                 " installed, not selected: this selects the first; its filament slots and colours come with it",
             {{"type", "printer"}, {"name", printers.front()}}}};
}

std::vector<NextStep> slot_change_next_steps(bool renumbered)
{
    if (!renumbered)
        return {};
    return {{"get_scene_info",
             "the slots were renumbered: each object's filaments_used shows the slots it prints with now; check it again "
             "after any undo, which would bring back the objects' old slot numbers but not the slots"}};
}

std::vector<NextStep> missing_slot_next_steps(const std::vector<int>& missing_slots, bool slots_can_be_added)
{
    if (missing_slots.empty() || !slots_can_be_added)
        return {};
    return {{"add_filament_slot",
             "the printer holds filament in " + named_ids("slot", missing_slots) +
                 ", which the project has no filament slot for: this adds one per call, then match_project_to_printer again"}};
}

std::vector<NextStep> printer_control_next_steps()
{
    return {{"get_printer_status",
             "the printer applies a command within a few seconds; printer.controls there reads back what it now reports"}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
