#include <catch2/catch_test_macros.hpp>

#include <string>

#include "libslic3r/AppConfig.hpp"
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

TEST_CASE("under MCP a 3MF is imported as geometry whatever the setting and the scene", "[orcamcp][load]")
{
    for (const std::string& setting : {load_all, ask_when_relevant, always_ask, load_geometry}) {
        for (bool scene_has_objects : {empty_scene, with_objects}) {
            INFO("setting " << setting << ", scene has objects " << scene_has_objects);
            CHECK(choose_3mf_load(setting, scene_has_objects, mcp) == ThreeMfLoad::ImportGeometry);
        }
    }
}

TEST_CASE("in the GUI the project load behaviour setting decides, as upstream", "[orcamcp][load]")
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

TEST_CASE("an unset or unknown setting opens the project, as upstream's fallthrough does", "[orcamcp][load]")
{
    CHECK(choose_3mf_load("", empty_scene, gui) == ThreeMfLoad::OpenProject);
    CHECK(choose_3mf_load("something_new", with_objects, gui) == ThreeMfLoad::OpenProject);
    CHECK(choose_3mf_load("", with_objects, mcp) == ThreeMfLoad::ImportGeometry);
}
