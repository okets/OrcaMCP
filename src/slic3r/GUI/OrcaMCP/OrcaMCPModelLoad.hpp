// src/slic3r/GUI/OrcaMCP/OrcaMCPModelLoad.hpp
#pragma once
#include <string>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// How a 3MF imported into the scene is loaded: as a project (its embedded printer, filament and
// process presets become active, the scene is reset and the project takes the file's name), as
// geometry appended to the scene, or by asking the user with the ProjectDropDialog.
enum class ThreeMfLoad { OpenProject, ImportGeometry, AskUser };

// The decision Plater::open_3mf_file makes for one 3MF, as a pure function.
// `setting` is the app's project_load_behaviour (load_all, ask_when_relevant, always_ask,
// load_geometry_only), `scene_has_objects` whether the plater already holds objects, and
// `automated` whether MCP dialog suppression is on. Under MCP the answer is always geometry: the
// tool that reaches this is load_model, and opening a project instead would silently swap the
// user's presets and rename the project. load_project opens a 3MF as a project on its own path.
ThreeMfLoad choose_3mf_load(const std::string& setting, bool scene_has_objects, bool automated);

}}} // namespace Slic3r::GUI::OrcaMCP
