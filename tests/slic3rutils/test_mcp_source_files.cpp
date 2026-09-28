#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPSourceFiles.hpp"

// What reload_from_disk and replace_volume_with_file decide before the app loads anything, and where the
// object list's Reload from disk finds a part's file (reload_sources, which the app and the tool share).

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

namespace fs = boost::filesystem;

SourceVolume from_file(int id, const std::string& file) { return {id, fs::path(file).filename().string(), file, true}; }
SourceVolume built_in(int id) { return {id, "Cube", "", false}; }

SourceObject object(int id, std::vector<SourceVolume> volumes, bool cut = false) { return {id, cut, std::move(volumes)}; }

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool mentions(const std::optional<std::string>& text, const std::string& part) { return text && mentions(*text, part); }

bool no_file(const std::string&) { return false; }
bool any_file(const std::string&) { return true; }

// A folder of the test's own, removed with it.
struct TempFolder
{
    fs::path path = fs::temp_directory_path() / fs::unique_path("mcp-source-files-%%%%-%%%%");
    TempFolder() { fs::create_directories(path); }
    ~TempFolder() { fs::remove_all(path); }
    std::string file(const std::string& name) const
    {
        const fs::path file = path / name;
        std::ofstream(file.string()) << "solid x\nendsolid x\n";
        return file.string();
    }
};

} // namespace

// ---- reload_from_disk ---------------------------------------------------------------------------

TEST_CASE("reload_from_disk reloads an object's parts loaded from a file, not its built-in shapes", "[McpSourceFiles][orcamcp]")
{
    const std::vector<SourceObject> scene = {object(0, {from_file(0, "/m/a.stl"), built_in(1)}), object(1, {from_file(0, "/m/b.stl")})};
    ReloadRequest one;
    one.object_id = 0;
    CHECK(plan_reload(one, scene).volumes == std::vector<std::pair<int, int>>{{0, 0}});
    // Without object_id: every object, as Reload All.
    CHECK(plan_reload(ReloadRequest{}, scene).volumes == std::vector<std::pair<int, int>>{{0, 0}, {1, 0}});
    one.volume_id = 1;
    CHECK(mentions(plan_reload(one, scene).refusal, "was not loaded from a file"));
}

TEST_CASE("reload_from_disk refuses a cut piece, and Reload All while one is in the scene", "[McpSourceFiles][orcamcp]")
{
    const std::vector<SourceObject> scene = {object(0, {from_file(0, "/m/a.stl")}, /*cut=*/true), object(1, {from_file(0, "/m/b.stl")})};
    ReloadRequest cut;
    cut.object_id = 0;
    CHECK(mentions(plan_reload(cut, scene).refusal, "piece of a cut"));
    CHECK(mentions(plan_reload(ReloadRequest{}, scene).refusal, "object 0"));
    ReloadRequest other;
    other.object_id = 1;
    CHECK(plan_reload(other, scene).volumes.size() == 1);
}

TEST_CASE("reload_from_disk refuses a volume without its object, and ids out of range", "[McpSourceFiles][orcamcp]")
{
    const std::vector<SourceObject> scene = {object(0, {from_file(0, "/m/a.stl")})};
    ReloadRequest volume_only;
    volume_only.volume_id = 0;
    CHECK(mentions(plan_reload(volume_only, scene).refusal, "needs the object_id"));
    ReloadRequest far;
    far.object_id = 4;
    CHECK(mentions(plan_reload(far, scene).refusal, "Invalid object_id 4"));
    far.object_id = 0;
    far.volume_id = 2;
    CHECK(mentions(plan_reload(far, scene).refusal, "Invalid volume_id 2"));
    CHECK(mentions(plan_reload(ReloadRequest{}, {}).refusal, "no objects"));
}

TEST_CASE("a missing source needs file_path, and file_path needs a missing source", "[McpSourceFiles][orcamcp]")
{
    CHECK(mentions(reload_sources_refusal({"/old/a.stl", "/old/b.stl"}, std::nullopt, any_file), "a.stl, b.stl are neither"));
    CHECK_FALSE(reload_sources_refusal({"/old/a.stl"}, std::string("/new/a.stl"), any_file));
    CHECK(mentions(reload_sources_refusal({"/old/a.stl"}, std::string("/new/a.stl"), no_file), "is not a file"));
    CHECK(mentions(reload_sources_refusal({}, std::string("/new/a.stl"), any_file), "replace_volume_with_file"));
    CHECK_FALSE(reload_sources_refusal({}, std::nullopt, any_file));
}

TEST_CASE("Reload from disk finds a part's file where it was loaded from, else beside its object's file", "[McpSourceFiles][orcamcp]")
{
    TempFolder here;
    TempFolder moved;
    Model      model;
    ModelObject* object = model.add_object();
    object->input_file  = (moved.path / "project.3mf").string();
    for (const char* name : {"found.stl", "beside.stl", "gone.stl"}) {
        ModelVolume* volume       = object->add_volume(TriangleMesh(its_make_cube(1., 1., 1.)));
        volume->source.input_file = (here.path / name).string();
    }
    here.file("found.stl");                 // where it was loaded from
    const std::string beside = moved.file("beside.stl"); // not there any more, but beside the object's file

    const ReloadSources sources = reload_sources(model, {{0, 0}, {0, 1}, {0, 2}});
    REQUIRE(sources.input_paths.size() == 2);
    CHECK(sources.input_paths[0] == here.path / "found.stl");
    CHECK(sources.input_paths[1] == fs::path(beside));
    REQUIRE(sources.missing.size() == 1);
    CHECK(sources.missing[0].filename() == "gone.stl");
}

TEST_CASE("a part from a file with an extension is reloadable, a built-in shape or a nameless source is not", "[McpSourceFiles][orcamcp]")
{
    Model        model;
    ModelObject* object = model.add_object();
    ModelVolume* file   = object->add_volume(TriangleMesh(its_make_cube(1., 1., 1.)));
    file->source.input_file = "/m/a.stl";
    CHECK(is_reloadable_volume(*file));
    file->source.is_from_builtin_objects = true;
    CHECK_FALSE(is_reloadable_volume(*file));
    ModelVolume* nameless = object->add_volume(TriangleMesh(its_make_cube(1., 1., 1.)));
    nameless->source.input_file = "/m/noextension";
    CHECK_FALSE(is_reloadable_volume(*nameless));
}

// ---- replace_volume_with_file -------------------------------------------------------------------

TEST_CASE("a file replaces one part: volume_id is needed on an object of several", "[McpSourceFiles][orcamcp]")
{
    const std::vector<SourceObject> scene = {object(0, {from_file(0, "/m/a.stl")}), object(1, {from_file(0, "/m/a.stl"), from_file(1, "/m/b.stl")})};
    ReplaceRequest one;
    one.object_id = 0;
    one.file_path = "/new/c.stl";
    CHECK(plan_replace(one, scene, any_file, no_file).volume_ids == std::vector<int>{0});
    CHECK(mentions(plan_replace(one, scene, no_file, no_file).refusal, "is not a file"));
    ReplaceRequest several = one;
    several.object_id      = 1;
    CHECK(mentions(plan_replace(several, scene, any_file, no_file).refusal, "pass its volume_id"));
    several.volume_id = 1;
    CHECK(plan_replace(several, scene, any_file, no_file).volume_ids == std::vector<int>{1});
}

TEST_CASE("a folder replaces the parts whose file of the same name it holds", "[McpSourceFiles][orcamcp]")
{
    TempFolder folder;
    folder.file("b.stl");
    const std::vector<SourceObject> scene = {object(0, {from_file(0, "/m/a.stl"), from_file(1, "/m/b.stl"), built_in(2)})};
    const auto is_file = [](const std::string& path) { return fs::is_regular_file(path); };
    const auto is_dir  = [](const std::string& path) { return fs::is_directory(path); };
    ReplaceRequest request;
    request.object_id = 0;
    request.folder    = folder.path.string();
    CHECK(plan_replace(request, scene, is_file, is_dir).volume_ids == std::vector<int>{1});
    request.volume_id = 0;
    CHECK(mentions(plan_replace(request, scene, is_file, is_dir).refusal, "holds no other file"));
    request.volume_id = 2;
    CHECK(mentions(plan_replace(request, scene, is_file, is_dir).refusal, "was not loaded from a file"));
    request.volume_id.reset();
    request.folder = (folder.path / "not-there").string();
    CHECK(mentions(plan_replace(request, scene, is_file, is_dir).refusal, "not an existing folder"));
}

TEST_CASE("replace_volume_with_file takes a file or a folder, and refuses a cut piece", "[McpSourceFiles][orcamcp]")
{
    const std::vector<SourceObject> scene = {object(0, {from_file(0, "/m/a.stl")}, /*cut=*/true)};
    ReplaceRequest neither;
    neither.object_id = 0;
    CHECK(mentions(plan_replace(neither, scene, any_file, any_file).refusal, "one of them"));
    ReplaceRequest both = neither;
    both.file_path      = "/n/a.stl";
    both.folder         = "/n";
    CHECK(mentions(plan_replace(both, scene, any_file, any_file).refusal, "one of them"));
    ReplaceRequest cut = neither;
    cut.file_path      = "/n/a.stl";
    CHECK(mentions(plan_replace(cut, scene, any_file, any_file).refusal, "piece of a cut"));
}
