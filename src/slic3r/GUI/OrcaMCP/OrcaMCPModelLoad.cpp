// src/slic3r/GUI/OrcaMCP/OrcaMCPModelLoad.cpp
#include "OrcaMCPModelLoad.hpp"
#include "OrcaMCPCommon.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

ThreeMfLoad choose_3mf_load(const std::string& setting, bool scene_has_objects, bool automated)
{
    // Automation first: with "load all", or "ask when relevant" on an empty plate, the setting
    // alone would open the file as a project without asking anyone.
    if (automated || setting == OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY)
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
