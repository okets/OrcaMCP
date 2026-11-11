#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/OrcaMCP/OrcaMCPPhysicalSegments.hpp"
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using nlohmann::json;

namespace {
std::vector<GcodeMove> split_wall()
{
    std::vector<GcodeMove> moves(13);
    for (size_t i = 0; i < moves.size(); ++i) {
        auto& m = moves[i];
        m.position = Vec3f(float(i), 0.f, 0.2f);
        m.type = i ? EMoveType::Extrude : EMoveType::Travel;
        m.extrusion_role = erExternalPerimeter;
        m.internal_only = i % 3 != 0;
        m.width = m.internal_only ? float(i) * 0.01f : 0.4f;
        m.height = m.internal_only ? float(i) * 0.01f : 0.2f;
    }
    return moves;
}
json physical_page(const std::vector<GcodeMove>& moves, const json& params)
{
    return layer_geometry_page(moves, physical_gcode_layers(moves), parse_layer_geometry_request(params),
        {"session", "plate", 0, 123, "app", "server", nullptr}, 1);
}
}

TEST_CASE("Physical traversal skips interpolated copies and keeps the last physical endpoint", "[PhysicalSegments]")
{
    auto moves = split_wall();
    std::vector<size_t> previous, indices;
    visit_physical_layer_segments(moves, gcode_layers(moves)[0], [&](const PhysicalSegment& s) {
        previous.push_back(s.previous_index); indices.push_back(s.index);
        return true;
    });
    CHECK(indices == std::vector<size_t>{3, 6, 9, 12});
    CHECK(previous == std::vector<size_t>{0, 3, 6, 9});
}

TEST_CASE("Physical layer selection retains raw spans but ignores internal endpoint heights", "[PhysicalSegments]")
{
    auto moves = split_wall();
    moves.back().internal_only = true;
    moves.back().position.z() = 99.f;
    auto legacy = gcode_layers(moves), physical = physical_gcode_layers(moves);
    REQUIRE(physical.size() == legacy.size());
    CHECK(physical[0].begin == legacy[0].begin);
    CHECK(physical[0].end == legacy[0].end);
    CHECK(json(physical[0].z) == json(double(moves[9].position.z())));
    CHECK(json(legacy[0].z) == json(99.));
}

TEST_CASE("Geometry pages preserve physical runs across arbitrary raw-index gaps", "[PhysicalSegments]")
{
    auto moves = split_wall();
    json p = {{"plate_index", 0}, {"layer", 1}, {"max_points", 3}};
    auto first = physical_page(moves, p);
    REQUIRE(first["paths"].size() == 1);
    CHECK(first["paths"][0]["start_move"] == 3);
    CHECK(first["paths"][0]["points"].size() == 3);
    CHECK(first["paths"][0]["width_mm"] == double(moves[3].width));
    CHECK(first["next_cursor"]["next_move"] == 9);
    p["cursor"] = first["next_cursor"];
    auto second = physical_page(moves, p);
    CHECK(second["paths"][0]["continues"] == true);
    CHECK(second["paths"][0]["start_move"] == 9);
    CHECK(second["paths"][0]["points"][0][0] == 6.);
    CHECK(second["has_more"] == false);
}

TEST_CASE("Physical predecessor flags outside the raw layer span fence geometry cursors", "[PhysicalSegments]")
{
    auto moves = split_wall();
    for (size_t i = 4; i < moves.size(); ++i) moves[i].layer_id = 1;
    json p = {{"plate_index", 0}, {"layer", 2}, {"max_points", 2}};
    auto first = physical_page(moves, p);
    CHECK(first["paths"][0]["points"][0][0] == 3.);
    p["cursor"] = first["next_cursor"];
    moves[3].internal_only = true; // Nominal layer begins at 4, predecessor now becomes dummy 0.
    try { physical_page(moves, p); FAIL("Expected stale_slice"); }
    catch (const GeometryError& e) { CHECK(e.code == "stale_slice"); }
    auto no_cursor = p; no_cursor.erase("cursor");
    auto changed = physical_page(moves, no_cursor);
    CHECK(changed["layer_digest"] != first["layer_digest"]);
    CHECK(changed["paths"][0]["points"][0][0] == 0.);
}
