// src/slic3r/GUI/OrcaMCP/OrcaMCPSourceFiles.hpp
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What reload_from_disk and replace_volume_with_file decide before the app loads anything: which volumes
// a call reloads or replaces, and why one is refused. The loads themselves are the object list's own --
// Reload from disk and Reload All (Plater::reload_from_disk, reload_all_from_disk), Replace 3D file and
// Replace all with 3D files (replace_with_stl, replace_all_with_stl) -- whose file and folder dialogs the
// call's path answers (mcp_answer_path_dialog). No wx and no Model: the tests drive it with plain values
// (tests/slic3rutils/test_mcp_source_files.cpp).

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A volume as the reload and the replace read it.
struct SourceVolume
{
    int         volume_id = -1;
    std::string name;
    std::string source_file; // ModelVolume::source.input_file, "" when it was not loaded from a file
    bool        reloadable = false; // is_reloadable_volume: loaded from a file, not a built-in shape
};

struct SourceObject
{
    int                       object_id = -1;
    bool                      cut       = false; // a piece of a cut (ModelObject::is_cut)
    std::vector<SourceVolume> volumes;
};

// ---- reload_from_disk ---------------------------------------------------------------------------

struct ReloadRequest
{
    std::optional<int>         object_id; // nullopt: every object (the plate menu's Reload All)
    std::optional<int>         volume_id;
    std::optional<std::string> file_path; // a moved or deleted source's new place, or another file
};

// The (object, volume) pairs the call reloads, or why it is refused: volume_id without object_id, an
// object or volume out of range, a cut piece (the app's Reload from disk is off for one; Reload All takes
// none), nothing loaded from a file.
struct ReloadDecision
{
    std::vector<std::pair<int, int>> volumes;
    std::string                      refusal; // when volumes is empty
};
ReloadDecision plan_reload(const ReloadRequest& request, const std::vector<SourceObject>& scene);

// Why the reload is refused once the app has looked for the source files (`missing`: those found neither
// where they were loaded from nor beside the object's own file): missing files and no file_path to find
// them by; a file_path that is no file; a file_path given while nothing is missing (it names only a moved
// source; replace_volume_with_file loads another file). Nothing when the reload can go ahead.
std::optional<std::string> reload_sources_refusal(const std::vector<std::string>& missing, const std::optional<std::string>& file_path,
                                                  const std::function<bool(const std::string&)>& is_file);

// The file names in `paths`, for a message.
std::string file_names(const std::vector<std::string>& paths);

// ---- replace_volume_with_file -------------------------------------------------------------------

struct ReplaceRequest
{
    int                        object_id = -1;
    std::optional<int>         volume_id;
    std::optional<std::string> file_path; // one volume, as Replace 3D file
    std::optional<std::string> folder;    // every volume with a source file, as Replace all with 3D files
};

// The volumes the call replaces, or why it is refused: file_path and folder both or neither; an object
// or volume out of range; a cut piece (the app's replace is off for one); no volume_id with file_path on
// an object of several volumes (Replace 3D file takes one); a file or a folder that is not there; with a
// folder, no volume loaded from a file whose file name the folder holds (other than the file itself),
// which Replace all with 3D files would skip.
struct ReplaceDecision
{
    std::vector<int> volume_ids;
    std::string      refusal; // when volume_ids is empty
};
ReplaceDecision plan_replace(const ReplaceRequest& request, const std::vector<SourceObject>& scene,
                             const std::function<bool(const std::string&)>& is_file, const std::function<bool(const std::string&)>& is_folder);

}}} // namespace Slic3r::GUI::OrcaMCP
