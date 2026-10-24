#include <catch2/catch_all.hpp>

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderPaint.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.hpp"

// render_plate_view's paint: the tints and their colours, the painted facets and the legend that counts them, and the
// request's refusals.

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using json = nlohmann::json;

namespace {

double distance(const ColorRGBA& a, const ColorRGBA& b)
{
    return std::sqrt(std::pow(a.r() - b.r(), 2) + std::pow(a.g() - b.g(), 2) + std::pow(a.b() - b.b(), 2));
}

std::string refusal_of(const json& payload)
{
    std::string error;
    REQUIRE_FALSE(read_render_request(payload, error).has_value());
    return error;
}

// A 12-facet cube, 10 mm: facets 0-3 painted `first`, 4-5 painted `second`, the rest bare.
PaintData cube_paint(const TriangleMesh& mesh, PaintMode mode, int first, int second)
{
    std::vector<int> states(mesh.its.indices.size(), 0);
    for (size_t i = 0; i < 4; ++i) states[i] = first;
    for (size_t i = 4; i < 6; ++i) states[i] = second;
    PaintWrite write;
    REQUIRE(build_paint_write(mesh, mode, states, true, PaintData{}, write));
    return write.data;
}

} // namespace

TEST_CASE("The support and seam tints are the paint panels' own colours", "[RenderPaint][orcamcp]")
{
    const auto tints = paint_tints({"support", "seam"}, {}, {});
    REQUIRE(tints.size() == 4);
    CHECK(tints[0].channel == "support");
    CHECK(tints[0].state == 1);
    CHECK(tints[0].color == TriangleSelectorGUI::enforcers_color);
    CHECK(tints[1].state == 2);
    CHECK(tints[1].color == TriangleSelectorGUI::blockers_color);
    CHECK(tints[2].channel == "seam");
    CHECK(tints[3].color == TriangleSelectorGUI::blockers_color);
}

TEST_CASE("Fuzzy skin has an enforcer tint only", "[RenderPaint][orcamcp]")
{
    const auto tints = paint_tints({"fuzzy_skin"}, {}, {});
    REQUIRE(tints.size() == 1);
    CHECK(tints[0].color == TriangleSelectorGUI::enforcers_color);
    CHECK(paint_channel_of(tints[0]).mode == PaintMode::FuzzySkin);
}

TEST_CASE("Texture tints are one per slot, chosen slots first and the plate's layers otherwise", "[RenderPaint][orcamcp]")
{
    const auto chosen = paint_tints({"texture"}, {5, 2, 5}, {0, 1});
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[0].texture_layer == 2);
    CHECK(chosen[1].texture_layer == 5);
    const auto defaulted = paint_tints({"texture"}, {}, {3, 1});
    REQUIRE(defaulted.size() == 2);
    CHECK(defaulted[0].texture_layer == 1);
    CHECK(defaulted[1].texture_layer == 3);
    CHECK(defaulted[1].color == texture_layer_color(3));
    CHECK(paint_channel_of(defaulted[1]).texture_layer == 3);
    CHECK(paint_tints({"texture"}, {}, {}).empty());
}

TEST_CASE("The eight texture colours differ from each other, the object palette, the gizmo colours and the grey", "[RenderPaint][orcamcp]")
{
    std::vector<ColorRGBA> others;
    for (int i = 0; i < 12; ++i)
        others.push_back(object_palette_color(i));
    others.push_back(TriangleSelectorGUI::enforcers_color);
    others.push_back(TriangleSelectorGUI::blockers_color);
    others.push_back(GLVolume::NEUTRAL_COLOR);
    for (int a = 0; a < 8; ++a) {
        for (const ColorRGBA& other : others)
            CHECK(distance(texture_layer_color(a), other) > 0.3);
        for (int b = a + 1; b < 8; ++b)
            CHECK(distance(texture_layer_color(a), texture_layer_color(b)) > 0.3);
    }
}

TEST_CASE("Where a colour comes from: colour paint wins, then the neutral base, then color_by", "[RenderPaint][orcamcp]")
{
    CHECK(color_source_of(true, true, ColorBy::Filament) == "paint");
    CHECK(color_source_of(false, true, ColorBy::Filament) == "neutral");
    CHECK(color_source_of(false, false, ColorBy::Filament) == "filament");
    CHECK(color_source_of(false, false, ColorBy::Object) == "palette");
}

TEST_CASE("A part's filament colour clamps to the slots there are", "[RenderPaint][orcamcp]")
{
    const std::vector<ColorRGBA> colors = {{1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}};
    CHECK(filament_color_for(1, colors) == colors[0]);
    CHECK(filament_color_for(2, colors) == colors[1]);
    CHECK(filament_color_for(0, colors) == colors[0]);
    CHECK(filament_color_for(9, colors) == colors[1]);
}

TEST_CASE("The legend counts a cube's support enforcers and blockers and their area", "[RenderPaint][orcamcp]")
{
    TriangleMesh mesh(its_make_cube(10.0, 10.0, 10.0));
    PaintVolumeInput in;
    in.object_id = 3;
    in.volume_id = 0;
    in.mesh      = std::make_shared<const TriangleMesh>(mesh);
    in.per_tint  = {{0, cube_paint(mesh, PaintMode::Support, 1, 2)}, {1, cube_paint(mesh, PaintMode::Support, 1, 2)}};
    const auto tints = paint_tints({"support"}, {}, {});
    const auto facets = compute_paint_facets({in}, tints, [] { return false; });
    REQUIRE(facets.size() == 2);

    const json legend = tint_legend_json(tints, facets);
    REQUIRE(legend.size() == 2);
    CHECK(legend[0]["label"] == "support enforcer");
    CHECK(legend[0]["state_name"] == "enforcer");
    CHECK(legend[0]["color"] == "#80ff80");
    REQUIRE(legend[0]["objects"].size() == 1);
    CHECK(legend[0]["objects"][0]["object_id"] == 3);
    CHECK(legend[0]["objects"][0]["facets"] == 4);
    CHECK(legend[1]["objects"][0]["facets"] == 2);
    // Two triangles of a cube face are 50 mm2 each at 10 mm: facets 0-3 are two faces, 4-5 one.
    CHECK(legend[0]["objects"][0]["area_mm2"].get<double>() == Catch::Approx(200.).margin(0.01));
    CHECK(legend[1]["objects"][0]["area_mm2"].get<double>() == Catch::Approx(100.).margin(0.01));
}

TEST_CASE("A tint nothing carries is listed with no objects, and the area follows the instance's transform", "[RenderPaint][orcamcp]")
{
    TriangleMesh mesh(its_make_cube(10.0, 10.0, 10.0));
    PaintVolumeInput in;
    in.object_id = 0;
    in.mesh      = std::make_shared<const TriangleMesh>(mesh);
    in.to_plate  = Transform3d(Eigen::Scaling(2.0));   // area x4
    in.per_tint  = {{0, cube_paint(mesh, PaintMode::Support, 1, 2)}};
    const auto tints = paint_tints({"support", "texture"}, {4}, {});
    const auto facets = compute_paint_facets({in}, tints, [] { return false; });
    const json legend = tint_legend_json(tints, facets);
    REQUIRE(legend.size() == 3);
    CHECK(legend[1]["objects"].empty());   // no support blocker tint data was captured for tint 1
    CHECK(legend[2]["layer"] == 4);
    CHECK(legend[2]["objects"].empty());
    CHECK(legend[0]["objects"][0]["area_mm2"].get<double>() == Catch::Approx(800.).margin(0.01));
}

TEST_CASE("Texture slot 3 of a two-part model counts only the part that carries it", "[RenderPaint][orcamcp]")
{
    TriangleMesh mesh(its_make_cube(10.0, 10.0, 10.0));
    PaintVolumeInput bare, painted;
    bare.object_id = painted.object_id = 0;
    bare.volume_id = 0;
    painted.volume_id = 1;
    bare.mesh = painted.mesh = std::make_shared<const TriangleMesh>(mesh);
    bare.per_tint    = {};                                                          // slot 3 has no paint on this part
    painted.per_tint = {{0, cube_paint(mesh, PaintMode::Texture, 1, 0)}};
    const auto tints  = paint_tints({"texture"}, {3}, {});
    const auto facets = compute_paint_facets({bare, painted}, tints, [] { return false; });
    REQUIRE(facets.size() == 1);
    CHECK(facets[0].volume_id == 1);
    CHECK(facets[0].facets == 4);
}

TEST_CASE("The facets of a tint are the leaves the annotation holds at that state", "[RenderPaint][orcamcp]")
{
    TriangleMesh mesh(its_make_cube(10.0, 10.0, 10.0));
    const PaintData data = cube_paint(mesh, PaintMode::Seam, 1, 2);
    const auto enforcers = summarize_paint_data(mesh, data);
    int counted = 0;
    for (const PaintedStateInfo& info : enforcers)
        if (info.state == 1) counted = info.facet_count;
    CHECK(painted_facets(mesh, data, 1).indices.size() == size_t(counted));
    CHECK(painted_facets(mesh, data, 2).indices.size() == 2);
}

TEST_CASE("Mesh work stops when asked to", "[RenderPaint][orcamcp]")
{
    TriangleMesh mesh(its_make_cube(10.0, 10.0, 10.0));
    PaintVolumeInput in;
    in.mesh     = std::make_shared<const TriangleMesh>(mesh);
    in.per_tint = {{0, cube_paint(mesh, PaintMode::Support, 1, 2)}};
    CHECK(compute_paint_facets({in}, paint_tints({"support"}, {}, {}), [] { return true; }).empty());
}

TEST_CASE("The request reads paint, texture_layers and color_by", "[RenderPaint][RenderRequest][orcamcp]")
{
    std::string error;
    auto r = read_render_request({{"plate_index", 0}, {"paint", {"texture", "support", "support"}}, {"texture_layers", {3, 1, 3}}, {"color_by", "filament"}}, error);
    REQUIRE(r.has_value());
    CHECK(r->paint == std::vector<std::string>{"support", "texture"});
    CHECK(r->texture_layers == std::vector<int>{1, 3});
    CHECK(r->color_by == ColorBy::Filament);
    r = read_render_request({{"plate_index", 0}}, error);
    REQUIRE(r.has_value());
    CHECK(r->paint.empty());
    CHECK(r->color_by == ColorBy::Object);
}

TEST_CASE("The request refuses a paint channel, layer or colouring it does not know", "[RenderPaint][RenderRequest][orcamcp]")
{
    CHECK(refusal_of({{"plate_index", 0}, {"paint", {"color"}}}).find("is not a channel") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"paint", "support"}}).find("list of channels") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"texture_layers", {1}}}).find("add \"texture\" to paint") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"paint", {"texture"}}, {"texture_layers", {8}}}).find("slot is an integer 0 to 7") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"paint", {"texture"}}, {"texture_layers", {1.5}}}).find("not a layer slot") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"color_by", "relief"}}).find("color_by must be") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"layer_view", "first_layer"}, {"paint", {"support"}}}).find("3D views") != std::string::npos);
}
