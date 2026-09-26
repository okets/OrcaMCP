// src/slic3r/GUI/OrcaMCP/OrcaMCPModelLoad.cpp
#include "OrcaMCPModelLoad.hpp"

#include "OrcaMCPCommon.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

LoadFileKind load_file_kind(const std::string& path)
{
    // Plater::load_files routes .gcode and .g to load_gcode; libslic3r's is_gcode_file knows .gcode.
    if (is_gcode_file(path) || boost::iends_with(path, ".g"))
        return LoadFileKind::GcodePreview;
    if (boost::iends_with(path, ".gcode.3mf"))
        return LoadFileKind::SlicedBundle;
    return LoadFileKind::Model;
}

std::optional<std::string> load_refusal(LoadFileKind kind, bool scene_has_objects, bool scene_is_preview)
{
    if (kind != LoadFileKind::Model && scene_has_objects)
        return std::string("Not loaded: this file is G-code (a .gcode or a sliced .gcode.3mf), and loading it replaces "
                           "the whole scene with a preview of that G-code, discarding the objects on the plate. "
                           "Call save_project first to keep them, then new_project, then load_model again.");
    if (kind == LoadFileKind::Model && scene_is_preview)
        return std::string("Not loaded: the scene is a G-code preview, and objects cannot be added to it. Call "
                           "new_project to go back to an editable scene, then load_model again.");
    return std::nullopt;
}

ThreeMfLoad choose_3mf_load(const std::string& setting, bool scene_has_objects, bool automated, bool sliced_bundle)
{
    // Automation first: with "load all", or "ask when relevant" on an empty plate, the setting
    // alone would open the file as a project without asking anyone. A sliced bundle is the
    // exception: it is only G-code, and a project load is what previews it.
    if (automated)
        return sliced_bundle && !scene_has_objects ? ThreeMfLoad::OpenProject : ThreeMfLoad::ImportGeometry;
    if (setting == OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY)
        return ThreeMfLoad::ImportGeometry;
    if (setting == OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK ||
        (setting == OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT && scene_has_objects))
        return ThreeMfLoad::AskUser;
    return ThreeMfLoad::OpenProject;
}

std::set<ObjectID> object_ids(const Model& model)
{
    std::set<ObjectID> ids;
    for (const ModelObject* object : model.objects)
        ids.insert(object->id());
    return ids;
}

nlohmann::json loaded_objects_json(const Model& model, const std::set<ObjectID>& before)
{
    nlohmann::json loaded = nlohmann::json::array();
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (before.count(model.objects[i]->id()) == 0)
            loaded.push_back(model_object_summary_json(*model.objects[i], static_cast<int>(i)));
    return loaded;
}

}}} // namespace Slic3r::GUI::OrcaMCP
