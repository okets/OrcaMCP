// src/slic3r/GUI/OrcaMCP/OrcaMCPModelLoad.hpp
#pragma once
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <nlohmann/json.hpp>

#include "libslic3r/ObjectID.hpp"

namespace Slic3r {
class Model;
namespace GUI { namespace OrcaMCP {

// What loading a file does to the scene. A model file adds objects. A G-code file (.gcode, .g)
// and a sliced 3MF bundle (.gcode.3mf) replace the whole scene with a preview of their G-code:
// the plater drops its objects and the project takes the file's name.
enum class LoadFileKind { Model, GcodePreview, SlicedBundle };
LoadFileKind load_file_kind(const std::string& path);

// Why load_model must not load a file of `kind` into the current scene, or nothing when it may.
// A preview load would discard the objects already there; objects cannot be added to a scene that
// is a G-code preview. The reason tells the agent what to call instead.
std::optional<std::string> load_refusal(LoadFileKind kind, bool scene_has_objects, bool scene_is_preview);

// How a 3MF imported into the scene is loaded: as a project (its embedded printer, filament and
// process presets become active, the scene is reset and the project takes the file's name), as
// geometry appended to the scene, or by asking the user with the ProjectDropDialog.
enum class ThreeMfLoad { OpenProject, ImportGeometry, AskUser };

// The decision Plater::open_3mf_file makes for one 3MF, as a pure function.
// `setting` is the app's project_load_behaviour (load_all, ask_when_relevant, always_ask,
// load_geometry_only), `scene_has_objects` whether the plater already holds objects, and
// `automated` whether MCP dialog suppression is on. Under MCP a model 3MF is always geometry: the
// tool that reaches this is load_model, and opening a project instead would silently swap the
// user's presets and rename the project. A sliced bundle has no geometry to import, only G-code to
// preview, so under MCP it opens as a project, and only onto an empty scene (load_refusal).
ThreeMfLoad choose_3mf_load(const std::string& setting, bool scene_has_objects, bool automated,
                            bool sliced_bundle = false);

// Whether a 3MF carries print settings of its own (a project config: BambuStudio/Orca's
// Metadata/project_settings.config or PrusaSlicer's Metadata/Slic3r_PE.config). False for a
// geometry-only 3MF and for anything that is not a readable zip.
bool threemf_carries_presets(const std::string& path);

// What load_model says after importing a 3MF's geometry.
std::string threemf_import_message(bool carries_presets);

// The ids of the objects in `model`, taken before a load so the objects it adds can be told apart
// from the ones already there (every load path appends, but compare ids, not counts).
std::set<ObjectID> object_ids(const Model& model);

// load_model's loaded_objects: one model_object_summary_json (OrcaMCPCommon.hpp) per object in
// `model` whose id is not in `before`, so an object reads the same here as in get_scene_info.
nlohmann::json loaded_objects_json(const Model& model, const std::set<ObjectID>& before);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
