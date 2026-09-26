#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPModelLoad.hpp"

// What load_model does with a 3MF. On 2026-09-26 a load_model of a Blender-exported 3MF onto an
// empty scene opened it as a project: the file's embedded Bambu A1 presets replaced the user's
// Flashforge ones, their unsaved edits were discarded and the project took the file's name, so a
// later save_project {} overwrote the user's export. The same call on a non-empty scene imported
// geometry only. The decision now lives in choose_3mf_load, which Plater::open_3mf_file calls.

using Slic3r::GUI::OrcaMCP::choose_3mf_load;
using Slic3r::GUI::OrcaMCP::ThreeMfLoad;

namespace {
const std::string load_all          = OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL;
const std::string ask_when_relevant = OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT;
const std::string always_ask        = OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK;
const std::string load_geometry     = OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY;

constexpr bool empty_scene = false;
constexpr bool with_objects = true;
constexpr bool gui = false;
constexpr bool mcp = true;
} // namespace

TEST_CASE("under MCP a 3MF is imported as geometry whatever the setting and the scene", "[McpModelLoad][orcamcp][load]")
{
    for (const std::string& setting : {load_all, ask_when_relevant, always_ask, load_geometry}) {
        for (bool scene_has_objects : {empty_scene, with_objects}) {
            INFO("setting " << setting << ", scene has objects " << scene_has_objects);
            CHECK(choose_3mf_load(setting, scene_has_objects, mcp) == ThreeMfLoad::ImportGeometry);
        }
    }
}

TEST_CASE("in the GUI the project load behaviour setting decides, as upstream", "[McpModelLoad][orcamcp][load]")
{
    CHECK(choose_3mf_load(load_all, empty_scene, gui) == ThreeMfLoad::OpenProject);
    CHECK(choose_3mf_load(load_all, with_objects, gui) == ThreeMfLoad::OpenProject);

    // "Ask when relevant": relevant means there is something on the plate to lose.
    CHECK(choose_3mf_load(ask_when_relevant, empty_scene, gui) == ThreeMfLoad::OpenProject);
    CHECK(choose_3mf_load(ask_when_relevant, with_objects, gui) == ThreeMfLoad::AskUser);

    CHECK(choose_3mf_load(always_ask, empty_scene, gui) == ThreeMfLoad::AskUser);
    CHECK(choose_3mf_load(always_ask, with_objects, gui) == ThreeMfLoad::AskUser);

    CHECK(choose_3mf_load(load_geometry, empty_scene, gui) == ThreeMfLoad::ImportGeometry);
    CHECK(choose_3mf_load(load_geometry, with_objects, gui) == ThreeMfLoad::ImportGeometry);
}

TEST_CASE("an unset or unknown setting opens the project, as upstream's fallthrough does", "[McpModelLoad][orcamcp][load]")
{
    CHECK(choose_3mf_load("", empty_scene, gui) == ThreeMfLoad::OpenProject);
    CHECK(choose_3mf_load("something_new", with_objects, gui) == ThreeMfLoad::OpenProject);
    CHECK(choose_3mf_load("", with_objects, mcp) == ThreeMfLoad::ImportGeometry);
}

// What load_model returns about the objects it added. The response used to say only "success", so
// an agent could not tell that a 3MF's four objects had been merged into one, or that a model had
// been scaled by 0.00328 to fit the bed, without asking get_scene_info and guessing which was new.
namespace {
using Slic3r::Model;
using Slic3r::ModelObject;
using Slic3r::GUI::OrcaMCP::loaded_objects_json;
using Slic3r::GUI::OrcaMCP::model_object_summary_json;
using Slic3r::GUI::OrcaMCP::object_ids;
using Catch::Matchers::WithinAbs;

ModelObject* add_box(Model& model, const std::string& name, double scale = 1.0, int volumes = 1)
{
    ModelObject* object = model.add_object();
    object->name        = name;
    for (int i = 0; i < volumes; ++i)
        object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(10.0, 20.0, 30.0)), false);
    object->add_instance()->set_scaling_factor(Slic3r::Vec3d(scale, scale, scale));
    object->invalidate_bounding_box();
    return object;
}
} // namespace

TEST_CASE("loaded_objects names only the objects the load added, by their scene index", "[McpModelLoad][orcamcp][load]")
{
    Model model;
    add_box(model, "already here");
    add_box(model, "also here");
    const auto before = object_ids(model);

    add_box(model, "Kuromi one piece", 0.5, 4);

    const nlohmann::json loaded = loaded_objects_json(model, before);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0]["object_index"] == 2);
    CHECK(loaded[0]["name"] == "Kuromi one piece");
    CHECK(loaded[0]["volume_count"] == 4);
    CHECK_THAT(loaded[0]["scale"]["x"].get<double>(), WithinAbs(0.5, 1e-9));
    CHECK_THAT(loaded[0]["scale"]["z"].get<double>(), WithinAbs(0.5, 1e-9));
    CHECK_THAT(loaded[0]["bounding_box"]["size_x"].get<double>(), WithinAbs(5.0, 1e-9));
    CHECK_THAT(loaded[0]["bounding_box"]["size_y"].get<double>(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(loaded[0]["bounding_box"]["size_z"].get<double>(), WithinAbs(15.0, 1e-9));
}

// get_scene_info and load_model describe an object with one serializer, so the same object reads
// the same in both: an agent that learned get_scene_info's object_index and bounding_box.size_x
// finds them under those names in loaded_objects too.
TEST_CASE("a loaded object reads exactly as get_scene_info's summary of it", "[McpModelLoad][orcamcp][load]")
{
    Model model;
    const auto before = object_ids(model);
    ModelObject* added = add_box(model, "part", 2.0, 1);

    const nlohmann::json loaded  = loaded_objects_json(model, before);
    const nlohmann::json summary = model_object_summary_json(*added, 0);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0] == summary);
    for (const char* field : {"id", "name", "object_index", "instance_count", "volume_count", "position",
                              "rotation_degrees", "scale", "bounding_box"})
        CHECK(summary.contains(field));
}

TEST_CASE("a load that added nothing reports no loaded objects", "[McpModelLoad][orcamcp][load]")
{
    Model model;
    add_box(model, "already here");
    CHECK(loaded_objects_json(model, object_ids(model)).empty());
    CHECK(loaded_objects_json(Model(), {}).empty());
}
