// src/slic3r/GUI/OrcaMCP/OrcaMCPSourceFileTools.cpp
//
// reload_from_disk and replace_volume_with_file: the object list's Reload from disk (and the plate menu's
// Reload All), Replace 3D file and Replace all with 3D files, run as the menus run them, with the call's
// path answering the file and folder dialogs they open (mcp_answer_path_dialog). Their decisions are
// OrcaMCPSourceFiles.hpp.

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPMeshHealth.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPPartTools.hpp"
#include "OrcaMCPServer.hpp"
#include "OrcaMCPSourceFiles.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "libslic3r/Model.hpp"

#include <boost/filesystem.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

bool is_file(const std::string& path)
{
    boost::system::error_code error;
    return boost::filesystem::is_regular_file(path, error);
}

bool is_folder(const std::string& path)
{
    boost::system::error_code error;
    return boost::filesystem::is_directory(path, error);
}

// The scene as the reload and the replace read it.
std::vector<SourceObject> source_objects(const Model& model)
{
    std::vector<SourceObject> scene;
    for (size_t o = 0; o < model.objects.size(); ++o) {
        const ModelObject& object = *model.objects[o];
        SourceObject       entry;
        entry.object_id = int(o);
        entry.cut       = object.is_cut();
        for (size_t v = 0; v < object.volumes.size(); ++v) {
            const ModelVolume& volume = *object.volumes[v];
            entry.volumes.push_back({int(v), volume.name, volume.source.input_file, is_reloadable_volume(volume)});
        }
        scene.push_back(std::move(entry));
    }
    return scene;
}

// Each object's volumes by id, to tell afterwards which volumes a reload or a replace made anew.
using VolumeIds = std::map<int, std::set<size_t>>;
VolumeIds volume_ids_of(const Model& model, const std::set<int>& objects)
{
    VolumeIds ids;
    for (int o : objects)
        for (const ModelVolume* volume : model.objects[size_t(o)]->volumes)
            ids[o].insert(volume->id().id);
    return ids;
}

// The volumes of `objects` that are not among `before`'s: the ones the reload or the replace loaded.
nlohmann::json new_volumes_json(const Model& model, const VolumeIds& before, std::vector<int>& changed_objects)
{
    nlohmann::json list = nlohmann::json::array();
    for (const auto& [o, ids] : before) {
        if (size_t(o) >= model.objects.size())
            continue;
        const ModelObject& object  = *model.objects[size_t(o)];
        bool               changed = false;
        for (size_t v = 0; v < object.volumes.size(); ++v) {
            const ModelVolume& volume = *object.volumes[v];
            if (ids.count(volume.id().id) != 0)
                continue;
            list.push_back({{"object_id", o}, {"volume_id", int(v)}, {"name", volume.name}, {"source_file", volume.source.input_file}});
            changed = true;
        }
        if (changed)
            changed_objects.push_back(o);
    }
    return list;
}

std::set<int> objects_of(const std::vector<std::pair<int, int>>& volumes)
{
    std::set<int> objects;
    for (const auto& [o, v] : volumes)
        objects.insert(o);
    return objects;
}

// What follows a reload or a replace that loaded new meshes: each changed object's placement (the mesh
// may have grown or shrunk on its plate), and the mesh checks a loaded object gets.
void report_loaded(Plater& plater, const std::vector<int>& changed_objects, nlohmann::json& answer)
{
    nlohmann::json objects = nlohmann::json::array();
    for (int o : changed_objects) {
        nlohmann::json placement = {{"object_id", o}};
        rehome_and_report_placement(placement, o, /*moved=*/true);
        objects.push_back(std::move(placement));
    }
    answer["objects"]         = std::move(objects);
    answer["active_warnings"] = get_active_warnings_json(&plater);
    add_next_steps(answer, mesh_next_steps(plater.model(), changed_objects, model_mesh_health(plater.model())));
}

// ---- reload_from_disk ----

std::optional<std::string> read_reload(const nlohmann::json& params, ReloadRequest& request)
{
    std::string error;
    if (params.contains("object_id")) {
        int id = -1;
        if (!parse_integer_param(params.at("object_id"), id))
            return "object_id must be a whole number, as get_scene_info gives it; got " + params.at("object_id").dump();
        request.object_id = id;
    }
    request.volume_id = read_volume_id(params, error);
    request.file_path = read_text(params, "file_path", error);
    if (!error.empty())
        return error;
    return std::nullopt;
}

nlohmann::json reload_on_main_thread(const ReloadRequest& request)
{
    Plater*              plater   = wxGetApp().plater();
    Model&               model    = plater->model();
    const ReloadDecision decision = plan_reload(request, source_objects(model));
    if (decision.volumes.empty())
        return error_response(decision.refusal);
    if (const auto refusal = edit_job_refusal(!plater->get_ui_job_worker().is_idle(), "reload_from_disk"))
        return error_response(*refusal);
    std::vector<std::string> missing;
    for (const boost::filesystem::path& path : reload_sources(model, decision.volumes).missing)
        missing.push_back(path.string());
    if (const auto refusal = reload_sources_refusal(missing, request.file_path, is_file))
        return error_response(*refusal);

    McpDialogSuppressionGuard guard;
    if (request.file_path)
        guard.answer_file(*request.file_path);
    const VolumeIds before = volume_ids_of(model, objects_of(decision.volumes));
    if (request.object_id) {
        ModelObject* object = model.objects[size_t(*request.object_id)];
        ModelVolume* volume = request.volume_id && object->volumes.size() > 1 ? object->volumes[size_t(*request.volume_id)] : nullptr;
        if (const auto refusal = select_as_a_click_does(*plater, *request.object_id, volume))
            return guard.report(error_response(*refusal));
        plater->reload_from_disk();
    } else {
        plater->reload_all_from_disk();
    }

    std::vector<int> changed_objects;
    nlohmann::json   reloaded = new_volumes_json(model, before, changed_objects);
    if (reloaded.empty()) {
        std::string why = "Nothing was reloaded";
        if (!missing.empty() && request.file_path)
            why += ": file_path answered the first missing file, and the app could not find the rest (" + file_names(missing) +
                   ") beside it; put them in one folder, or reload the objects one at a time";
        why += "; info_messages says what the app reported";
        return guard.report(error_response(why));
    }
    nlohmann::json answer = {{"status", "success"}, {"reloaded", std::move(reloaded)}};
    report_loaded(*plater, changed_objects, answer);
    return guard.report(answer);
}

// ---- replace_volume_with_file ----

std::optional<std::string> read_replace(const nlohmann::json& params, ReplaceRequest& request)
{
    std::string error;
    if (!parse_integer_param(params.at("object_id"), request.object_id))
        return "object_id must be a whole number, as get_scene_info gives it; got " + params.at("object_id").dump();
    request.volume_id = read_volume_id(params, error);
    request.file_path = read_text(params, "file_path", error);
    request.folder    = read_text(params, "folder", error);
    if (!error.empty())
        return error;
    return std::nullopt;
}

// Replace 3D file needs exactly one of the object's copies selected in the 3D view: the part's row selects
// it on every instance, so the selection is narrowed to instance 0's, as a click on that part in the 3D
// view does.
void select_one_copy(Plater& plater, int object_id, int volume_id)
{
    Selection& selection = plater.get_view3D_canvas3D()->get_selection();
    if (selection.get_volume_idxs().size() > 1)
        selection.add_volume(unsigned(object_id), unsigned(volume_id), 0, /*as_single_selection=*/true);
}

nlohmann::json replace_on_main_thread(const ReplaceRequest& request)
{
    Plater*               plater   = wxGetApp().plater();
    Model&                model    = plater->model();
    const ReplaceDecision decision = plan_replace(request, source_objects(model), is_file, is_folder);
    if (decision.volume_ids.empty())
        return error_response(decision.refusal);
    if (const auto refusal = edit_job_refusal(!plater->get_ui_job_worker().is_idle(), "replace_volume_with_file"))
        return error_response(*refusal);

    ModelObject* object = model.objects[size_t(request.object_id)];
    std::string  closed_tool;
    if (const auto failed = close_toolbar_tool_for_change(*plater, request.object_id, object->volumes.size(), closed_tool))
        return *failed;
    McpDialogSuppressionGuard guard;
    const VolumeIds           before = volume_ids_of(model, {request.object_id});
    ModelVolume*              volume = request.volume_id && object->volumes.size() > 1 ? object->volumes[size_t(*request.volume_id)] : nullptr;
    if (const auto refusal = select_as_a_click_does(*plater, request.object_id, volume))
        return with_closed_tool(guard.report(error_response(*refusal)), closed_tool);
    if (request.file_path) {
        guard.answer_file(*request.file_path);
        select_one_copy(*plater, request.object_id, decision.volume_ids.front());
        plater->replace_with_stl();
    } else {
        guard.answer_folder(*request.folder);
        // One undo step for the whole folder, where the menu takes one per part: the plan found a file for at
        // least one part, so the step has a change in it.
        Plater::TakeSnapshot snapshot(plater, _u8L("Replace with 3D file"));
        plater->replace_all_with_stl();
    }

    std::vector<int> changed_objects;
    nlohmann::json   replaced = new_volumes_json(model, before, changed_objects);
    if (replaced.empty())
        return with_closed_tool(guard.report(error_response("Nothing was replaced: the file could not be read, or holds more than one "
                                                            "part (info_messages says what the app reported)")),
                                closed_tool);
    nlohmann::json answer = {{"status", "success"}, {"replaced", std::move(replaced)}, {"object_name", object->name}};
    report_loaded(*plater, changed_objects, answer);
    return with_closed_tool(guard.report(answer), closed_tool);
}

} // namespace

void Slic3r::GUI::OrcaMCPServer::register_source_file_tools()
{
    register_tool({
        "reload_from_disk",
        ToolCategory::Models,
        "Reload objects from their source files",
        "Reload an object's parts from the files they were loaded from, as the object list's Reload from disk does -- or, "
        "without object_id, every object, as the plate menu's Reload All does: each part takes its file's mesh as it is now, "
        "keeping its place, settings, filament and type (painting as the app's keep_painting setting says). volume_id "
        "reloads one part. A file is found where it was loaded from, else beside the object's own file; when it is in neither "
        "place the app asks for it, and file_path answers: a file of the same name there (its folder is then searched for the "
        "other missing ones), or another file, which replaces the part. Without file_path a missing file is refused, "
        "naming it. A piece of a cut is refused (a reload would bring back the whole model). reloaded lists the parts loaded "
        "anew, with the placement; a file that fails to load is named in info_messages and the others are still reloaded. "
        "One undo step, taken only when something changed.",
        {{"type", "object"},
         {"properties",
          {{"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "The object to reload (default: every object)"}}},
           {"volume_id", {{"type", "integer"}, {"minimum", 0}, {"description", "One part of object_id, as get_object_info numbers them"}}},
           {"file_path",
            {{"type", "string"},
             {"description", "Where a source file that moved or was deleted is now, or another file to load into the part"}}}}}},
        [](const nlohmann::json& params) -> nlohmann::json {
            ReloadRequest request;
            if (const auto error = read_reload(params, request))
                return error_response(*error);
            return run_on_main_thread([request]() -> nlohmann::json { return reload_on_main_thread(request); });
        }});

    register_tool({
        "replace_volume_with_file",
        ToolCategory::Models,
        "Replace a part's mesh with another file",
        "Replace a part's mesh with another 3D file (STL, 3MF, STEP, OBJ, ...), as the object list's Replace 3D file does: "
        "the new mesh takes the part's place, settings, filament, type and name (the object's too when it has one part), and "
        "painting as the app's keep_painting setting says. The file must hold one part. volume_id names the part (needed when "
        "the object has several). folder instead replaces every part of the object (or volume_id's) with the file of its "
        "source file's name in that folder, as Replace all with 3D files does, skipping parts whose file is not there. A piece "
        "of a cut is refused. An open toolbar tool is closed first (closed_toolbar_tool). replaced lists the parts loaded anew, "
        "with the placement. One undo step.",
        {{"type", "object"},
         {"properties",
          {{"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "The object"}}},
           {"volume_id", {{"type", "integer"}, {"minimum", 0}, {"description", "The part, as get_object_info numbers them (needed with several)"}}},
           {"file_path", {{"type", "string"}, {"description", "The new file for the part"}}},
           {"folder", {{"type", "string"}, {"description", "Or a folder holding a file of each part's source file name"}}}}},
         {"required", {"object_id"}}},
        [](const nlohmann::json& params) -> nlohmann::json {
            ReplaceRequest request;
            if (const auto error = read_replace(params, request))
                return error_response(*error);
            return run_on_main_thread([request]() -> nlohmann::json { return replace_on_main_thread(request); });
        }});
}
