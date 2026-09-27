#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPLayerPlan.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.hpp"
#include "fff_print/test_helpers.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "mcp_slice_fixtures.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
#include <vector>

// The sliced layer plan: render_plate_view's layer_view {layer} / {z}. An agent asked "is this the
// best support we can have" could not see a single sliced layer; the plan draws any one of them from
// the G-code the Preview shows, filtered by feature and filament, with the numbers that let it check
// coverage without guessing from pixels.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A move as the processor stores it: where it ends, on 0-based layer `layer_id`.
GcodeMove move_to(unsigned layer_id, double x, double y, double z, EMoveType type = EMoveType::Travel,
                  ExtrusionRole role = erNone, int filament = 1, float width = 0.4f)
{
    GcodeMove m;
    m.layer_id       = layer_id;
    m.position       = Vec3f(float(x), float(y), float(z));
    m.type           = type;
    m.extrusion_role = role;
    m.extruder_id    = static_cast<unsigned char>(filament - 1);
    m.width          = width;
    return m;
}

GcodeMove extrude_to(unsigned layer_id, double x, double y, double z, ExtrusionRole role, int filament = 1, float width = 0.4f)
{
    return move_to(layer_id, x, y, z, EMoveType::Extrude, role, filament, width);
}

// Three layers at 0.2, 0.4 and 0.6 mm, each a travel to (0, 0) and a 10 mm outer wall along x.
std::vector<GcodeMove> three_walls()
{
    std::vector<GcodeMove> moves;
    for (unsigned id = 0; id < 3; ++id) {
        const double z = 0.2 * (id + 1);
        moves.push_back(move_to(id, 0., 0., z));
        moves.push_back(extrude_to(id, 10., 0., z, erExternalPerimeter));
    }
    return moves;
}

std::vector<GcodeLayer> layers_at(const std::vector<double>& zs)
{
    std::vector<GcodeLayer> layers;
    for (double z : zs)
        layers.push_back({0, 0, z});
    return layers;
}

double feature_mm2(const LayerExtrusion& e, ExtrusionFeature f) { return e.mm2[size_t(f)]; }

double area_mm2(const ExPolygons& shape) { return area(shape) * SCALING_FACTOR * SCALING_FACTOR; }

// layer_view's object form, read the way render_plate_view reads it.
LayerPlanRequest sliced_request(const nlohmann::json& view)
{
    const LayerView parsed = parse_layer_view(view);
    REQUIRE_FALSE(parsed.first_layer);
    return parsed.sliced;
}

} // namespace

// --- which layer ---------------------------------------------------------------------------------

TEST_CASE("the G-code's layers are found by layer number, each at the height it prints", "[orcamcp][LayerPlan]")
{
    const std::vector<GcodeMove>  moves  = three_walls();
    const std::vector<GcodeLayer> layers = gcode_layers(moves);
    REQUIRE(layers.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(layers[i].begin == 2 * i);
        CHECK(layers[i].end == 2 * i + 2);
        CHECK_THAT(layers[i].z, WithinAbs(0.2 * (i + 1), 1e-6));
    }
    CHECK(gcode_layers({}).empty());
}

TEST_CASE("a layer's height is where its last extrusion is, not a travel lift after it", "[orcamcp][LayerPlan]")
{
    std::vector<GcodeMove> moves = {move_to(0, 0., 0., 0.2), extrude_to(0, 10., 0., 0.2, erPerimeter),
                                    move_to(0, 10., 0., 0.6),  // z-hop, or the end G-code's lift
                                    extrude_to(0, 12., 0., 5.0, erCustom)};  // not a feature: ignored
    const std::vector<GcodeLayer> layers = gcode_layers(moves);
    REQUIRE(layers.size() == 1);
    CHECK_THAT(layers[0].z, WithinAbs(0.2, 1e-6));
}

TEST_CASE("layer numbers count from 1, as the Preview's slider does", "[orcamcp][LayerPlan]")
{
    const std::vector<GcodeLayer> layers = gcode_layers(three_walls());
    CHECK(layer_by_number(layers, 1) == 0);
    CHECK(layer_by_number(layers, 3) == 2);
    CHECK_THROWS_WITH(layer_by_number(layers, 0), ContainsSubstring("1..3"));
    CHECK_THROWS_WITH(layer_by_number(layers, 4), ContainsSubstring("1..3"));
}

TEST_CASE("a height snaps to the nearest printed layer", "[orcamcp][LayerPlan]")
{
    const std::vector<GcodeLayer> layers = layers_at({0.2, 0.4, 0.6});
    CHECK(layer_nearest_z(layers, 0.41).index == 1);
    CHECK(layer_nearest_z(layers, 0.29).index == 0);
    CHECK(layer_nearest_z(layers, 0.3).index == 0);   // exactly between: the lower
    CHECK(layer_nearest_z(layers, -5.).index == 0);   // below the print: its first layer
    CHECK(layer_nearest_z(layers, 99.).index == 2);   // above it: its last
    CHECK(layer_nearest_z(layers, 0.4).also_at.empty());
    CHECK_THROWS(layer_nearest_z({}, 0.2));
}

TEST_CASE("a height exactly between two layers goes to the lower, whatever float rounding did", "[orcamcp][LayerPlan]")
{
    // G-code heights come from floats: float(0.7) is 0.69999999, float(0.9) 0.89999998, so the upper
    // layer is 4e-8 nearer to 0.8 than the lower. Such a difference is noise, not a nearer layer.
    const std::vector<GcodeLayer> layers = layers_at({double(float(0.7)), double(float(0.9))});
    CHECK(layer_nearest_z(layers, 0.8).index == 0);
    // Heights the G-code rounds to a thousandth: 0.3 is still "between" 0.2 and 0.4005.
    CHECK(layer_nearest_z(layers_at({0.2, 0.4005}), 0.3).index == 0);
    // A real difference still decides.
    CHECK(layer_nearest_z(layers, 0.81).index == 1);
}

TEST_CASE("a height a by-object print reaches twice names the other layer", "[orcamcp][LayerPlan]")
{
    // Two objects printed one after the other, two layers each.
    const std::vector<GcodeLayer> layers = layers_at({0.2, 0.4, 0.2, 0.4});
    const LayerAtHeight           hit    = layer_nearest_z(layers, 0.4);
    CHECK(hit.index == 1);
    CHECK(hit.also_at == std::vector<int>{4});
}

TEST_CASE("support at heights of its own is matched to its own layer", "[orcamcp][LayerPlan]")
{
    const std::vector<double> object  = {0.2, 0.4, 0.6, 0.8};
    const std::vector<double> support = {0.2, 0.475, 0.75};
    // A support-only height has no object layer.
    CHECK_FALSE(layer_index_at_height(object, 0.475, k_gcode_height_tolerance).has_value());
    CHECK(layer_index_at_height(support, 0.475, k_gcode_height_tolerance) == 1u);
    // A shared height has both.
    CHECK(layer_index_at_height(object, 0.2, k_gcode_height_tolerance) == 0u);
    CHECK(layer_index_at_height(support, 0.2, k_gcode_height_tolerance) == 0u);
    // The G-code rounds heights to a thousandth; the closest layer wins.
    CHECK(layer_index_at_height(object, 0.4005, k_gcode_height_tolerance) == 1u);
    CHECK_FALSE(layer_index_at_height(object, 0.43, k_gcode_height_tolerance).has_value());
    CHECK(layer_index_at_height({0.4, 0.4015}, 0.4012, k_gcode_height_tolerance) == 1u);
    CHECK_FALSE(layer_index_at_height({}, 0.4, k_gcode_height_tolerance).has_value());
}

// --- the request ---------------------------------------------------------------------------------

TEST_CASE("a layer view names a layer or a height, and nothing else", "[orcamcp][LayerPlan]")
{
    const LayerPlanRequest by_number = sliced_request({{"layer", 3}});
    CHECK(by_number.layer == 3);
    CHECK_FALSE(by_number.z.has_value());
    CHECK(sliced_request({{"layer", "3"}}).layer == 3);  // a stale client's string
    const LayerPlanRequest by_height = sliced_request({{"z", 1.5}});
    REQUIRE(by_height.z.has_value());
    CHECK_THAT(*by_height.z, WithinAbs(1.5, 1e-12));

    CHECK_THROWS_WITH(sliced_request(nlohmann::json::object()), ContainsSubstring("layer"));
    CHECK_THROWS_WITH(sliced_request({{"layer", 1}, {"z", 1.0}}), ContainsSubstring("not both"));
    CHECK_THROWS(sliced_request({{"layer", 2.5}}));
    CHECK_THROWS_WITH(sliced_request({{"layers", 3}}), ContainsSubstring("layers"));
}

TEST_CASE("one validator reads every form of layer_view", "[orcamcp][LayerPlan]")
{
    CHECK(parse_layer_view("first_layer").first_layer);
    CHECK(parse_layer_view(R"({"layer": 7})").sliced.layer == 7);  // a stale client's JSON text
    for (const nlohmann::json& wrong : {nlohmann::json("last_layer"), nlohmann::json(3), nlohmann::json::array({1}),
                                        nlohmann::json("[1]")})
        CHECK_THROWS_WITH(parse_layer_view(wrong), ContainsSubstring(R"("first_layer", {"layer": n} or {"z": mm})"));
}

TEST_CASE("a layer view draws every feature and filament, coloured by feature, unless asked otherwise", "[orcamcp][LayerPlan]")
{
    const LayerPlanRequest r = sliced_request({{"layer", 1}});
    for (size_t f = 0; f < k_plan_feature_count; ++f)
        CHECK(r.draws(ExtrusionFeature(f)));
    CHECK(r.draws(1));
    CHECK(r.draws(4));
    CHECK(r.color_by == LayerColorBy::feature);
    CHECK_FALSE(r.fit_object.has_value());
}

TEST_CASE("features, filaments, colour and fit are read and checked", "[orcamcp][LayerPlan]")
{
    const LayerPlanRequest r = sliced_request({{"layer", 1},
                                                         {"features", {"support", "support_interface"}},
                                                         {"filaments", {2, 4}},
                                                         {"color_by", "filament"},
                                                         {"fit", {{"object_index", 2}}}});
    CHECK(r.draws(ExtrusionFeature::support));
    CHECK(r.draws(ExtrusionFeature::support_interface));
    CHECK_FALSE(r.draws(ExtrusionFeature::perimeters));
    CHECK_FALSE(r.draws(ExtrusionFeature::prime_tower));
    CHECK(r.draws(2));
    CHECK_FALSE(r.draws(1));
    CHECK(r.color_by == LayerColorBy::filament);
    CHECK(r.fit_object == 2);
    CHECK_FALSE(sliced_request({{"layer", 1}, {"fit", "plate"}}).fit_object.has_value());

    const nlohmann::json drawn = drawn_json(r);
    CHECK(drawn["features"] == nlohmann::json({"support", "support_interface"}));
    CHECK(drawn["filaments"] == nlohmann::json({2, 4}));
    CHECK(drawn["color_by"] == "filament");

    CHECK_THROWS_WITH(sliced_request({{"layer", 1}, {"features", {"walls"}}}), ContainsSubstring("prime_tower"));
    CHECK_THROWS_WITH(sliced_request({{"layer", 1}, {"features", {"other"}}}), ContainsSubstring("perimeters"));
    CHECK_THROWS_WITH(sliced_request({{"layer", 1}, {"filaments", {0}}}), ContainsSubstring("from 1"));
    CHECK_THROWS_WITH(check_filaments(sliced_request({{"layer", 1}, {"filaments", {5}}}), 4), ContainsSubstring("1..4"));
    CHECK_NOTHROW(check_filaments(sliced_request({{"layer", 1}, {"filaments", {4}}}), 4));
    CHECK_THROWS_WITH(sliced_request({{"layer", 1}, {"color_by", "tool"}}), ContainsSubstring("filament"));
    CHECK_THROWS(sliced_request({{"layer", 1}, {"fit", {{"object_index", "two"}}}}));
}

// --- toolpaths -----------------------------------------------------------------------------------

namespace {

// One layer at 0.2 mm: a wall on filament 1, support on filament 2, interface on filament 2, a
// tower on filament 1 -- each a 10 mm line after a travel.
std::vector<GcodeMove> mixed_layer()
{
    return {move_to(0, 0., 0., 0.2),  extrude_to(0, 10., 0., 0.2, erExternalPerimeter, 1),
            move_to(0, 0., 5., 0.2),  extrude_to(0, 10., 5., 0.2, erSupportMaterial, 2),
            move_to(0, 0., 10., 0.2), extrude_to(0, 10., 10., 0.2, erSupportMaterialInterface, 2),
            move_to(0, 50., 0., 0.2), extrude_to(0, 60., 0., 0.2, erWipeTower, 1)};
}

std::set<ExtrusionFeature> features_of(const std::vector<ToolpathRun>& runs)
{
    std::set<ExtrusionFeature> out;
    for (const ToolpathRun& r : runs)
        out.insert(r.feature);
    return out;
}

} // namespace

TEST_CASE("the feature filter draws only the chosen features", "[orcamcp][LayerPlan]")
{
    const std::vector<GcodeMove> moves = mixed_layer();
    const GcodeLayer             layer = gcode_layers(moves).front();
    const auto runs = layer_toolpaths(moves, layer, sliced_request({{"layer", 1}, {"features", {"support_interface"}}}));
    CHECK(features_of(runs) == std::set<ExtrusionFeature>{ExtrusionFeature::support_interface});
    CHECK(features_of(layer_toolpaths(moves, layer, sliced_request({{"layer", 1}}))).size() == 4);
}

TEST_CASE("the filament filter draws only the chosen filaments", "[orcamcp][LayerPlan]")
{
    const std::vector<GcodeMove> moves = mixed_layer();
    const GcodeLayer             layer = gcode_layers(moves).front();
    const auto runs = layer_toolpaths(moves, layer, sliced_request({{"layer", 1}, {"filaments", {2}}}));
    REQUIRE(runs.size() == 2);
    for (const ToolpathRun& r : runs)
        CHECK(r.filament == 2);
    CHECK(features_of(runs) == std::set<ExtrusionFeature>{ExtrusionFeature::support, ExtrusionFeature::support_interface});
}

TEST_CASE("a run breaks where the feature, filament or width changes, or the nozzle travels", "[orcamcp][LayerPlan]")
{
    const std::vector<GcodeMove> moves = {
        move_to(0, 0., 0., 0.2),
        extrude_to(0, 1., 0., 0.2, erPerimeter), extrude_to(0, 2., 0., 0.2, erPerimeter),
        extrude_to(0, 3., 0., 0.2, erExternalPerimeter),              // same feature: perimeters
        extrude_to(0, 4., 0., 0.2, erPerimeter, 1, 0.6f),             // wider
        extrude_to(0, 5., 0., 0.2, erPerimeter, 2, 0.6f),             // another filament
        move_to(0, 9., 0., 0.2),                                      // travel
        extrude_to(0, 10., 0., 0.2, erPerimeter, 2, 0.6f),
        extrude_to(0, 11., 0., 0.2, erInternalInfill, 2, 0.6f),       // another feature
    };
    const auto runs = layer_toolpaths(moves, gcode_layers(moves).front(), sliced_request({{"layer", 1}}));
    REQUIRE(runs.size() == 5);
    CHECK(runs[0].points.size() == 4);  // from (0,0) through x = 1, 2, 3
    CHECK_THAT(runs[0].points.front().x(), WithinAbs(0., 1e-6));
    CHECK_THAT(runs[0].points.back().x(), WithinAbs(3., 1e-6));
    CHECK_THAT(double(runs[1].width), WithinAbs(0.6, 1e-6));
    CHECK(runs[2].filament == 2);
    CHECK_THAT(runs[3].points.front().x(), WithinAbs(9., 1e-6));  // starts where the travel ended
    CHECK(runs[4].feature == ExtrusionFeature::infill);
}

// --- numbers -------------------------------------------------------------------------------------

TEST_CASE("areas are length times width for the whole layer, whatever is drawn", "[orcamcp][LayerPlan]")
{
    std::vector<GcodeMove> moves = mixed_layer();
    moves.push_back(move_to(0, 0., 20., 0.2));
    moves.push_back(extrude_to(0, 10., 20., 0.2, erSkirt, 1, 0.5f));     // 10 mm x 0.5 mm
    moves.push_back(extrude_to(0, 10., 30., 0.2, erCustom, 1));          // a purge line: not a feature
    const LayerExtrusion e = layer_extrusion(moves, gcode_layers(moves).front());

    CHECK_THAT(feature_mm2(e, ExtrusionFeature::perimeters), WithinRel(4.0, 1e-5));
    CHECK_THAT(feature_mm2(e, ExtrusionFeature::support), WithinRel(4.0, 1e-5));
    CHECK_THAT(feature_mm2(e, ExtrusionFeature::support_interface), WithinRel(4.0, 1e-5));
    CHECK_THAT(feature_mm2(e, ExtrusionFeature::prime_tower), WithinRel(4.0, 1e-5));
    CHECK_THAT(feature_mm2(e, ExtrusionFeature::skirt), WithinRel(5.0, 1e-5));
    CHECK_THAT(feature_mm2(e, ExtrusionFeature::infill), WithinAbs(0., 1e-12));
    CHECK_THAT(e.object_mm2(), WithinRel(4.0, 1e-5));
    CHECK_THAT(e.support_mm2(), WithinRel(8.0, 1e-5));

    REQUIRE(e.mm2_by_filament.count(2) == 1);
    CHECK_THAT(e.mm2_by_filament.at(2)[size_t(ExtrusionFeature::support)], WithinRel(4.0, 1e-5));
    CHECK_THAT(e.mm2_by_filament.at(1)[size_t(ExtrusionFeature::support)], WithinAbs(0., 1e-12));
    CHECK(e.filament_order == std::vector<int>{1, 2});

    // The extent covers the object and its support, not the tower 40 mm away or the skirt.
    CHECK_THAT(e.extent.max.x(), WithinAbs(10., 1e-6));
    CHECK_THAT(e.extent.max.y(), WithinAbs(10., 1e-6));

    const nlohmann::json json = layer_extrusion_json(e);
    CHECK_THAT(json["extruded_mm2"]["support_interface"].get<double>(), WithinAbs(4.0, 0.005));
    CHECK_THAT(json["extruded_mm2_by_filament"]["2"]["support"].get<double>(), WithinAbs(4.0, 0.005));
    CHECK(json["filaments"] == nlohmann::json({1, 2}));
    CHECK_THAT(json["support_mm2"].get<double>(), WithinAbs(8.0, 0.005));
}

TEST_CASE("a layer's first line starts where the layer below ended", "[orcamcp][LayerPlan]")
{
    const std::vector<GcodeMove> moves = {move_to(0, 0., 0., 0.2), extrude_to(0, 10., 0., 0.2, erPerimeter),
                                          extrude_to(1, 10., 10., 0.4, erPerimeter, 1, 0.5f)};
    const std::vector<GcodeLayer> layers = gcode_layers(moves);
    REQUIRE(layers.size() == 2);
    CHECK_THAT(feature_mm2(layer_extrusion(moves, layers[1]), ExtrusionFeature::perimeters), WithinRel(5.0, 1e-5));
    const auto runs = layer_toolpaths(moves, layers[1], sliced_request({{"layer", 2}}));
    REQUIRE(runs.size() == 1);
    CHECK_THAT(runs[0].points.front().y(), WithinAbs(0., 1e-6));
}

TEST_CASE("heights and areas are reported as the numbers they are, not float noise", "[orcamcp][LayerPlan]")
{
    ObjectAtHeight o;
    o.object_index = 0;
    o.object_layer = PrintedLayerRef{30, 6.0500000000000007, 0.2};
    Overhang hang;
    hang.area_mm2      = 858.4200000000001;
    hang.support_below = SupportBelow{9.8500000000000014, 0.40000000000000036, 742.3100000000001, 0.};
    o.overhang         = hang;
    const nlohmann::json json = objects_at_height_json({o})[0];
    CHECK(json["object_layer"]["print_z"].dump() == "6.05");
    CHECK(json["overhang"]["area_mm2"].dump() == "858.42");
    CHECK(json["overhang"]["support_below"]["z"].dump() == "9.85");
    CHECK(json["overhang"]["support_below"]["gap_mm"].dump() == "0.4");
    CHECK(json["overhang"]["support_below"]["support_mm2"].dump() == "742.31");
    CHECK(json["support_layer"].is_null());

    hang.support_below.reset();  // no support layer below at all
    o.overhang = hang;
    CHECK(objects_at_height_json({o})[0]["overhang"]["support_below"].is_null());
}

// --- colour and legend ---------------------------------------------------------------------------

TEST_CASE("colour by feature is the Preview's, colour by filament is the slot's", "[orcamcp][LayerPlan]")
{
    const std::vector<ColorRGBA> slots = {ColorRGBA(1.f, 0.f, 0.f, 1.f), ColorRGBA(0.f, 0.f, 1.f, 1.f)};
    ToolpathRun                  run;
    run.feature  = ExtrusionFeature::support_interface;
    run.filament = 2;
    CHECK(run_color(run, LayerColorBy::feature, slots) == extrusion_feature_color(ExtrusionFeature::support_interface));
    CHECK(run_color(run, LayerColorBy::filament, slots) == slots[1]);
    run.filament = 9;  // no slot colour known: never throws, never borrows another slot's
    CHECK_FALSE(run_color(run, LayerColorBy::filament, slots) == slots[0]);
    CHECK_FALSE(run_color(run, LayerColorBy::filament, slots) == slots[1]);
}

TEST_CASE("the legend has one entry per colour drawn, with the area it drew", "[orcamcp][LayerPlan]")
{
    const std::vector<GcodeMove> moves = mixed_layer();
    const LayerExtrusion         e     = layer_extrusion(moves, gcode_layers(moves).front());
    const std::vector<ColorRGBA> slots = {ColorRGBA(1.f, 0.f, 0.f, 1.f), ColorRGBA(0.f, 0.f, 1.f, 1.f)};

    // By feature, filament 2 only: support and its interface, 4 mm2 each.
    const auto by_feature = layer_legend(sliced_request({{"layer", 1}, {"filaments", {2}}}), e, slots);
    REQUIRE(by_feature.size() == 2);
    CHECK(by_feature[0].key == "support");
    CHECK(by_feature[1].key == "support_interface");
    CHECK_THAT(by_feature[1].mm2, WithinRel(4.0, 1e-5));
    CHECK(by_feature[1].color == extrusion_feature_color(ExtrusionFeature::support_interface));

    // By filament, every feature: filament 1 drew the wall and the tower, filament 2 the support.
    const auto by_filament = layer_legend(sliced_request({{"layer", 1}, {"color_by", "filament"}}), e, slots);
    REQUIRE(by_filament.size() == 2);
    CHECK(by_filament[0].key == "1");
    CHECK_THAT(by_filament[0].mm2, WithinRel(8.0, 1e-5));
    CHECK(by_filament[1].color == slots[1]);
    CHECK_THAT(legend_json(by_filament)[1]["extruded_mm2"].get<double>(), WithinAbs(8.0, 0.005));
    CHECK(legend_json(by_filament)[1]["color"] == "#0000FF");
}

// --- on a real slice -----------------------------------------------------------------------------

namespace {

// The cap on its stem, with support in filament 2 at heights of its own, and the G-code result the
// Preview would draw.
struct SlicedCap
{
    Print                print;
    Model                model;
    GCodeProcessorResult result;

    SlicedCap()
    {
        Test::init_print({mcp_test::supported_cap()}, print, model,
                         Test::multifilament_config(2, {
                             {"enable_support", 1},
                             {"support_filament", 2},
                             {"support_interface_filament", 2},
                             {"independent_support_layer_height", 1},
                             {"support_interface_spacing", 0},
                             {"layer_height", 0.2},
                             {"initial_layer_print_height", 0.2},
                             {"enable_prime_tower", 0},
                             {"skirt_loops", 0},
                             {"brim_type", "no_brim"},
                         }));
        print.set_status_silent();
        print.process();
        ScopedTemporaryFile gcode(".gcode");
        print.export_gcode(gcode.string(), &result, nullptr);
    }
};

} // namespace

TEST_CASE("the plan's layers are the G-code's printed layers, and each is an object or support layer", "[orcamcp][LayerPlan]")
{
    SlicedCap                     cap;
    const std::vector<GcodeLayer> layers = gcode_layers(cap.result.moves);
    CHECK(layers.size() == count_print_layers(cap.print).printed);

    size_t support_only = 0;
    for (const GcodeLayer& layer : layers) {
        const LayerExtrusion              e       = layer_extrusion(cap.result.moves, layer);
        const std::vector<ObjectAtHeight> objects = objects_at_height(cap.print, cap.model, layer.z, e.extent);
        REQUIRE(objects.size() == 1);
        const ObjectAtHeight& cap_here = objects.front();
        CHECK(cap_here.object_index == 0);
        const bool printed = cap_here.object_layer.has_value() || cap_here.support_layer.has_value();
        CHECK(printed);
        if (!cap_here.object_layer.has_value()) {
            // Only support prints at this height, so only the support's filament does.
            ++support_only;
            CHECK(e.filament_order == std::vector<int>{2});
            CHECK_THAT(e.object_mm2(), WithinAbs(0., 1e-9));
        }
    }
    CHECK(support_only > 0);  // independent support heights do make some

    // Above the cap's underside nothing but the cap prints: filament 1 alone, no support.
    const size_t          top = layer_nearest_z(layers, 11.5).index;
    const LayerExtrusion  e   = layer_extrusion(cap.result.moves, layers[top]);
    CHECK(e.filament_order == std::vector<int>{1});
    CHECK_THAT(e.support_mm2(), WithinAbs(0., 1e-9));
}

TEST_CASE("the cap's underside is an overhang with interface lines under it", "[orcamcp][LayerPlan]")
{
    SlicedCap          cap;
    const PrintObject& object = *cap.print.objects().front();

    // The cap's first layer: the first one as wide as the cap.
    size_t cap_layer = 0;
    while (cap_layer < object.layers().size() && area_mm2(object.layers()[cap_layer]->lslices) < 800.)
        ++cap_layer;
    REQUIRE(cap_layer < object.layers().size());

    const double            tolerance = 0.2;  // half the 0.4 mm nozzle printing its walls
    const std::optional<Overhang> hang = overhang_of(object, cap_layer);
    REQUIRE(hang.has_value());
    // The 30 x 30 mm cap less the 8 x 8 mm stem grown by the tolerance.
    const double expected = 900. - area_mm2(offset_ex(object.layers()[cap_layer - 1]->lslices, float(scale_(tolerance))));
    CHECK_THAT(hang->area_mm2, WithinRel(expected, 0.02));
    CHECK_THAT(hang->tolerance_mm, WithinAbs(tolerance, 1e-9));
    // Directly beneath it: the support's contact, one 0.2 mm top gap down, dense interface.
    REQUIRE(hang->support_below.has_value());
    const SupportBelow& below = *hang->support_below;
    const double        bottom = object.layers()[cap_layer]->bottom_z();
    CHECK(below.z <= bottom + 1e-6);
    CHECK_THAT(below.gap_mm, WithinAbs(bottom - below.z, 1e-9));
    CHECK_THAT(below.gap_mm, WithinAbs(object.slicing_parameters().gap_support_object, 1e-6));
    CHECK(below.interface_mm2 > 0.8 * hang->area_mm2);
    CHECK(below.support_mm2 >= below.interface_mm2 - 1e-6);
    CHECK(below.support_mm2 <= hang->area_mm2 + 1e-6);

    // Half way up the stem nothing overhangs, and the first layer has nothing under it to compare.
    const std::optional<Overhang> stem = overhang_of(object, cap_layer / 2);
    REQUIRE(stem.has_value());
    CHECK(stem->area_mm2 < 1.0);
    CHECK_FALSE(overhang_of(object, 0).has_value());
}

namespace {

// The first layer of the cap: the first one as wide as the cap.
size_t cap_layer_of(const PrintObject& object)
{
    size_t index = 0;
    while (index < object.layers().size() && area_mm2(object.layers()[index]->lslices) < 800.)
        ++index;
    REQUIRE(index < object.layers().size());
    return index;
}

} // namespace

TEST_CASE("support far below an overhang is reported with its gap", "[orcamcp][LayerPlan]")
{
    // A raft and no support: the support layer beneath the cap is the raft's top, 10 mm down. The
    // raft reaches past the stem, under part of the overhang, and the gap says it touches none of it.
    Print print;
    Model model;
    Test::init_print({mcp_test::supported_cap()}, print, model, {
        {"enable_support", 0},
        {"raft_layers", 2},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
    });
    print.process();
    const PrintObject& object = *print.objects().front();
    REQUIRE_FALSE(object.support_layers().empty());
    const double raft_top = object.support_layers().back()->print_z;
    REQUIRE(raft_top < 2.0);

    const size_t                  cap  = cap_layer_of(object);
    const std::optional<Overhang> hang = overhang_of(object, cap);
    REQUIRE(hang.has_value());
    CHECK(hang->area_mm2 > 800.);
    REQUIRE(hang->support_below.has_value());
    CHECK_THAT(hang->support_below->z, WithinAbs(raft_top, 1e-6));
    CHECK_THAT(hang->support_below->gap_mm, WithinAbs(object.layers()[cap]->bottom_z() - raft_top, 1e-6));
    CHECK(hang->support_below->gap_mm > 8.);
}

TEST_CASE("tree support's interface sits one top gap under the cap", "[orcamcp][LayerPlan]")
{
    Print print;
    Model model;
    Test::init_print({mcp_test::supported_cap()}, print, model, {
        {"enable_support", 1},
        {"support_type", "tree(auto)"},
        {"support_top_z_distance", 0.2},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
    });
    print.process();
    const PrintObject&            object = *print.objects().front();
    const std::optional<Overhang> hang   = overhang_of(object, cap_layer_of(object));
    REQUIRE(hang.has_value());
    REQUIRE(hang->support_below.has_value());
    CHECK_THAT(hang->support_below->gap_mm, WithinAbs(0.2, 1e-6));
    CHECK(hang->support_below->interface_mm2 > 0.);
    CHECK(hang->support_below->support_mm2 >= hang->support_below->interface_mm2 - 1e-6);
}

TEST_CASE("an overhang with no support layer below it says so", "[orcamcp][LayerPlan]")
{
    Print print;
    Model model;
    Test::init_print({mcp_test::supported_cap()}, print, model, {
        {"enable_support", 0},
        {"raft_layers", 0},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
    });
    print.process();
    const PrintObject& object = *print.objects().front();
    REQUIRE(object.support_layers().empty());
    const std::optional<Overhang> hang = overhang_of(object, cap_layer_of(object));
    REQUIRE(hang.has_value());
    CHECK(hang->area_mm2 > 800.);
    CHECK_FALSE(hang->support_below.has_value());
}

TEST_CASE("the overhang tolerance is half the nozzle that prints the object's walls", "[orcamcp][LayerPlan]")
{
    // Two nozzles, 0.4 and 0.8 mm, the walls on the second.
    Print print;
    Model model;
    Test::init_print({mcp_test::supported_cap()}, print, model,
                     Test::multifilament_config(2, {
                         {"nozzle_diameter", "0.4,0.8"},
                         {"printer_extruder_id", "1,2"},
                         {"printer_extruder_variant", "Direct Drive Standard,Direct Drive Standard"},
                         {"extruder_printable_height", "0,0"},
                         {"single_extruder_multi_material", 0},
                         {"outer_wall_filament_id", 2},
                         {"inner_wall_filament_id", 2},
                         {"enable_prime_tower", 0},
                         {"layer_height", 0.2},
                         {"initial_layer_print_height", 0.2},
                     }));
    print.process();
    const PrintObject&            object = *print.objects().front();
    const std::optional<Overhang> hang   = overhang_of(object, cap_layer_of(object));
    REQUIRE(hang.has_value());
    CHECK_THAT(hang->tolerance_mm, WithinAbs(0.4, 1e-9));
}

TEST_CASE("an object's frame on a layer takes in its brim and its support lines", "[orcamcp][LayerPlan]")
{
    // A 10 mm cube with a 5 mm brim: on the first layer the frame reaches the brim's edge.
    Print print;
    Model model;
    Test::init_print({Test::cube(10)}, print, model, {
        {"brim_type", "outer_only"},
        {"brim_width", 5},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
    });
    print.process();
    const BoundingBoxf3 cube = model.objects[0]->instance_bounding_box(0);
    const std::optional<BoundingBoxf> first = object_frame(print, model, 0, first_print_height(print));
    REQUIRE(first.has_value());
    CHECK(first->min.x() <= cube.min.x() - 4.5);
    CHECK(first->max.y() >= cube.max.y() + 4.5);
    // Higher up there is no brim, and the frame is the cube's.
    const std::optional<BoundingBoxf> mid = object_frame(print, model, 0, 5.0);
    REQUIRE(mid.has_value());
    CHECK_THAT(mid->min.x(), WithinAbs(cube.min.x(), 0.5));
    CHECK_THAT(mid->max.y(), WithinAbs(cube.max.y(), 0.5));
    CHECK_FALSE(object_frame(print, model, 7, 5.0).has_value());  // no such object
}

TEST_CASE("an object's footprint comes from the sliced object, where the model puts it", "[orcamcp][LayerPlan]")
{
    SlicedCap cap;
    const std::vector<PrintFootprint> footprints = print_footprints(cap.print, cap.model);
    REQUIRE(footprints.size() == 1);
    CHECK(footprints[0].object_index == 0);
    const BoundingBoxf3 model_box = cap.model.objects[0]->instance_bounding_box(0);
    CHECK_THAT(footprints[0].box.min.x(), WithinAbs(model_box.min.x(), 0.01));
    CHECK_THAT(footprints[0].box.min.y(), WithinAbs(model_box.min.y(), 0.01));
    CHECK_THAT(footprints[0].box.max.x(), WithinAbs(model_box.max.x(), 0.01));
    CHECK_THAT(footprints[0].box.max.y(), WithinAbs(model_box.max.y(), 0.01));
}

// Hidden ([.]): a timing, not a check. The plan runs on the GUI thread, so what it costs on a long
// print matters: 1230 layers of about 4000 moves, a large model's G-code.
TEST_CASE("the layer plan's arithmetic over a 1230-layer G-code", "[.][Benchmark][orcamcp][LayerPlan]")
{
    std::vector<GcodeMove> moves;
    const unsigned         n_layers = 1230, per_layer = 4000;
    moves.reserve(size_t(n_layers) * per_layer);
    for (unsigned id = 0; id < n_layers; ++id) {
        const double z = 0.08 * (id + 1);
        for (unsigned i = 0; i < per_layer; ++i)
            moves.push_back(i % 10 == 0 ? move_to(id, i % 97, i % 89, z)
                                        : extrude_to(id, i % 97, i % 89, z, i % 3 ? erInternalInfill : erSupportMaterial, 1 + i % 2));
    }
    const auto started = std::chrono::steady_clock::now();
    const std::vector<GcodeLayer> layers  = gcode_layers(moves);
    const LayerAtHeight           hit     = layer_nearest_z(layers, 49.2);
    const LayerExtrusion          e       = layer_extrusion(moves, layers[hit.index]);
    const auto runs = layer_toolpaths(moves, layers[hit.index], sliced_request({{"z", 49.2}}));
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    WARN("index + pick + areas + runs over " << moves.size() << " moves: " << elapsed << " ms");
    CHECK(layers.size() == n_layers);
    CHECK_FALSE(runs.empty());
    CHECK(e.object_mm2() > 0.);
}
