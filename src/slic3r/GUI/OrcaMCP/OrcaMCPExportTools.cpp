// src/slic3r/GUI/OrcaMCP/OrcaMCPExportTools.cpp
#include "OrcaMCPExportTools.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPExports.hpp"
#include "OrcaMCPGcodeCheck.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPServer.hpp"
#include "OrcaMCPUiJob.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "libslic3r/Model.hpp"

#include <boost/filesystem.hpp>

#include <chrono>
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// Every plate as Export plate sliced file reads it.
std::vector<SlicedFilePlate> sliced_file_plates_of(PartPlateList& plate_list)
{
    std::vector<SlicedFilePlate> plates;
    for (int i = 0; i < plate_list.get_plate_count(); ++i) {
        PartPlate* plate = plate_list.get_plate(i);
        SlicedFilePlate state;
        state.index           = i;
        state.has_objects     = !plate->empty();
        state.all_unprintable = plate->is_all_instances_unprintable();
        state.sliced          = plate_gcode_checked(*plate);
        state.check_refusal   = plate_gcode_check_refusal(*plate, i);
        plates.push_back(std::move(state));
    }
    return plates;
}

} // namespace

nlohmann::json export_sliced_file(Plater& plater, McpDialogSuppressionGuard& guard, const std::string& output_path, bool all_plates)
{
    if (plater.model().objects.empty())
        return error_response("The scene has no objects, so there is no G-code to export.");
    if (plater.is_export_gcode_scheduled())
        return error_response("Another export job is running.");
    PartPlateList&                     plate_list = plater.get_partplate_list();
    const std::vector<SlicedFilePlate> plates     = sliced_file_plates_of(plate_list);
    const int                          selected   = plate_list.get_curr_plate_index();
    if (const auto refusal = sliced_file_refusal(plates, all_plates, selected))
        return error_response(*refusal);

    guard.answer_file(output_path);
    const bool        written = plater.export_gcode_3mf(all_plates);
    const std::string file    = sliced_file_path(output_path); // where the app wrote it
    nlohmann::json    answer;
    if (written) {
        answer = {{"status", "success"},
                  {"output_path", file},
                  {"format", "gcode.3mf"},
                  {"plates", sliced_file_plates(plates, all_plates, selected)}};
        boost::system::error_code error;
        const auto                bytes = boost::filesystem::file_size(file, error);
        if (!error)
            answer["bytes"] = bytes;
    } else {
        answer = error_response("The app did not write the sliced file; error_messages or active_warnings say why when it did");
    }
    answer["active_warnings"] = get_active_warnings_json(&plater);
    return guard.fail_on_errors(guard.report(answer));
}

nlohmann::json wait_for_gcode_export(GcodeExportOutcome& outcome, const std::string& output_path, const nlohmann::json& started)
{
    const auto started_at = std::chrono::steady_clock::now();
    // Ended when the app has told the outcome, or has taken the export off without a completion (reset_export: a new
    // project, a project opened), which the scheduled path's going says.
    const auto ended = [&outcome]() {
        if (outcome.state() != GcodeExportOutcome::State::pending)
            return true;
        bool still_scheduled = true;
        try {
            still_scheduled = run_on_main_thread([] { return nlohmann::json(wxGetApp().plater()->is_export_gcode_scheduled()); }).get<bool>();
        } catch (const McpShuttingDown&) {
            return false; // the wait's own quit check answers it
        }
        if (!still_scheduled)
            outcome.end(GcodeExportOutcome::State::dropped);
        return !still_scheduled;
    };
    const UiJobWait waited = wait_until(ended, tool_wait_cap(), std::chrono::milliseconds(50), [] { wxWakeUpIdle(); });
    const double    waited_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
    const GcodeExportWait wait = waited == UiJobWait::finished  ? GcodeExportWait::ended :
                                 waited == UiJobWait::quitting  ? GcodeExportWait::quitting :
                                                                  GcodeExportWait::timed_out;
    std::optional<std::uintmax_t> bytes;
    boost::system::error_code     error;
    if (const auto size = boost::filesystem::file_size(output_path, error); !error)
        bytes = size;
    nlohmann::json answer = gcode_export_answer(outcome, wait, output_path, bytes, std::round(waited_s * 10.) / 10.);
    if (started.contains("info_messages"))
        answer["info_messages"] = started.at("info_messages");
    try {
        answer["active_warnings"] = run_on_main_thread([] { return get_active_warnings_json(wxGetApp().plater()); });
    } catch (const McpShuttingDown&) {
    }
    return answer;
}

}}} // namespace Slic3r::GUI::OrcaMCP

namespace {

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

// export_stl's arguments, or why they cannot be read.
std::optional<std::string> read_mesh_export_request(const nlohmann::json& params, MeshExportRequest& request)
{
    std::string error;
    request.output_path = read_text(params, "output_path", error).value_or(std::string());
    request.format      = read_text(params, "format", error);
    request.one_file_per_object = read_flag(params, "one_file_per_object", error).value_or(false);
    if (!error.empty())
        return error;
    if (params.contains("object_ids")) {
        if (!params.at("object_ids").is_array())
            return std::string("object_ids must be a list of objects, by object_id");
        std::vector<int> ids;
        for (const nlohmann::json& given : params.at("object_ids")) {
            int id = -1;
            if (!parse_integer_param(given, id))
                return "object_ids must be whole numbers, as get_scene_info gives object_id; got " + given.dump();
            ids.push_back(id);
        }
        request.object_ids = std::move(ids);
    }
    return std::nullopt;
}

bool is_folder(const std::string& path)
{
    boost::system::error_code error;
    return boost::filesystem::is_directory(path, error);
}

// The objects `object_ids` selected in the 3D view for the object menu's Export as one STL / as STLs,
// which export what is selected, and the selection as it was put back afterwards (a selection is not an
// edit: no undo step, and the user's stays).
class ScopedObjectSelection
{
public:
    ScopedObjectSelection(Plater& plater, const std::vector<int>& object_ids)
        : m_selection(plater.get_view3D_canvas3D()->get_selection()), m_before(m_selection.get_volume_idxs())
    {
        m_selection.clear();
        for (int id : object_ids)
            m_selection.add_object(unsigned(id), /*as_single_selection=*/false);
    }
    ~ScopedObjectSelection()
    {
        m_selection.clear();
        for (unsigned int idx : m_before)
            m_selection.add(idx, /*as_single_selection=*/false);
    }

private:
    Selection&             m_selection;
    Selection::IndicesList m_before;
};

nlohmann::json files_json(const std::vector<std::string>& files)
{
    nlohmann::json list = nlohmann::json::array();
    for (const std::string& file : files) {
        boost::system::error_code error;
        const auto                bytes = boost::filesystem::file_size(file, error);
        list.push_back({{"path", file}, {"bytes", error ? nlohmann::json(nullptr) : nlohmann::json(bytes)}});
    }
    return list;
}

nlohmann::json export_stl_on_main_thread(const MeshExportRequest& request)
{
    Plater*                  plater   = wxGetApp().plater();
    const MeshExportDecision decision = plan_mesh_export(request, int(plater->model().objects.size()), is_folder);
    if (!decision.plan)
        return error_response(decision.refusal);
    if (const auto refusal = edit_job_refusal(!plater->get_ui_job_worker().is_idle(), "export_stl"))
        return error_response(*refusal);
    const MeshExportPlan& plan = *decision.plan;

    McpDialogSuppressionGuard guard;
    if (plan.one_file_per_object)
        guard.answer_folder(request.output_path);
    else
        guard.answer_file(request.output_path);
    std::vector<std::string> written;
    {
        std::optional<ScopedObjectSelection> selected;
        if (plan.selection_only)
            selected.emplace(*plater, plan.object_ids);
        plater->export_stl(/*extended=*/false, plan.selection_only, plan.one_file_per_object, plan.drc ? FT_DRC : FT_STL, &written);
    }
    nlohmann::json answer = written.empty() ? error_response("The app wrote no file; error_messages or active_warnings say why when it did") :
                                              nlohmann::json{{"status", "success"}};
    answer["format"]              = plan.drc ? "drc" : "stl";
    answer["one_file_per_object"] = plan.one_file_per_object;
    answer["object_ids"]          = plan.selection_only ? nlohmann::json(plan.object_ids) : nlohmann::json("all");
    answer["files"]               = files_json(written);
    answer["active_warnings"]     = get_active_warnings_json(plater);
    return guard.fail_on_errors(guard.report(answer));
}

} // namespace

void Slic3r::GUI::OrcaMCPServer::register_export_tools()
{
    register_tool({
        "export_stl",
        ToolCategory::Scene,
        "Export objects as STL or DRC mesh files",
        "Export objects' meshes as STL (or Draco DRC) files, as the app's File > Export > Export all objects as one STL / as "
        "STLs and the object menu's Export as one STL / as STLs write them: negative parts cut from the parts (the parts alone, "
        "with a notice in active_warnings, when that fails), modifiers and support volumes left out. Without object_ids every "
        "object goes in, in plate coordinates; with object_ids only those, as the object menu exports what is selected (an "
        "object with one instance is written at its own origin). one_file_per_object writes one file per object (per selected "
        "instance with object_ids), a single object's too, into the folder output_path names, each named after its object, "
        "never overwriting: a name "
        "taken gets (1), (2), ... A single file at output_path is overwritten. files lists what was written. No undo step: the "
        "scene does not change, and the user's selection is put back.",
        {{"type", "object"},
         {"properties",
          {{"output_path",
            {{"type", "string"},
             {"description", "The file to write, ending in .stl or .drc; with one_file_per_object the existing folder the files go "
                             "in (required: file dialogs cannot be opened from MCP)"}}},
           {"format", {{"type", "string"}, {"enum", {"stl", "drc"}}, {"description", "stl (default) or drc; for one file, its extension decides"}}},
           {"object_ids",
            {{"type", "array"},
             {"items", {{"type", "integer"}, {"minimum", 0}}},
             {"description", "The objects to export, by object_id (default: every object)"}}},
           {"one_file_per_object", {{"type", "boolean"}, {"description", "One file per object in the folder output_path (default false: one file)"}}}}},
         {"required", {"output_path"}}},
        [](const nlohmann::json& params) -> nlohmann::json {
            MeshExportRequest request;
            if (const auto error = read_mesh_export_request(params, request))
                return error_response(*error);
            return run_on_main_thread([request]() -> nlohmann::json { return export_stl_on_main_thread(request); });
        }});
}
