#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateAnalysis.hpp"
#include <cmath>
#include <limits>
#include <memory>
#include <vector>
#include <map>
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "test_utils.hpp"
#include <fstream>
#include <boost/filesystem/path.hpp>

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using nlohmann::json;
using Catch::Matchers::WithinAbs;

namespace {
struct ControlValues : ConfigOptionResolver {
    std::map<std::string, const ConfigOption*> values;
    const ConfigOption* optptr(const t_config_option_key& key) const override {
        const auto it = values.find(key); return it == values.end() ? nullptr : it->second;
    }
};
GeometryIdentity analysis_id() { return {"session", "plate", 0, 123, "app", "server", nullptr}; }
json detail_request(size_t budget = 10000) { return {{"plate_index", 0}, {"section", "detail"}, {"max_moves", budget}}; }
GCodeProcessorResult::MoveVertex record(size_t index, bool internal = false)
{
    GCodeProcessorResult::MoveVertex m;
    m.gcode_id = unsigned(index); m.layer_id = 0; m.internal_only = internal;
    m.type = index ? EMoveType::Extrude : EMoveType::Noop;
    m.extrusion_role = erSupportMaterialInterface; m.extruder_id = 3;
    m.position = Vec3f(float(index), 0.f, 0.2f); m.width = 0.4f; m.height = 0.2f; m.delta_extruder = 0.5f;
    return m;
}
std::unique_ptr<GCodeProcessorResult> fixture(size_t count = 10)
{
    auto owned = std::make_unique<GCodeProcessorResult>();
    auto& r = *owned;
    r.id = 123; r.filaments_count = 4;
    r.filament_diameters = {1.75f, 1.75f, 1.75f, 1.75f};
    r.filament_densities = {1.24f, 1.24f, 1.24f, 1.24f};
    r.filament_costs = {25.f, 25.f, 25.f, 25.f};
    r.settings_ids.filament = {"one", "two", "three", "four"};
    r.print_statistics.total_filament_changes = 1;
    r.print_statistics.total_extruder_changes = 0;
    for (size_t i = 0; i < count; ++i) r.moves.push_back(record(i));
    return owned;
}
json analysis(const GCodeProcessorResult& r, const json& p)
{
    return plate_analysis_page(r, parse_plate_analysis_request(p), analysis_id(), analysis_controls(ControlValues{}));
}
}

TEST_CASE("Analysis requests accept integral floats and reject ambiguous section parameters", "[PlateAnalysis]")
{
    CHECK(parse_plate_analysis_request({{"plate_index", 3.0}}).plate_index == 3);
    for (const auto& p : std::vector<json>{ {{"plate_index", true}}, {{"plate_index", 0.5}},
        {{"plate_index", -1}}, {{"plate_index", 1e20}}, {{"plate_index", 0}, {"max_moves", 5}},
        {{"plate_index", 0}, {"section", "other"}}, {{"plate_index", 0}, {"section", "detail"}, {"max_moves", 4}},
        {{"plate_index", 0}, {"section", "detail"}, {"max_moves", 10001}}, {{"plate_index", 0}, {"unknown", 1}} })
        CHECK_THROWS_AS(parse_plate_analysis_request(p), GeometryError);
}

TEST_CASE("Accounting admission requires both native process steps", "[PlateAnalysis]")
{
    AnalysisSourceState state{false, false, true, true, true, true};
    CHECK_NOTHROW(require_analysis_source(state));
    for (int field = 0; field < 6; ++field) {
        auto unsupported = state;
        if (field == 0) unsupported.only_gcode = true;
        if (field == 1) unsupported.gcode_3mf = true;
        if (field == 2) unsupported.has_print = false;
        if (field == 3) unsupported.has_objects = false;
        if (field == 4) unsupported.slicing_finished = false; // Loaded 3mf, including cached moves.
        if (field == 5) unsupported.export_finished = false;
        CHECK_THROWS_AS(require_analysis_source(unsupported), GeometryError);
    }
}

TEST_CASE("Physical analysis ignores interpolated copies for area count and signed filament E", "[PlateAnalysis]")
{
    auto owned = fixture(4); auto& r = *owned;
    r.moves[1].internal_only = true; r.moves[1].width = 0.1f; r.moves[1].delta_extruder = 100.f;
    r.moves[2].internal_only = true; r.moves[2].width = 0.2f; r.moves[2].delta_extruder = 200.f;
    auto out = analysis(r, detail_request(5));
    CHECK(out["inspected_moves"] == 5);
    REQUIRE(out["occupancy_parts"].size() == 1);
    const auto& row = out["occupancy_parts"][0];
    CHECK(row["segment_count"] == 1);
    CHECK(row["first_move"] == 3);
    CHECK(row["filament"] == 4);
    CHECK(row["raw_role_name"] == "erSupportMaterialInterface");
    CHECK_THAT(row["swept_area_xy_mm2"].get<double>(), WithinAbs(3. * double(r.moves[3].width), 1e-12));
    CHECK_THAT(row["extruded_filament_mm"].get<double>(), WithinAbs(0.5, 1e-12));
    CHECK(out["has_more"] == false);
}

TEST_CASE("Detail pages stop only after a physical record and count discarded internal suffixes", "[PlateAnalysis]")
{
    auto owned = fixture(7); auto& r = *owned;
    r.moves[2].internal_only = true; r.moves[3].internal_only = true;
    auto p = detail_request(5);
    auto first = analysis(r, p);
    CHECK(first["range"]["end_move_exclusive"] == 2);
    CHECK(first["inspected_moves"] == 5);
    CHECK(first["next_cursor"]["next_move"] == 2);
    p["cursor"] = first["next_cursor"];
    auto second = analysis(r, p);
    CHECK(second["range"]["begin_move"] == 2);
    CHECK(second["range"]["end_move_exclusive"] == 5);
    CHECK(second["occupancy_parts"][0]["first_move"] == 4);
    p["cursor"] = second["next_cursor"];
    CHECK(analysis(r, p)["has_more"] == false);
    r.moves[1].internal_only = true;
    CHECK_THROWS_AS(analysis(r, detail_request(5)), GeometryError);
}

TEST_CASE("Detail cursor shape is checked before changed-result bounds", "[PlateAnalysis]")
{
    auto owned = fixture(10); auto& r = *owned; auto p = detail_request(5);
    p["cursor"] = analysis(r, p)["next_cursor"];
    r.moves.resize(2);
    try { analysis(r, p); FAIL("Expected stale_slice"); }
    catch (const GeometryError& e) { CHECK(e.code == "stale_slice"); }
    r = std::move(*fixture(10)); p["cursor"] = analysis(r, detail_request(5))["next_cursor"];
    p["cursor"]["next_move"] = r.moves.size();
    try { analysis(r, p); FAIL("Expected invalid_arguments"); }
    catch (const GeometryError& e) { CHECK(e.code == "invalid_arguments"); }
    p["cursor"]["next_move"] = std::ldexp(1., 64);
    CHECK_THROWS_AS(parse_plate_analysis_request(p), GeometryError);
    p["cursor"]["next_move"] = 3; r.moves[2].internal_only = true;
    CHECK_THROWS_AS(analysis(r, p), GeometryError);
}

TEST_CASE("Summary preserves native categories without adding flush to total twice", "[PlateAnalysis]")
{
    auto owned = fixture(); auto& r = *owned;
    r.print_statistics.model_volumes_per_extruder = {{1, 100.}};
    r.print_statistics.support_volumes_per_extruder = {{1, 20.}};
    r.print_statistics.flush_per_filament = {{1, 5.}};
    r.print_statistics.total_volumes_per_extruder = {{1, 125.}, {3, 10.}};
    auto out = analysis(r, {{"plate_index", 0}});
    CHECK(out["native_estimate"]["filament"]["total_volume_mm3"] == 135.);
    CHECK(out["material_categories"].size() == 4);
    CHECK(out["material_category_totals"]["total"]["volume_mm3"]["value"] == 135.);
    CHECK(out["material_category_totals"]["flush"]["volume_mm3"]["value"] == 5.);
    CHECK(out["material_categories"][0]["total"]["volume_mm3"]["availability"] == "recorded_zero");
    CHECK(out["pricing"]["currency"].is_null());
    CHECK(out["native_estimate"]["estimated_time_silent_seconds"].is_null());
    CHECK(out["has_more"] == false);
}

TEST_CASE("Missing or invalid positive-volume properties remain unknown while zero stays known", "[PlateAnalysis]")
{
    auto owned = fixture(); auto& r = *owned; r.print_statistics.total_volumes_per_extruder = {{1, 10.}};
    r.filament_densities[1] = 0.f; r.filament_costs[1] = -1.f;
    auto out = analysis(r, {{"plate_index", 0}});
    CHECK(out["native_estimate"]["filament"]["total_weight_grams"].is_null());
    CHECK(out["native_estimate"]["filament"]["total_cost"].is_null());
    CHECK(out["material_categories"][1]["total"]["weight_g"]["reason"] == "density_nonpositive");
    CHECK(out["material_categories"][0]["total"]["weight_g"]["value"] == 0);
    CHECK(out["profile_inputs"]["slots"][1]["price_status"] == "invalid");
    r.filament_densities[1] = 1.24f; r.filament_costs[1] = 0.f;
    out = analysis(r, {{"plate_index", 0}});
    CHECK(out["native_estimate"]["filament"]["total_cost"] == 0.);
    CHECK(out["profile_inputs"]["slots"][1]["price_status"] == "zero");
    r.print_statistics.total_volumes_per_extruder[1] = -1.;
    CHECK_THROWS_AS(analysis(r, {{"plate_index", 0}}), GeometryError);
}

TEST_CASE("Nullable native nozzle controls preserve nil elements and typed values", "[PlateAnalysis]")
{
    ControlValues config;
    ConfigOptionFloatsNullable nozzle;
    nozzle.values = {107., std::numeric_limits<double>::quiet_NaN()};
    ConfigOptionFloats multiplier;
    multiplier.values = {0.75};
    ConfigOptionBool tower(true);
    ConfigOptionFloat time_cost(30.);
    config.values = {{"nozzle_volume", &nozzle}, {"flush_multiplier", &multiplier},
        {"enable_prime_tower", &tower}, {"time_cost", &time_cost}};
    auto controls = analysis_controls(config);
    CHECK(controls.size() == 7);
    CHECK(controls["nozzle_volume"]["value"] == json::array({107., nullptr}));
    CHECK(controls["nozzle_volume"]["reason"] == "contains_nil");
    CHECK(controls["nozzle_volume"]["availability"] == "recorded");
    CHECK(controls["flush_multiplier"]["value"] == json::array({0.75}));
    CHECK(controls["enable_prime_tower"]["value"] == true);
    CHECK(controls["prime_volume_mode"]["value"].is_null());
    CHECK(controls["prime_volume_mode"]["reason"] == "not_in_print_config");
    nozzle.values = {std::numeric_limits<double>::quiet_NaN()};
    CHECK(analysis_controls(config)["nozzle_volume"]["value"] == json::array({nullptr}));
    auto owned = fixture(); auto& r = *owned;
    r.print_statistics.total_volumes_per_extruder = {{1, 100.}};
    auto with_time = plate_analysis_page(r, parse_plate_analysis_request({{"plate_index", 0}}), analysis_id(), controls);
    CHECK(with_time["pricing"]["cost_includes_time_cost"] == false);
    CHECK(with_time["native_estimate"]["filament"]["total_cost"] == analysis(r, {{"plate_index", 0}})["native_estimate"]["filament"]["total_cost"]);
}

TEST_CASE("Detail byte limits commit all reducers atomically and include timing fields", "[PlateAnalysis]")
{
    auto owned = fixture(4000); auto& r = *owned;
    for (size_t i = 1; i < r.moves.size(); ++i) {
        r.moves[i].type = EMoveType::Tool_change;
        r.moves[i].layer_id = unsigned(i);
    }
    const auto out = analysis(r, detail_request());
    CHECK(out.dump().size() <= 65536);
    CHECK(out["has_more"] == true);
    const size_t end = out["range"]["end_move_exclusive"];
    CHECK(out["events"].size() == end - 1);
    CHECK(out["layer_parts"].size() == end - 1);
    CHECK(out["occupancy_parts"].empty());
    CHECK(out["timing_ms"].size() == 3);
    CHECK(out["inspected_moves"].get<size_t>() <= 10000);
    CHECK(out["next_cursor"]["next_move"] == end);
}

TEST_CASE("Travel-only physical pages advance and arbitrary internal runs refuse without progress", "[PlateAnalysis]")
{
    auto owned = fixture(10020); auto& r = *owned;
    for (auto& m : r.moves) m.type = EMoveType::Travel;
    auto out = analysis(r, detail_request(5));
    CHECK(out["range"]["end_move_exclusive"] == 4);
    CHECK(out["occupancy_parts"].empty());
    CHECK(out["events"].empty());
    CHECK(out["has_more"] == true);
    for (size_t i = 1; i + 1 < r.moves.size(); ++i) r.moves[i].internal_only = true;
    try { analysis(r, detail_request(5)); FAIL("Expected output_limit"); }
    catch (const GeometryError& e) { CHECK(e.code == "output_limit"); CHECK(std::string(e.what()).find("increase max_moves") != std::string::npos); }
    try { analysis(r, detail_request(10000)); FAIL("Expected output_limit"); }
    catch (const GeometryError& e) { CHECK(e.code == "output_limit"); CHECK(std::string(e.what()).find("cannot be paged") != std::string::npos); }
}

TEST_CASE("Unsupported native move sources fail without coercing roles or geometric values", "[PlateAnalysis]")
{
    for (int field = 0; field < 5; ++field) {
        auto owned = fixture(3); auto& r = *owned;
        if (field == 0) r.moves[2].internal_only = true;
        if (field == 1) r.moves[1].width = std::numeric_limits<float>::infinity();
        if (field == 2) r.moves[1].extrusion_role = static_cast<ExtrusionRole>(erCount);
        if (field == 3) { r.moves[1].layer_id = 2; r.moves[2].layer_id = 1; }
        if (field == 4) r.filaments_count = 65;
        try { analysis(r, detail_request()); FAIL("Expected unsupported_source"); }
        catch (const GeometryError& e) { CHECK(e.code == "unsupported_source"); }
    }
}

TEST_CASE("Summary metadata overflow refuses instead of truncating profiles", "[PlateAnalysis]")
{
    auto owned = fixture(); auto& r = *owned;
    r.settings_ids.filament[0].assign(65536, 'x');
    try { analysis(r, {{"plate_index", 0}}); FAIL("Expected output_limit"); }
    catch (const GeometryError& e) { CHECK(e.code == "output_limit"); }
}

TEST_CASE("Paged occupancy matches an independent physical-record reduction", "[PlateAnalysis]")
{
    auto owned = fixture(601); auto& r = *owned;
    double xyz = 0., xy = 0., swept = 0., e = 0.; size_t segments = 0, previous = 0, event_count = 0;
    for (size_t i = 1; i < r.moves.size(); ++i) {
        auto& m = r.moves[i];
        m.internal_only = i % 4 == 1 || i % 4 == 2;
        m.layer_id = unsigned(i / 80);
        m.position.z() = float(i % 80) * 0.002f + float(i / 80) * 0.2f;
        m.width = float(i % 3) * 0.05f + 0.3f;
        m.delta_extruder = i % 9 == 0 ? -0.1f : 0.2f;
        m.extruder_id = i % 3 == 0 ? 1 : 3;
        m.extrusion_role = i % 7 == 0 ? erSupportTransition : erSupportMaterialInterface;
        if (i % 13 == 0) m.type = EMoveType::Tool_change;
        if (i % 17 == 0) m.type = EMoveType::Travel;
        if (m.internal_only) continue;
        if (m.type == EMoveType::Tool_change) ++event_count;
        if (m.type == EMoveType::Extrude) {
            const double dx = double(m.position.x()) - r.moves[previous].position.x();
            const double dy = double(m.position.y()) - r.moves[previous].position.y();
            const double dz = double(m.position.z()) - r.moves[previous].position.z();
            const double projected = std::hypot(dx, dy);
            xyz += std::sqrt(dx * dx + dy * dy + dz * dz); xy += projected;
            swept += projected * double(m.width); e += m.delta_extruder; ++segments;
        }
        previous = i;
    }
    auto p = detail_request(17); size_t pages = 0, actual_segments = 0, actual_events = 0, expected_begin = 1;
    double actual_xyz = 0., actual_xy = 0., actual_swept = 0., actual_e = 0.;
    do {
        const auto out = analysis(r, p);
        CHECK(out["range"]["begin_move"] == expected_begin);
        CHECK(out["inspected_moves"].get<size_t>() <= 17);
        expected_begin = out["range"]["end_move_exclusive"];
        REQUIRE(expected_begin > out["range"]["begin_move"].get<size_t>());
        for (const auto& row : out["occupancy_parts"]) {
            actual_segments += row["segment_count"].get<size_t>();
            actual_xyz += row["path_length_xyz_mm"].get<double>(); actual_xy += row["projected_length_xy_mm"].get<double>();
            actual_swept += row["swept_area_xy_mm2"].get<double>(); actual_e += row["extruded_filament_mm"].get<double>();
            CHECK(row["unknown"].empty());
            CHECK(row["filament"].get<int>() >= 2);
        }
        actual_events += out["events"].size();
        REQUIRE(++pages < r.moves.size());
        if (!out["has_more"].get<bool>()) break;
        p["cursor"] = out["next_cursor"];
        CHECK_FALSE(r.moves[expected_begin - 1].internal_only);
    } while (true);
    CHECK(actual_segments == segments); CHECK(actual_events == event_count); CHECK(expected_begin == r.moves.size());
    CHECK_THAT(actual_xyz, WithinAbs(xyz, 1e-9)); CHECK_THAT(actual_xy, WithinAbs(xy, 1e-9));
    CHECK_THAT(actual_swept, WithinAbs(swept, 1e-9)); CHECK_THAT(actual_e, WithinAbs(e, 1e-9));
}

TEST_CASE("Ordered native events preserve type slot and raw source metadata", "[PlateAnalysis]")
{
    auto owned = fixture(5); auto& r = *owned;
    r.moves[1].type = EMoveType::Tool_change; r.moves[1].extruder_id = 3;
    r.moves[2].type = EMoveType::Color_change; r.moves[2].cp_color_id = 7;
    r.moves[3].type = EMoveType::Tool_change; r.moves[3].extruder_id = 1;
    auto out = analysis(r, detail_request());
    REQUIRE(out["events"].size() == 3);
    CHECK(out["events"][0]["type"] == "tool_change");
    CHECK(out["events"][0]["native_type"] == "Tool_change");
    CHECK(out["events"][1]["type"] == "color_change");
    CHECK(out["events"][1]["cp_color_id"] == 7);
    CHECK(out["events"][2]["filament"] == 2);
    for (size_t i = 0; i < out["events"].size(); ++i) {
        CHECK(out["events"][i]["move_index"] == i + 1);
        CHECK(out["events"][i]["native_gcode_id"] == i + 1);
        CHECK(out["events"][i].size() == 8);
        CHECK_FALSE(out["events"][i].contains("previous_filament"));
        CHECK_FALSE(out["events"][i].contains("physical_extruder"));
    }
}

TEST_CASE("Seam records remain physical predecessors and vase layer heights are not flattened", "[PlateAnalysis]")
{
    auto owned = fixture(4); auto& r = *owned;
    r.moves[1].type = EMoveType::Seam; r.moves[1].position.x() = 0.125f;
    r.moves[2].position.z() = 0.3f; r.moves[3].position.z() = 0.4f;
    r.moves[3].layer_id = 1;
    const auto out = analysis(r, detail_request());
    REQUIRE(out["occupancy_parts"].size() == 2);
    CHECK_THAT(out["occupancy_parts"][0]["projected_length_xy_mm"].get<double>(), WithinAbs(1.875, 1e-12));
    CHECK(out["layer_parts"][0]["last_supported_endpoint_z_mm"] == double(r.moves[2].position.z()));
    CHECK(out["layer_parts"][1]["last_supported_endpoint_z_mm"] == double(r.moves[3].position.z()));
    CHECK(out["occupancy_parts"][1]["z_min_mm"] == double(r.moves[2].position.z()));
}

TEST_CASE("Profile availability records missing and nonfinite inputs without hiding unused slots", "[PlateAnalysis]")
{
    auto owned = fixture(); auto& r = *owned;
    r.filament_diameters.resize(1); r.filament_densities[1] = std::numeric_limits<float>::quiet_NaN();
    r.filament_costs[1] = std::numeric_limits<float>::infinity(); r.settings_ids.filament.resize(1);
    r.print_statistics.total_volumes_per_extruder = {{1, 5.}};
    const auto out = analysis(r, {{"plate_index", 0}});
    REQUIRE(out["profile_inputs"]["slots"].size() == 4);
    const auto& slot = out["profile_inputs"]["slots"][1];
    CHECK(slot["unavailable"]["preset"] == "missing");
    CHECK(slot["unavailable"]["diameter_mm"] == "missing");
    CHECK(slot["unavailable"]["density_g_cm3"] == "nonfinite");
    CHECK(slot["unavailable"]["price_per_kg"] == "nonfinite");
    CHECK(out["material_categories"][1]["total"]["length_mm"]["reason"] == "diameter_missing");
    CHECK(out["material_categories"][2]["total"]["length_mm"]["value"] == 0.);
    CHECK(out["native_estimate"]["filament"]["per_filament"].size() == 1);
}

TEST_CASE("Geometry and plate analysis share completed-result identity", "[PlateAnalysis]")
{
    auto owned = fixture(); auto& r = *owned;
    const auto summary = analysis(r, {{"plate_index", 0}});
    const auto geometry = layer_geometry_page(r.moves, gcode_layers(r.moves),
        parse_layer_geometry_request({{"plate_index", 0}, {"layer", 1}}), analysis_id(), 4);
    CHECK(summary["result_id"] == geometry["result_id"]);
    auto changed = analysis_id(); changed.session = "another-session";
    const auto replacement = plate_analysis_page(r, parse_plate_analysis_request({{"plate_index", 0}}), changed, analysis_controls(ControlValues{}));
    CHECK(replacement["result_id"] != summary["result_id"]);
}


// Native processor integration: when the next extrusion is processed, OptionsZCorrector moves
// the marker to the last recorded endpoint before storing that extrusion. Two intervening travels
// make relocation observable beyond the native comment-line-plus-one gcode_id convention. Preserve native order and monotonic admission unchanged.
TEST_CASE("Native color correction retains physical event order and valid layer admission", "[PlateAnalysis][NativeProcessor]")
{
    const std::string original_resources = resources_dir();
    const ScopeGuard restore_resources([original_resources] { set_resources_dir(original_resources); });
    set_resources_dir((boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources").string());
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
    config.filament_diameter.values = {1.75, 1.75};
    const bool prior_bbl = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore([prior_bbl] { GCodeProcessor::s_IsBBLPrinter = prior_bbl; });
    GCodeProcessor::s_IsBBLPrinter = false;
    ScopedTemporaryFile file(".gcode");
    std::ofstream(file.string()) << "M83\nG90\nT0\n;LAYER_CHANGE\nG1 Z0.2 F3000\n"
        ";TYPE:External perimeter\nG1 X10 Y0 E1 F1200\n;LAYER_CHANGE\nG1 Z0.4\n"
        ";COLOR_CHANGE,T0,#FF0000\nG1 X15 Y0 Z0.4 F3000\nG1 X17 Y0 Z0.4 F3000\nG1 X20 Y0 E1 F1200\n;COLOR_CHANGE,T1,#00FF00\nG1 X25 Y0 E1\n";
    GCodeProcessor processor;
    processor.apply_config(config);
    processor.process_file(file.string());
    const auto& r = processor.get_result();
    auto p = detail_request();
    auto out = analysis(r, p);
    constexpr const char* move_names[] = {"Noop", "Retract", "Unretract", "Seam", "Tool_change", "Color_change",
        "Pause_Print", "Custom_GCode", "Travel", "Wipe", "Extrude"};
    json sequence = json::array();
    for (size_t i = 0; i < r.moves.size(); ++i) {
        const auto& m = r.moves[i];
        sequence.push_back({{"move_index", i}, {"layer", uint64_t(m.layer_id) + 1},
            {"native_type", move_names[size_t(m.type)]}, {"native_gcode_id", m.gcode_id},
            {"internal_only", m.internal_only}, {"filament", int(m.extruder_id) + 1},
            {"position_xyz", {m.position.x(), m.position.y(), m.position.z()}}});
    }
    INFO("native_color_sequence=" << sequence.dump());
    size_t colors = 0, previous_physical = 0;
    for (size_t i = 1; i < r.moves.size(); ++i) {
        if (r.moves[i].internal_only) continue;
        if (r.moves[i].type == EMoveType::Color_change) {
            ++colors;
            CHECK(r.moves[previous_physical].type == EMoveType::Travel);
            CHECK(r.moves[previous_physical].gcode_id > r.moves[i].gcode_id);
            CHECK(json(r.moves[i].position.x()) == json(17.));
            CHECK(json(r.moves[i].position.z()) == json(r.moves[previous_physical].position.z()));
            size_t following = i + 1;
            while (following < r.moves.size() && r.moves[following].internal_only) ++following;
            REQUIRE(following < r.moves.size());
            CHECK(r.moves[following].type == EMoveType::Extrude);
            CHECK(json(r.moves[following].position.x()) == json(20.));
        }
        previous_physical = i;
    }
    CHECK(colors == 1); // Inactive-target T1 colour command does not produce a native Color_change.
    size_t returned = 0;
    for (const auto& event : out["events"])
        if (event["type"] == "color_change") {
            ++returned;
            const size_t index = event["move_index"];
            CHECK(r.moves[index].type == EMoveType::Color_change);
            CHECK(event["layer"] == uint64_t(r.moves[index].layer_id) + 1);
            CHECK(event["native_gcode_id"] == r.moves[index].gcode_id);
            CHECK(event["native_z_mm"] == double(r.moves[index].position.z()));
        }
    CHECK(returned == colors);
}
