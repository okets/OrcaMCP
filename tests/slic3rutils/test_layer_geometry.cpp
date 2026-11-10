#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/OrcaMCP/OrcaMCPLayerGeometry.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPToolArguments.hpp"
#include <limits>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using nlohmann::json;

namespace {
std::vector<GcodeMove> wall(size_t segments)
{
    std::vector<GcodeMove> moves(segments + 1);
    for (size_t i = 0; i < moves.size(); ++i) {
        auto& m = moves[i];
        m.type = i ? EMoveType::Extrude : EMoveType::Travel;
        m.extrusion_role = erExternalPerimeter;
        m.position = Vec3f(float(i) + 10.f, 20.f, float(i) * 0.001f + 0.2f);
        m.width = 0.42f;
        m.height = 0.2f;
        m.extruder_id = 1;
    }
    return moves;
}
json request_json() { return {{"plate_index", 0}, {"layer", 1}}; }
GeometryIdentity identity() { return {"session", "plate", 0, 123, "app", "server", nullptr}; }
json page(const std::vector<GcodeMove>& moves, const json& params)
{
    return layer_geometry_page(moves, gcode_layers(moves), parse_layer_geometry_request(params), identity(), 2);
}
}

TEST_CASE("Geometry requests reject ambiguous selectors and malformed filters", "[LayerGeometry]")
{
    auto p = request_json();
    CHECK(parse_layer_geometry_request(p).max_points == 512);
    for (const auto& change : std::vector<json>{ {{"layer", true}}, {{"layer", 0}}, {{"layer", 1.5}},
            {{"z", 0.2}}, {{"features", json::array()}}, {{"features", {"other"}}},
            {{"filaments", {true}}}, {{"filaments", {0}}}, {{"max_points", 1}},
            {{"max_points", 8193}}, {{"unknown", 1}}, {{"plate_index", -1}} }) {
        auto invalid = p;
        invalid.update(change);
        CHECK_THROWS(parse_layer_geometry_request(invalid));
    }
    p.erase("layer");
    CHECK_THROWS(parse_layer_geometry_request(p));
    p["z"] = std::numeric_limits<double>::infinity();
    CHECK_THROWS(parse_layer_geometry_request(p));
    p = request_json();
    p["filaments"] = {2, 1, 2};
    CHECK(parse_layer_geometry_request(p).filaments == std::vector<int>{1, 2});
}

TEST_CASE("Geometry pages retain exact native XYZ and metadata across run boundaries", "[LayerGeometry]")
{
    auto moves = wall(7);
    moves[2].object_label_id = 8;
    moves[3].height = 0.3f;
    moves[4].width += 0.0001f;
    moves[5].type = EMoveType::Travel;
    moves[6].extrusion_role = erSupportMaterialInterface;
    auto out = page(moves, request_json());
    REQUIRE(out["paths"].size() == 6);
    CHECK(out["paths"][0]["filament"] == 2);
    CHECK(out["paths"][0]["object_label_id"] == -1);
    CHECK(out["paths"][1]["object_label_id"] == 8);
    CHECK(out["paths"][2]["height_mm"] == double(moves[3].height));
    CHECK(out["paths"][3]["width_mm"] == double(moves[4].width));
    CHECK(out["paths"][4]["feature"] == "support_interface");
    CHECK(out["paths"][0]["points"][0] == json::array({10., 20., double(moves[0].position.z())}));
    CHECK(out["paths"][0]["points"][1][2] == double(moves[1].position.z()));
}

TEST_CASE("Paged geometry reconstructs every segment exactly once", "[LayerGeometry]")
{
    auto moves = wall(100);
    auto p = request_json();
    p["max_points"] = 7;
    size_t expected = 1;
    bool continued = false;
    do {
        auto out = page(moves, p);
        CHECK(out.dump().size() <= 65536);
        size_t points = 0;
        for (const auto& path : out["paths"]) {
            CHECK(path["start_move"] == expected);
            CHECK(path["continues"] == continued);
            points += path["points"].size();
            for (size_t i = 1; i < path["points"].size(); ++i) {
                CHECK(path["points"][i][0] == double(moves[expected].position.x()));
                ++expected;
            }
        }
        CHECK(points <= 7);
        if (!out["has_more"].get<bool>()) break;
        p["cursor"] = out["next_cursor"];
        continued = true;
    } while (true);
    CHECK(expected == moves.size());
}

TEST_CASE("Geometry cursor fences predecessor content and independent result identity", "[LayerGeometry]")
{
    auto moves = wall(5);
    for (size_t i = 1; i < moves.size(); ++i) moves[i].layer_id = 1;
    auto p = request_json();
    p["layer"] = 2;
    p["max_points"] = 2;
    auto first = page(moves, p);
    p["cursor"] = first["next_cursor"];
    CHECK_NOTHROW(page(moves, p));
    moves[0].position.z() += 1.f;
    CHECK_THROWS_AS(page(moves, p), GeometryError);
    moves[0].position.z() -= 1.f;
    auto changed = identity();
    ++changed.native_result_id;
    CHECK_THROWS_AS(layer_geometry_page(moves, gcode_layers(moves), parse_layer_geometry_request(p), changed, 2), GeometryError);
    p["cursor"]["next_move"] = 999;
    CHECK_THROWS_AS(page(moves, p), GeometryError);
}

TEST_CASE("Geometry pages honor byte bounds and return completed empty filters", "[LayerGeometry]")
{
    auto moves = wall(10000);
    for (size_t i = 1; i < moves.size(); ++i) moves[i].object_label_id = int(i % 2);
    auto p = request_json();
    p["max_points"] = 8192;
    auto out = page(moves, p);
    CHECK(out.dump().size() <= 65536);
    CHECK(out["has_more"] == true);
    size_t points = 0;
    for (const auto& path : out["paths"]) points += path["points"].size();
    CHECK(points < 8192);
    p["features"] = {"support"};
    out = page(moves, p);
    CHECK(out["paths"].empty());
    CHECK(out["nothing_matched"] == true);
    CHECK(out["empty_layer"] == false);
    CHECK(out["has_more"] == false);
    CHECK(out["next_cursor"].is_null());
    moves = wall(3);
    for (auto& m : moves) m.type = EMoveType::Travel;
    CHECK(page(moves, request_json())["empty_layer"] == true);
}

TEST_CASE("Every supported native feature keeps its published name and filament slot", "[LayerGeometry]")
{
    const std::vector<ExtrusionRole> roles{erExternalPerimeter, erInternalInfill, erSupportMaterial,
        erSupportMaterialInterface, erBrim, erSkirt, erWipeTower};
    auto moves = wall(roles.size());
    for (size_t i = 0; i < roles.size(); ++i) moves[i + 1].extrusion_role = roles[i];
    auto out = page(moves, request_json());
    REQUIRE(out["paths"].size() == roles.size());
    for (size_t i = 0; i < roles.size(); ++i) {
        CHECK(out["paths"][i]["feature"] == extrusion_feature_key(ExtrusionFeature(i)));
        CHECK(out["paths"][i]["filament"] == 2);
    }
}

TEST_CASE("A final point boundary completes without an empty continuation page", "[LayerGeometry]")
{
    auto p = request_json();
    p["max_points"] = 2;
    auto out = page(wall(1), p);
    CHECK(out["has_more"] == false);
    CHECK(out["next_cursor"].is_null());
    REQUIRE(out["paths"].size() == 1);
    CHECK(out["paths"][0]["points"].size() == 2);
    p["max_points"] = 8192;
    out = page(wall(9000), p);
    size_t points = 0;
    for (const auto& path : out["paths"]) points += path["points"].size();
    CHECK(points <= 8192);
    CHECK(out.dump().size() <= k_geometry_byte_limit);
    CHECK(out["has_more"] == true);
}

TEST_CASE("Continuation resumes a split run but keeps metadata and travel boundaries", "[LayerGeometry]")
{
    auto moves = wall(6);
    moves[3].height = 0.3f;
    moves[4].type = EMoveType::Travel;
    moves[5].position = moves[3].position;
    auto p = request_json();
    p["max_points"] = 2;
    auto first = page(moves, p);
    p["cursor"] = first["next_cursor"];
    auto second = page(moves, p);
    CHECK(second["paths"][0]["continues"] == true);
    p["cursor"] = second["next_cursor"];
    auto third = page(moves, p);
    CHECK(third["paths"][0]["continues"] == false);
    CHECK(third["paths"][0]["height_mm"] == double(moves[3].height));
    p["cursor"] = third["next_cursor"];
    auto fourth = page(moves, p);
    CHECK(fourth["paths"][0]["start_move"] == 5);
    CHECK(fourth["paths"][0]["continues"] == false);
}

TEST_CASE("Result identity changes independently of identical layer geometry", "[LayerGeometry]")
{
    auto moves = wall(3);
    auto p = request_json();
    p["max_points"] = 2;
    auto first = page(moves, p);
    for (int change = 0; change < 4; ++change) {
        auto id = identity();
        if (change == 0) id.session = "another-session";
        if (change == 1) id.plate = "another-plate";
        if (change == 2) ++id.plate_index;
        if (change == 3) ++id.native_result_id;
        auto out = layer_geometry_page(moves, gcode_layers(moves), parse_layer_geometry_request(p), id, 2);
        CHECK(out["result_id"] != first["result_id"]);
        CHECK(out["layer_digest"] == first["layer_digest"]);
    }
    p["cursor"] = first["next_cursor"];
    moves[2].position.x() += 1.f;
    CHECK_THROWS_AS(page(moves, p), GeometryError);
    moves = wall(3);
    p["features"] = {"support"};
    CHECK_THROWS_AS(page(moves, p), GeometryError);
}

TEST_CASE("Nearest height selection retains duplicate heights and clamps", "[LayerGeometry]")
{
    auto moves = wall(3);
    moves[0].layer_id = 0;
    moves[1].layer_id = 0;
    moves[1].position.z() = 0.2f;
    moves[2].layer_id = 1;
    moves[2].position.z() = 0.4f;
    moves[3].layer_id = 2;
    moves[3].position.z() = 0.2f;
    auto p = request_json();
    p.erase("layer"); p["z"] = 0.3;
    auto out = page(moves, p);
    CHECK(out["layer"]["number"] == 1);
    CHECK(out["layer"]["also_at"] == json::array({3}));
    CHECK(out["layer"]["requested_z"] == 0.3);
    p["z"] = 100.;
    CHECK(page(moves, p)["layer"]["number"] == 2);
    p["z"] = -100.;
    CHECK(page(moves, p)["layer"]["number"] == 1);
    CHECK_THROWS_AS(page({}, request_json()), GeometryError);
}

TEST_CASE("Geometry cursors reject malformed positions and changed canonical filters", "[LayerGeometry]")
{
    auto p = request_json();
    p["max_points"] = 2;
    const auto first = page(wall(5), p);
    p["cursor"] = first["next_cursor"];
    p["max_points"] = 3;
    CHECK_NOTHROW(page(wall(5), p));
    for (const auto& change : std::vector<json>{ {{"next_move", true}}, {{"next_move", -1}},
            {{"next_move", 2.5}}, {{"result_id", "short"}}, {{"layer_digest", nullptr}},
            {{"layer", 0}}, {{"features", {"support"}}}, {{"filaments", {1}}}, {{"extra", 1}} }) {
        auto invalid = p;
        invalid["cursor"].update(change);
        CHECK_THROWS_AS(parse_layer_geometry_request(invalid), GeometryError);
    }
    auto moves = wall(5);
    moves[3].type = EMoveType::Travel;
    auto out = page(moves, request_json());
    p["cursor"]["layer_digest"] = out["layer_digest"];
    p["cursor"]["next_move"] = 3;
    CHECK_THROWS_AS(page(moves, p), GeometryError);
    p["cursor"] = first["next_cursor"];
    p["cursor"]["layer"] = 2;
    CHECK_THROWS_AS(page(wall(5), p), GeometryError);
}

TEST_CASE("Every native grouping input participates in the layer digest", "[LayerGeometry]")
{
    auto original = wall(3);
    const auto first = page(original, request_json());
    for (int field = 0; field < 8; ++field) {
        auto moves = original;
        if (field == 0) moves[2].width += 0.0001f;
        if (field == 1) moves[2].height += 0.01f;
        if (field == 2) ++moves[2].object_label_id;
        if (field == 3) ++moves[2].extruder_id;
        if (field == 4) moves[2].type = EMoveType::Travel;
        if (field == 5) moves[2].extrusion_role = erPerimeter; // Same feature, different native role.
        if (field == 6) moves[2].position.y() += 0.1f;
        if (field == 7) moves[2].position.z() += 0.01f;
        const auto changed = page(moves, request_json());
        CHECK(changed["layer_digest"] != first["layer_digest"]);
        CHECK(changed["result_id"] == first["result_id"]);
    }
}

TEST_CASE("The native visitor breaks at travel and retains the off-layer predecessor", "[LayerGeometry]")
{
    auto moves = wall(4);
    moves[0].layer_id = 0;
    moves[0].position.z() = 0.1f;
    for (size_t i = 1; i < moves.size(); ++i) moves[i].layer_id = 1;
    moves[2].type = EMoveType::Travel;
    moves[3].extrusion_role = erCustom;
    const auto layers = gcode_layers(moves);
    std::vector<bool> extrusions;
    visit_layer_segments(moves, layers[1], [&](const NativeLayerSegment& segment) {
        if (segment.index == 1) CHECK_THAT(segment.previous.position.z(), Catch::Matchers::WithinAbs(0.1f, 1e-8));
        extrusions.push_back(segment.extrusion);
        return true;
    });
    CHECK(extrusions == std::vector<bool>{true, false, false, true});
    auto p = request_json(); p["layer"] = 2;
    const auto out = page(moves, p);
    REQUIRE(out["paths"].size() == 2);
    CHECK(out["paths"][0]["points"][0][2] == double(moves[0].position.z()));
    CHECK(out["paths"][1]["continues"] == false);
}

TEST_CASE("Geometry discovery rejects undeclared top-level and cursor keys before dispatch", "[LayerGeometry]")
{
    const auto schema = layer_geometry_schema();
    CHECK(schema["additionalProperties"] == false);
    CHECK(schema["properties"]["cursor"]["additionalProperties"] == false);
    CHECK_FALSE(tool_arguments_error("get_layer_geometry", schema, request_json()).has_value());
    auto p = request_json(); p["unknown"] = 1;
    CHECK(tool_arguments_error("get_layer_geometry", schema, p).has_value());
    p = request_json(); p["cursor"] = {{"unknown", 1}};
    CHECK(tool_arguments_error("get_layer_geometry", schema, p).has_value());
    const auto& names = schema["properties"]["features"]["items"]["enum"];
    REQUIRE(names.size() == 7);
    for (size_t i = 0; i < names.size(); ++i)
        CHECK(names[i] == extrusion_feature_key(ExtrusionFeature(i)));
}

TEST_CASE("Filtered pages reconstruct separate native runs without skipping segments", "[LayerGeometry]")
{
    auto moves = wall(1000);
    std::vector<size_t> expected;
    for (size_t i = 1; i < moves.size(); ++i) {
        moves[i].object_label_id = int(i % 2);
        if (i % 5 == 0) moves[i].type = EMoveType::Travel;
        if (i % 7 == 0) moves[i].extrusion_role = erSupportMaterial;
        if (i % 11 == 0) moves[i].extruder_id = 0;
        if (i % 5 != 0 && i % 7 != 0 && i % 11 != 0) expected.push_back(i);
    }
    auto p = request_json();
    p["features"] = {"perimeters"}; p["filaments"] = {2}; p["max_points"] = 31;
    size_t seen = 0, pages = 0;
    do {
        auto out = page(moves, p);
        CHECK(out.dump().size() <= k_geometry_byte_limit);
        REQUIRE_FALSE(out["paths"].empty());
        for (const auto& path : out["paths"]) {
            REQUIRE(seen < expected.size());
            const size_t index = expected[seen++];
            CHECK(path["start_move"] == index);
            CHECK(path["object_label_id"] == moves[index].object_label_id);
            CHECK(path["points"].size() == 2);
            CHECK(path["points"][0][0] == double(moves[index - 1].position.x()));
            CHECK(path["points"][1][0] == double(moves[index].position.x()));
            CHECK(path["continues"] == false);
        }
        ++pages;
        REQUIRE(pages <= expected.size());
        if (!out["has_more"].get<bool>()) break;
        p["cursor"] = out["next_cursor"];
    } while (true);
    CHECK(seen == expected.size());
}

TEST_CASE("The default page budget includes its repeated starting endpoint", "[LayerGeometry]")
{
    const auto out = page(wall(2000), request_json());
    REQUIRE(out["paths"].size() == 1);
    CHECK(out["paths"][0]["points"].size() == 512);
    CHECK(out["next_cursor"]["next_move"] == 512);
    CHECK(out["has_more"] == true);
}

TEST_CASE("Geometry failures preserve the endpoint-local domain codes", "[LayerGeometry]")
{
    try { page({}, request_json()); FAIL("Expected no_valid_slice"); }
    catch (const GeometryError& e) { CHECK(e.code == "no_valid_slice"); }
    auto p = request_json(); p["filaments"] = {3};
    try { page(wall(2), p); FAIL("Expected invalid_arguments"); }
    catch (const GeometryError& e) { CHECK(e.code == "invalid_arguments"); }
    p = request_json(); p["max_points"] = 2;
    p["cursor"] = page(wall(3), p)["next_cursor"];
    auto changed = wall(3); changed[1].height = 0.3f;
    try { page(changed, p); FAIL("Expected stale_slice"); }
    catch (const GeometryError& e) { CHECK(e.code == "stale_slice"); }
    auto id = identity(); id.application_version.assign(k_geometry_byte_limit, 'x');
    const auto moves = wall(1);
    try {
        layer_geometry_page(moves, gcode_layers(moves), parse_layer_geometry_request(request_json()), id, 2);
        FAIL("Expected output_limit");
    } catch (const GeometryError& e) { CHECK(e.code == "output_limit"); }
}
