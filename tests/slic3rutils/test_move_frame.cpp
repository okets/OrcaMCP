#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/OrcaMCP/OrcaMCPMoveFrame.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPLayerGeometry.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateAnalysis.hpp"
#include <limits>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using nlohmann::json;
namespace {
MoveFrameProvenance captured(size_t slots = 2)
{
    MoveFrameProvenance frame;
    frame.captured = true; frame.basis = MoveFrameBasis::StreamingExport;
    frame.config_kind = MoveFrameConfigKind::TypedPrintConfig;
    frame.classification = MoveFrameClassification::OrdinaryNonbeltTranslation;
    frame.reason = MoveFrameReason::None;
    frame.belt_config_known = true; frame.machine_transform_known = true;
    frame.xy_mm = {{256.125, -33.0625}}; frame.z_offset_mm = 0.1f; frame.first_layer_height_mm = 0.2f;
    frame.extruder_offsets_mm.assign(slots, Vec3f(1.25f, -2.5f, 0.125f));
    return frame;
}
std::vector<GcodeMove> moves()
{
    std::vector<GcodeMove> result(20);
    for (size_t i = 0; i < result.size(); ++i) {
        result[i].type = i ? EMoveType::Extrude : EMoveType::Noop;
        result[i].extrusion_role = erSupportMaterialInterface;
        result[i].position = Vec3f(float(i), 2.f, 0.2f);
        result[i].width = 0.4f;
    }
    return result;
}
GeometryIdentity identity() { return {"session", "plate", 2, 123, "app", "server", nullptr, captured()}; }
}

TEST_CASE("Captured frame wire values retain exact native float and double inputs", "[MoveFrame]")
{
    const auto frame = captured();
    const auto out = coordinate_frame_json(frame, "result");
    CHECK(out.size() == 10);
    CHECK(out["binding"] == "captured_with_moves");
    CHECK(out["normalization"]["availability"] == "available");
    CHECK(out["normalization"]["reason"].is_null());
    CHECK(out["processor_offsets"]["xy_mm"] == json::array({256.125, -33.0625}));
    CHECK(out["processor_offsets"]["z_offset_mm"] == double(frame.z_offset_mm));
    CHECK(out["processor_offsets"]["extruder_offsets_mm"][0][2] == double(frame.extruder_offsets_mm[0].z()));
    CHECK(out["start_custom_z"]["first_layer_height_mm"] == double(frame.first_layer_height_mm));
    CHECK(out["frame_digest"].get<std::string>().size() == 40);
    CHECK(out["result_id"] == "result");
}

TEST_CASE("Unavailable frame metadata never turns missing offsets into zeros", "[MoveFrame]")
{
    const auto out = coordinate_frame_json({}, "result");
    CHECK(out["binding"] == "unavailable");
    CHECK(out["normalization"]["reason"] == "not_captured");
    CHECK(out["processor_offsets"].is_null()); CHECK(out["start_custom_z"].is_null());
}

TEST_CASE("Frame domain refusals are explicit and preserve only valid captured diagnostics", "[MoveFrame]")
{
    for (int change = 0; change < 6; ++change) {
        auto frame = captured();
        if (change == 0) frame.extruder_offsets_mm.resize(65);
        if (change == 1) frame.xy_mm[0] = std::numeric_limits<double>::infinity();
        if (change == 2) frame.config_kind = MoveFrameConfigKind::NoneOrDefaulted;
        if (change == 3) { frame.config_kind = MoveFrameConfigKind::ExplicitGcodeConfigBlock; frame.explicit_frame_keys = 2; }
        if (change == 4) frame.belt_configured = true;
        if (change == 5) { frame.source_file_state = MoveFrameSourceFileState::ReloadFailed; frame.reason = MoveFrameReason::ReloadFailedSourceMismatch; }
        const auto out = coordinate_frame_json(frame, "result");
        CHECK(out["normalization"]["availability"] == "unavailable");
        CHECK(out["normalization"]["kind"].is_null());
        if (change < 4) CHECK(out["processor_offsets"].is_null());
        else CHECK_FALSE(out["processor_offsets"].is_null());
    }
}

TEST_CASE("Every captured frame input independently fences the canonical digest", "[MoveFrame]")
{
    const auto initial = captured();
    const auto baseline = coordinate_frame_json(initial, "result")["frame_digest"];
    CHECK(coordinate_frame_json(initial, "other-result")["frame_digest"] == baseline);
    for (int change = 0; change < 12; ++change) {
        auto frame = initial;
        if (change == 0) frame.xy_mm[0] += 1.;
        if (change == 1) frame.z_offset_mm += 0.01f;
        if (change == 2) frame.extruder_offsets_mm[1].y() += 1.f;
        if (change == 3) frame.first_layer_height_mm += 0.01f;
        if (change == 4) frame.source_file_state = MoveFrameSourceFileState::ReloadSucceeded;
        if (change == 5) frame.belt_header_observed = true;
        if (change == 6) frame.belt_tilt_angle = 45.f;
        if (change == 7) frame.belt_z_origin = 1.f;
        if (change == 8) frame.basis = MoveFrameBasis::FileReprocess;
        if (change == 9) frame.explicit_frame_keys = 15;
        if (change == 10) frame.machine_transform_active = true;
        if (change == 11) frame.classification = MoveFrameClassification::UnsupportedTransform;
        CHECK(coordinate_frame_json(frame, "result")["frame_digest"] != baseline);
    }
}

TEST_CASE("Geometry v3 rejects a metadata-only cursor mutation under the same result identity", "[MoveFrame]")
{
    const auto native_moves = moves(); const auto layers = gcode_layers(native_moves);
    auto id = identity();
    json params = {{"plate_index", 2}, {"layer", 1}, {"max_points", 2}};
    const auto first = layer_geometry_page(native_moves, layers, parse_layer_geometry_request(params), id, 2);
    params["cursor"] = first["next_cursor"];
    CHECK_NOTHROW(layer_geometry_page(native_moves, layers, parse_layer_geometry_request(params), id, 2));
    id.move_frame.xy_mm[0] += 1.;
    try { layer_geometry_page(native_moves, layers, parse_layer_geometry_request(params), id, 2); FAIL("Expected stale_slice"); }
    catch (const GeometryError& e) { CHECK(e.code == "stale_slice"); }
    params.erase("cursor");
    const auto changed = layer_geometry_page(native_moves, layers, parse_layer_geometry_request(params), id, 2);
    CHECK(changed["result_id"] == first["result_id"]);
    CHECK(changed["layer_digest"] != first["layer_digest"]);
}

TEST_CASE("Maximum bounded frame metadata fits paged geometry and preserves detail inspection budget", "[MoveFrame]")
{
    auto id = identity(); id.move_frame = captured(64);
    const auto native_moves = moves();
    const auto geometry = layer_geometry_page(native_moves, gcode_layers(native_moves),
        parse_layer_geometry_request({{"plate_index", 2}, {"layer", 1}, {"max_points", 2}}), id, 64);
    CHECK(geometry.dump().size() <= 65536);
    REQUIRE(geometry["coordinate_frame"]["processor_offsets"]["extruder_offsets_mm"].size() == 64);
    GCodeProcessorResult result{}; result.filaments_count = 64; result.moves = native_moves;
    const auto detail = plate_analysis_page(result, parse_plate_analysis_request({{"plate_index", 2}, {"section", "detail"}, {"max_moves", 5}}), id, {});
    CHECK(detail["inspected_moves"] == 5);
    CHECK(detail.dump().size() <= 65536);
    CHECK(detail["coordinate_frame"] == geometry["coordinate_frame"]);
}

TEST_CASE("All response sections bind the same frame without extending the analysis cursor", "[MoveFrame]")
{
    auto id = identity(); id.move_frame = captured(4);
    GCodeProcessorResult result{}; result.filaments_count = 4; result.moves = moves();
    for (auto& mode : result.print_statistics.modes) mode.time = 0.;
    const auto geometry = layer_geometry_page(result.moves, gcode_layers(result.moves),
        parse_layer_geometry_request({{"plate_index", 2}, {"layer", 1}}), id, 4);
    const auto summary = plate_analysis_page(result, parse_plate_analysis_request({{"plate_index", 2}}), id, {});
    json params = {{"plate_index", 2}, {"section", "detail"}, {"max_moves", 5}};
    const auto detail = plate_analysis_page(result, parse_plate_analysis_request(params), id, {});
    CHECK(geometry["coordinate_frame"] == summary["coordinate_frame"]);
    CHECK(summary["coordinate_frame"] == detail["coordinate_frame"]);
    REQUIRE(detail["next_cursor"].size() == 5);
    params["cursor"] = detail["next_cursor"];
    id.move_frame.xy_mm[0] += 1.;
    const auto changed = plate_analysis_page(result, parse_plate_analysis_request(params), id, {});
    CHECK(changed["result_id"] == detail["result_id"]);
    CHECK(changed["coordinate_frame"]["frame_digest"] != detail["coordinate_frame"]["frame_digest"]);
    // Same-ID arbitrary metadata mutation is outside publication; clients must reject this join.
}
