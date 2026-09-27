#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp"
#include "libslic3r/TriangleMesh.hpp"

// remap_paint and paint_object's selection "state": renumbering painted filaments. A Blender export
// numbers a 3-colour model 1-3, and keeping slot 1 free for a support filament used to take a
// hidden triangle painted in Blender. The renumbering must be simultaneous -- {1: 2, 2: 3} moves 1 to
// 2 and 2 to 3, never 1 on to 3 -- or it merges two colours into one.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// A 12-facet cube: facets 0-3 painted filament 1, 4-7 filament 2, 8-11 filament 3.
struct ThreeColourCube
{
    TriangleMesh mesh{its_make_cube(10.0, 10.0, 10.0)};
    PaintData    paint;

    ThreeColourCube()
    {
        std::vector<int> states(mesh.its.indices.size());
        for (size_t i = 0; i < states.size(); ++i)
            states[i] = 1 + int(i) / 4;
        PaintWrite write;
        REQUIRE(build_paint_write(mesh, PaintMode::Color, states, /*replace=*/true, PaintData{}, write));
        paint = std::move(write.data);
    }
};

// {state: facet count} out of a summary.
std::map<int, int> counts(const std::vector<PaintedStateInfo>& painted)
{
    std::map<int, int> by_state;
    for (const PaintedStateInfo& info : painted)
        by_state[info.state] = info.facet_count;
    return by_state;
}

} // namespace

TEST_CASE("a renumbering moves every painted filament at once, never chaining one into the next", "[orcamcp][PaintRemap]")
{
    ThreeColourCube cube;
    PaintRemapWrite remapped;
    REQUIRE(build_remap_write(cube.mesh, cube.paint, {{1, 2}, {2, 3}, {3, 4}}, remapped));

    CHECK(counts(remapped.before) == std::map<int, int>{{1, 4}, {2, 4}, {3, 4}});
    CHECK(counts(remapped.after) == std::map<int, int>{{2, 4}, {3, 4}, {4, 4}});
}

TEST_CASE("a filament the mapping does not list keeps its facets", "[orcamcp][PaintRemap]")
{
    ThreeColourCube cube;
    PaintRemapWrite remapped;
    REQUIRE(build_remap_write(cube.mesh, cube.paint, {{1, 5}}, remapped));
    CHECK(counts(remapped.after) == std::map<int, int>{{2, 4}, {3, 4}, {5, 4}});
}

TEST_CASE("two filaments mapped to one merge, and the facets are all kept", "[orcamcp][PaintRemap]")
{
    ThreeColourCube cube;
    PaintRemapWrite remapped;
    REQUIRE(build_remap_write(cube.mesh, cube.paint, {{2, 3}}, remapped));
    CHECK(counts(remapped.after) == std::map<int, int>{{1, 4}, {3, 8}});
}

TEST_CASE("mapping a filament to 0 unpaints its facets", "[orcamcp][PaintRemap]")
{
    ThreeColourCube cube;
    PaintRemapWrite remapped;
    REQUIRE(build_remap_write(cube.mesh, cube.paint, {{3, 0}}, remapped));
    CHECK(counts(remapped.after) == std::map<int, int>{{0, 4}, {1, 4}, {2, 4}});
}

TEST_CASE("mapping from 0 paints the bare facets of a volume that carries no paint yet", "[orcamcp][PaintRemap]")
{
    const TriangleMesh mesh(its_make_cube(10.0, 10.0, 10.0));
    PaintRemapWrite    remapped;
    REQUIRE(build_remap_write(mesh, PaintData{}, {{0, 3}}, remapped));
    CHECK(counts(remapped.before) == std::map<int, int>{{0, 12}});
    CHECK(counts(remapped.after) == std::map<int, int>{{3, 12}});
}

TEST_CASE("the written paint reads back as the after summary", "[orcamcp][PaintRemap]")
{
    ThreeColourCube cube;
    PaintRemapWrite remapped;
    REQUIRE(build_remap_write(cube.mesh, cube.paint, {{1, 2}, {2, 3}, {3, 4}}, remapped));
    CHECK(counts(summarize_paint_data(cube.mesh, remapped.data)) == counts(remapped.after));
}

TEST_CASE("a remap is refused for a mesh with no facets or a state no facet can hold", "[orcamcp][PaintRemap]")
{
    ThreeColourCube cube;
    PaintRemapWrite remapped;
    CHECK_FALSE(build_remap_write(TriangleMesh(), PaintData{}, {{1, 2}}, remapped));
    CHECK_FALSE(build_remap_write(cube.mesh, cube.paint, {{1, max_paint_state() + 1}}, remapped));
    CHECK_FALSE(build_remap_write(cube.mesh, cube.paint, {{-1, 2}}, remapped));
}

TEST_CASE("a new colour must be a real filament slot, or 0 to unpaint", "[orcamcp][PaintRemap]")
{
    CHECK(state_mapping_error(PaintMode::Color, {{1, 2}, {2, 3}, {3, 4}}, /*slot_count=*/4).empty());
    CHECK(state_mapping_error(PaintMode::Color, {{1, 0}}, 4).empty());
    CHECK(state_mapping_error(PaintMode::Color, {{1, 5}}, 4).find("out of range 1..4") != std::string::npos);
}

TEST_CASE("an old colour may be any state a facet holds, so a stale one can be moved off", "[orcamcp][PaintRemap]")
{
    CHECK(state_mapping_error(PaintMode::Color, {{9, 1}}, /*slot_count=*/4).empty());
    CHECK(state_mapping_error(PaintMode::Color, {{max_paint_state() + 1, 1}}, 4).find("cannot be remapped") != std::string::npos);
}

TEST_CASE("an empty mapping is refused", "[orcamcp][PaintRemap]")
{
    CHECK(state_mapping_error(PaintMode::Color, {}, 4).find("empty") != std::string::npos);
}

TEST_CASE("a support state can only become none, enforcer or blocker", "[orcamcp][PaintRemap]")
{
    CHECK(state_mapping_error(PaintMode::Support, {{1, 2}}, 4).empty());
    CHECK_FALSE(state_mapping_error(PaintMode::Support, {{1, 3}}, 4).empty());
}

TEST_CASE("a filament slot is paintable when it exists and a facet state can hold it", "[orcamcp][PaintRemap]")
{
    CHECK(color_slot_error(0, 4).empty());
    CHECK(color_slot_error(4, 4).empty());
    CHECK(color_slot_error(5, 4).find("out of range 1..4") != std::string::npos);
    CHECK(color_slot_error(max_paint_state() + 1, 40).find("cannot be painted") != std::string::npos);
}

TEST_CASE("a part whose own filament is moved away is told the call that moves its unpainted facets too", "[orcamcp][PaintRemap]")
{
    const std::optional<std::string> note =
        unpainted_filament_note(/*object_id=*/0, /*volume_id=*/0, /*part_has_own_filament=*/false, /*object_has_one_part=*/true,
                                /*part_filament=*/1, /*unpainted_facets=*/120, {{1, 2}, {2, 3}, {3, 4}});
    REQUIRE(note.has_value());
    CHECK(note->find("unpainted facets still print with filament 1") != std::string::npos);
    CHECK(note->find("call set_object_filament {object_id: 0, filament: 2} to move them too") != std::string::npos);
}

TEST_CASE("a part of a multi-part object is told the volume form, which leaves the other parts alone", "[orcamcp][PaintRemap]")
{
    const std::optional<std::string> note = unpainted_filament_note(3, 1, /*part_has_own_filament=*/false, /*object_has_one_part=*/false,
                                                                    1, 50, {{1, 2}});
    REQUIRE(note.has_value());
    CHECK(note->find("set_object_filament {object_id: 3, volume_id: 1, filament: 2}") != std::string::npos);
}

TEST_CASE("a part with its own filament slot is told the volume form", "[orcamcp][PaintRemap]")
{
    const std::optional<std::string> note = unpainted_filament_note(0, 2, /*part_has_own_filament=*/true, /*object_has_one_part=*/true,
                                                                    3, 50, {{3, 4}});
    REQUIRE(note.has_value());
    CHECK(note->find("set_object_filament {object_id: 0, volume_id: 2, filament: 4}") != std::string::npos);
}

TEST_CASE("no note when nothing is unpainted, or the part's filament is not moved", "[orcamcp][PaintRemap]")
{
    CHECK_FALSE(unpainted_filament_note(0, 0, false, true, /*part_filament=*/1, /*unpainted_facets=*/0, {{1, 2}}).has_value());
    CHECK_FALSE(unpainted_filament_note(0, 0, false, true, /*part_filament=*/1, 120, {{2, 3}}).has_value());
    CHECK_FALSE(unpainted_filament_note(0, 0, false, true, /*part_filament=*/1, 120, {{1, 0}}).has_value());
}

TEST_CASE("a mapping is read from old-filament keys to new-filament numbers", "[orcamcp][PaintRemap]")
{
    PaintStateMap mapping;
    std::string   error;
    REQUIRE(parse_state_mapping(nlohmann::json{{"1", 2}, {"2", "3"}, {"3", 4.0}}, mapping, error));
    CHECK(mapping == PaintStateMap{{1, 2}, {2, 3}, {3, 4}});
}

TEST_CASE("a mapping whose keys or values are not whole numbers is refused, naming the entry", "[orcamcp][PaintRemap]")
{
    PaintStateMap mapping;
    std::string   error;
    CHECK_FALSE(parse_state_mapping(nlohmann::json{{"one", 2}}, mapping, error));
    CHECK(error.find("\"one\"") != std::string::npos);
    CHECK_FALSE(parse_state_mapping(nlohmann::json{{"1", "two"}}, mapping, error));
    CHECK(error.find("\"1\"") != std::string::npos);
    CHECK_FALSE(parse_state_mapping(nlohmann::json{{"-1", 2}}, mapping, error));
    CHECK_FALSE(parse_state_mapping(nlohmann::json::array({1, 2}), mapping, error));
}

// ---- the state a paint call names: one reader for `filament`/`state` and `match_filament`/`match_state`

TEST_CASE("the filament to paint with must be an existing slot, or 0", "[orcamcp][PaintRemap]")
{
    int         state = -1;
    std::string error;
    REQUIRE(parse_state_param(nlohmann::json{{"filament", 3}}, PaintMode::Color, k_paint_with, /*slot_count=*/4, state, error));
    CHECK(state == 3);
    CHECK_FALSE(parse_state_param(nlohmann::json{{"filament", 6}}, PaintMode::Color, k_paint_with, 4, state, error));
    CHECK(error.find("out of range 1..4") != std::string::npos);
}

TEST_CASE("the filament whose facets to repaint may be a stale one no slot has", "[orcamcp][PaintRemap]")
{
    int         state = -1;
    std::string error;
    REQUIRE(parse_state_param(nlohmann::json{{"match_filament", "9"}}, PaintMode::Color, k_repaint_from, /*slot_count=*/4, state, error));
    CHECK(state == 9);
    CHECK_FALSE(parse_state_param(nlohmann::json{{"match_filament", -1}}, PaintMode::Color, k_repaint_from, 4, state, error));
    CHECK(error.find("match_filament") != std::string::npos);
}

TEST_CASE("a missing or malformed state parameter is named in the message, with what it is for", "[orcamcp][PaintRemap]")
{
    int         state = -1;
    std::string error;
    CHECK_FALSE(parse_state_param(nlohmann::json::object(), PaintMode::Color, k_repaint_from, 4, state, error));
    CHECK(error.find("match_filament") != std::string::npos);
    CHECK(error.find("whose facets to repaint") != std::string::npos);

    CHECK_FALSE(parse_state_param(nlohmann::json{{"filament", "two"}}, PaintMode::Color, k_paint_with, 4, state, error));
    CHECK(error.find("filament must be a whole number") != std::string::npos);

    CHECK_FALSE(parse_state_param(nlohmann::json::object(), PaintMode::Support, k_paint_with, 4, state, error));
    CHECK(error.find("needs state") != std::string::npos);
    CHECK(error.find("to paint with") != std::string::npos);
}

TEST_CASE("outside colour mode the state is a name, read the same way for either parameter", "[orcamcp][PaintRemap]")
{
    int         state = -1;
    std::string error;
    REQUIRE(parse_state_param(nlohmann::json{{"match_state", "enforcer"}}, PaintMode::Support, k_repaint_from, 4, state, error));
    CHECK(state == 1);
    REQUIRE(parse_state_param(nlohmann::json{{"state", "blocker"}}, PaintMode::Seam, k_paint_with, 4, state, error));
    CHECK(state == 2);
    CHECK_FALSE(parse_state_param(nlohmann::json{{"match_state", "blocker"}}, PaintMode::FuzzySkin, k_repaint_from, 4, state, error));
    CHECK(error.find("Unknown match_state for mode 'fuzzy_skin'") != std::string::npos);
    CHECK_FALSE(parse_state_param(nlohmann::json{{"state", 1}}, PaintMode::Support, k_paint_with, 4, state, error));
    CHECK(error.find("needs state") != std::string::npos);
}
