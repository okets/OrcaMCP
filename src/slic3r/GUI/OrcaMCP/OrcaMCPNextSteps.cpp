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

std::vector<NextStep> slice_start_next_steps(const SliceStartReport& report)
{
    if (report.status == SliceStart::started)
        return {{"wait_for_slice", "the slice runs in the background: wait_for_slice returns once it is over, with each plate's result",
                 nullptr}};
    if (report.reason == "busy_slicing")
        return {{"wait_for_slice", "the slicing pipeline is busy, so nothing was started: wait for it, then call slice_all again",
                 nullptr}};
    if (report.reason == "busy_job")
        return {{"get_slicing_status",
                 "an arrange or an orient holds the app, which wait_for_slice does not wait for: call slice_all again once "
                 "get_slicing_status's ui_job is null",
                 nullptr}};
    if (report.reason == "already_sliced")
        return {{"get_print_estimate", "the plates are already sliced: it reads the time and filament of the selected one", nullptr}};
    return {};
}

std::vector<NextStep> export_next_steps(bool export_started)
{
    if (!export_started)
        return {};
    return {{"wait_for_slice",
             "the export has started, not failed: the G-code is still being written in the background, and "
             "wait_for_slice returns once it is written",
             nullptr}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
