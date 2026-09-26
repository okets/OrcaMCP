// src/slic3r/GUI/OrcaMCP/OrcaMCPModelLoad.cpp
#include "OrcaMCPModelLoad.hpp"

#include "libslic3r/AppConfig.hpp"

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

}}} // namespace Slic3r::GUI::OrcaMCP
