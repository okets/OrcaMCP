// src/slic3r/GUI/OrcaMCP/OrcaMCPModelLoad.hpp
#pragma once
#include <cstddef>
#include <set>
#include <string>
#include <nlohmann/json.hpp>

#include "libslic3r/ObjectID.hpp"

namespace Slic3r {
class Model;
class ModelObject;
namespace GUI { namespace OrcaMCP {

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

// The ids of the objects in `model`, taken before a load so the objects it adds can be told apart
// from the ones already there (every load path appends, but compare ids, not counts).
std::set<ObjectID> object_ids(const Model& model);

// load_model's loaded_objects: one entry per object in `model` whose id is not in `before`.
nlohmann::json loaded_objects_json(const Model& model, const std::set<ObjectID>& before);

// One loaded_objects entry: object_id (the scene index other tools take), name, the first
// instance's scale, the bounding-box size in mm (as get_scene_info measures it) and volume_count.
// Add per-object load findings here.
nlohmann::json loaded_object_json(const ModelObject& object, size_t object_id);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
