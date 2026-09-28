// src/slic3r/GUI/OrcaMCP/OrcaMCPSourceFiles.cpp
#include "OrcaMCPSourceFiles.hpp"

#include <boost/filesystem/path.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

std::string object_words(int object_id) { return "object " + std::to_string(object_id); }

const SourceObject* object_at(const std::vector<SourceObject>& scene, int object_id)
{
    return object_id >= 0 && std::size_t(object_id) < scene.size() ? &scene[std::size_t(object_id)] : nullptr;
}

std::string invalid_object(int object_id, std::size_t count)
{
    return "Invalid object_id " + std::to_string(object_id) + ": the scene has " + std::to_string(count) + " objects";
}

std::string invalid_volume(const SourceObject& object, int volume_id)
{
    return "Invalid volume_id " + std::to_string(volume_id) + ": " + object_words(object.object_id) + " has volumes 0.." +
           std::to_string(int(object.volumes.size()) - 1);
}

std::string cut_piece(int object_id, const std::string& action)
{
    return object_words(object_id) + " is a piece of a cut, which the app does not " + action + " (the object list's menu item is off for one)";
}

// The object's volumes the reload takes, (object, volume).
void add_reloadable(const SourceObject& object, std::optional<int> only, std::vector<std::pair<int, int>>& volumes)
{
    for (const SourceVolume& volume : object.volumes)
        if (volume.reloadable && (!only || volume.volume_id == *only))
            volumes.emplace_back(object.object_id, volume.volume_id);
}

} // namespace

ReloadDecision plan_reload(const ReloadRequest& request, const std::vector<SourceObject>& scene)
{
    const auto refuse = [](std::string why) { return ReloadDecision{{}, std::move(why)}; };
    if (request.volume_id && !request.object_id)
        return refuse("volume_id needs the object_id it belongs to");
    if (scene.empty())
        return refuse("The scene has no objects, so there is nothing to reload.");
    ReloadDecision decision;
    if (request.object_id) {
        const SourceObject* object = object_at(scene, *request.object_id);
        if (object == nullptr)
            return refuse(invalid_object(*request.object_id, scene.size()));
        if (request.volume_id && (*request.volume_id < 0 || std::size_t(*request.volume_id) >= object->volumes.size()))
            return refuse(invalid_volume(*object, *request.volume_id));
        if (object->cut)
            return refuse(cut_piece(object->object_id, "reload"));
        add_reloadable(*object, request.volume_id, decision.volumes);
        if (decision.volumes.empty())
            return refuse((request.volume_id ? "volume " + std::to_string(*request.volume_id) + " of " : std::string()) +
                          object_words(object->object_id) +
                          " was not loaded from a file (a shape added in the app, or a file that no longer names it), so there is "
                          "nothing to reload");
        return decision;
    }
    // Reload All, as the app's: every part loaded from a file, a cut piece's too -- the parts the cut went through
    // were made anew and name no file (Cut's add_cut_volume), and a part it left whole reloads as itself.
    for (const SourceObject& object : scene)
        add_reloadable(object, std::nullopt, decision.volumes);
    if (decision.volumes.empty())
        return refuse("No object in the scene was loaded from a file, so there is nothing to reload.");
    return decision;
}

std::string file_names(const std::vector<std::string>& paths)
{
    std::string names;
    for (const std::string& path : paths)
        names += (names.empty() ? "" : ", ") + boost::filesystem::path(path).filename().string();
    return names;
}

std::optional<std::string> reload_sources_refusal(const std::vector<std::string>& missing, const std::optional<std::string>& file_path,
                                                  const std::function<bool(const std::string&)>& is_file)
{
    if (file_path && !is_file(*file_path))
        return "file_path \"" + *file_path + "\" is not a file";
    if (missing.empty()) {
        if (file_path)
            return std::string("Every source file is where it was, so file_path has nothing to find: it is only for a source "
                               "that moved or was deleted. To load another file into a part, use replace_volume_with_file");
        return std::nullopt;
    }
    if (!file_path)
        return "The source file" + std::string(missing.size() > 1 ? "s " : " ") + file_names(missing) +
               (missing.size() > 1 ? " are" : " is") +
               " neither where it was loaded from nor beside the object's own file, so nothing was reloaded: pass file_path with "
               "its new place (a file of that name; its folder is searched for the others), or another file to load into the part";
    return std::nullopt;
}

ReplaceDecision plan_replace(const ReplaceRequest& request, const std::vector<SourceObject>& scene,
                             const std::function<bool(const std::string&)>& is_file, const std::function<bool(const std::string&)>& is_folder)
{
    const auto refuse = [](std::string why) { return ReplaceDecision{{}, std::move(why)}; };
    if (request.file_path.has_value() == request.folder.has_value())
        return refuse("Pass file_path (a file for one part, as Replace 3D file) or folder (every part's file of the same name in "
                      "it, as Replace all with 3D files), one of them");
    const SourceObject* object = object_at(scene, request.object_id);
    if (object == nullptr)
        return refuse(invalid_object(request.object_id, scene.size()));
    if (request.volume_id && (*request.volume_id < 0 || std::size_t(*request.volume_id) >= object->volumes.size()))
        return refuse(invalid_volume(*object, *request.volume_id));
    if (object->cut)
        return refuse(cut_piece(object->object_id, "replace a part of"));

    ReplaceDecision decision;
    if (request.file_path) {
        if (!request.volume_id && object->volumes.size() > 1)
            return refuse(object_words(object->object_id) + " has " + std::to_string(object->volumes.size()) +
                          " volumes, and a file replaces one: pass its volume_id (get_object_info lists them)");
        if (!is_file(*request.file_path))
            return refuse("file_path \"" + *request.file_path + "\" is not a file");
        decision.volume_ids.push_back(request.volume_id.value_or(0));
        return decision;
    }
    if (!is_folder(*request.folder))
        return refuse("folder \"" + *request.folder + "\" is not an existing folder");
    // As Replace all with 3D files: each volume loaded from a file takes the file of that name in the folder,
    // and one whose file is not there, or is the one it was loaded from, is skipped.
    std::vector<std::string> sources;
    for (const SourceVolume& volume : object->volumes) {
        if (volume.source_file.empty() || (request.volume_id && volume.volume_id != *request.volume_id))
            continue;
        sources.push_back(volume.source_file);
        const std::string candidate = (boost::filesystem::path(*request.folder) / boost::filesystem::path(volume.source_file).filename()).string();
        if (is_file(candidate) && boost::filesystem::path(candidate) != boost::filesystem::path(volume.source_file))
            decision.volume_ids.push_back(volume.volume_id);
    }
    if (sources.empty())
        return refuse((request.volume_id ? "volume " + std::to_string(*request.volume_id) + " of " : std::string()) +
                      object_words(object->object_id) +
                      " was not loaded from a file, so a folder has no file of its name to replace it with: pass file_path");
    if (decision.volume_ids.empty())
        return refuse("folder \"" + *request.folder + "\" holds no other file named as " + object_words(object->object_id) +
                      "'s part files (" + file_names(sources) + ")");
    return decision;
}

}}} // namespace Slic3r::GUI::OrcaMCP
