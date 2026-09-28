// src/slic3r/GUI/OrcaMCP/OrcaMCPExports.cpp
#include "OrcaMCPExports.hpp"

#include <algorithm>
#include <set>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

bool ends_with_nocase(const std::string& text, const std::string& suffix) { return boost::algorithm::iends_with(text, suffix); }

// A plate whose G-code goes in the file, and one that keeps the file from being written.
bool counts_for_sliced_file(const SlicedFilePlate& plate) { return plate.has_objects && !plate.all_unprintable; }
bool ready_for_print(const SlicedFilePlate& plate) { return plate.sliced && !plate.check_refusal; }

std::optional<std::string> plate_not_ready(const SlicedFilePlate& plate)
{
    if (!plate.sliced)
        return "plate_index " + std::to_string(plate.index) + " has no slice result: slice_all, then wait_for_slice, first";
    return plate.check_refusal;
}

const SlicedFilePlate* plate_at(const std::vector<SlicedFilePlate>& plates, int index)
{
    const auto it = std::find_if(plates.begin(), plates.end(), [index](const SlicedFilePlate& p) { return p.index == index; });
    return it != plates.end() ? &*it : nullptr;
}

} // namespace

GcodeExportKind gcode_export_kind(const std::string& output_path)
{
    return ends_with_nocase(output_path, ".gcode.3mf") ? GcodeExportKind::sliced_file : GcodeExportKind::gcode;
}

std::optional<std::string> gcode_export_path_refusal(const std::string& output_path, bool all_plates)
{
    if (output_path.empty())
        return std::string("output_path is required: file dialogs cannot be opened from MCP.");
    const GcodeExportKind kind = gcode_export_kind(output_path);
    if (kind == GcodeExportKind::gcode && ends_with_nocase(output_path, ".3mf"))
        return "output_path \"" + output_path + "\" is a 3MF but not a .gcode.3mf: end it in .gcode.3mf for the sliced file, or save "
               "the project with export_3mf";
    if (kind == GcodeExportKind::gcode && all_plates)
        return std::string("A plain G-code file holds one plate: pass an output_path ending in .gcode.3mf for every plate's G-code in "
                           "one file, or select_plate and export_gcode each plate");
    return std::nullopt;
}

std::optional<std::string> sliced_file_refusal(const std::vector<SlicedFilePlate>& plates, bool all_plates, int selected_index)
{
    const std::string refused = "The sliced file was not written: ";
    if (!all_plates) {
        const SlicedFilePlate* selected = plate_at(plates, selected_index);
        if (selected == nullptr || !selected->has_objects)
            return refused + "plate_index " + std::to_string(selected_index) + " has nothing on it to export";
        if (const auto not_ready = plate_not_ready(*selected))
            return refused + *not_ready;
        return std::nullopt;
    }
    bool any_ready = false;
    for (const SlicedFilePlate& plate : plates) {
        if (!counts_for_sliced_file(plate))
            continue;
        if (const auto not_ready = plate_not_ready(plate))
            return refused + *not_ready;
        any_ready = true;
    }
    if (!any_ready)
        return refused + "no plate has a printable object on it";
    return std::nullopt;
}

std::vector<int> sliced_file_plates(const std::vector<SlicedFilePlate>& plates, bool all_plates, int selected_index)
{
    std::vector<int> indexes;
    for (const SlicedFilePlate& plate : plates)
        if ((all_plates || plate.index == selected_index) && ready_for_print(plate))
            indexes.push_back(plate.index);
    return indexes;
}

MeshExportDecision plan_mesh_export(const MeshExportRequest& request, int object_count, const std::function<bool(const std::string&)>& is_folder)
{
    const auto refuse = [](std::string why) { return MeshExportDecision{std::nullopt, std::move(why)}; };
    if (object_count <= 0)
        return refuse("The scene has no objects, so there is nothing to export.");
    if (request.output_path.empty())
        return refuse("output_path is required: file dialogs cannot be opened from MCP.");

    MeshExportPlan plan;
    plan.one_file_per_object = request.one_file_per_object;

    std::string format = request.format ? boost::algorithm::to_lower_copy(*request.format) : std::string();
    if (!format.empty() && format != "stl" && format != "drc")
        return refuse("format must be stl or drc, got \"" + *request.format + "\"");
    if (plan.one_file_per_object) {
        if (!is_folder(request.output_path))
            return refuse("With one_file_per_object, output_path is the folder the files go in, and \"" + request.output_path +
                          "\" is not an existing folder");
        if (format.empty())
            format = "stl";
    } else {
        const std::string extension = ends_with_nocase(request.output_path, ".stl") ? "stl" :
                                      ends_with_nocase(request.output_path, ".drc") ? "drc" :
                                                                                      std::string();
        if (extension.empty())
            return refuse("output_path must end in .stl or .drc (or pass one_file_per_object with a folder), got \"" +
                          request.output_path + "\"");
        if (!format.empty() && format != extension)
            return refuse("format is " + format + " but output_path ends in ." + extension + ": make them match");
        format = extension;
    }
    plan.drc = format == "drc";

    if (request.object_ids) {
        if (request.object_ids->empty())
            return refuse("object_ids is empty: leave it out to export every object");
        std::set<int> seen;
        for (int id : *request.object_ids) {
            if (id < 0 || id >= object_count)
                return refuse("Invalid object_id " + std::to_string(id) + ": the scene has " + std::to_string(object_count) + " objects");
            if (!seen.insert(id).second)
                return refuse("object_id " + std::to_string(id) + " is listed twice in object_ids");
        }
        plan.selection_only = true;
        plan.object_ids     = *request.object_ids;
    }
    return {plan, {}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
