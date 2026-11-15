#pragma once

// Immutable native Preview traversal and bounded centerline pages. No wx or rendering dependency.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "OrcaMCPExtrusionFeatures.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

using GcodeMove = GCodeProcessorResult::MoveVertex;

// --- which layer -----------------------------------------------------------------------------

// One printed layer of the G-code: its moves are [begin, end) of GCodeProcessorResult::moves, and z is
// the height it prints at -- where its last extrusion is, which for a spiral vase is the top of the
// climb. Layer i is the one the Preview's slider numbers i + 1.
struct GcodeLayer
{
    size_t begin = 0, end = 0;
    double z     = 0.;
};

// Every layer of `moves`, found by binary search on MoveVertex::layer_id, which the processor only
// ever counts up (one per layer change), so this costs a few probes per layer, not a pass over moves.
std::vector<GcodeLayer> gcode_layers(const std::vector<GcodeMove>& moves);

// The layer the slider calls `number` (1-based). Throws, naming the valid range, when there is none.
size_t layer_by_number(const std::vector<GcodeLayer>& layers, int number);

struct LayerAtHeight
{
    size_t           index = 0;  // into the layers
    std::vector<int> also_at;    // other layer numbers printed at the same height (a by-object print)
};

// The layer printed nearest to `z` mm. A height exactly between two goes to the lower; one outside
// the print goes to its first or last layer. Throws when there are no layers.
LayerAtHeight layer_nearest_z(const std::vector<GcodeLayer>& layers, double z);


constexpr size_t k_plan_feature_count = size_t(ExtrusionFeature::other);
constexpr size_t k_geometry_byte_limit = 64 * 1024;

struct NativeLayerSegment {
    size_t index;
    const GcodeMove& previous;
    const GcodeMove& move;
    ExtrusionFeature feature;
    int filament; // Native extruder_id stores the processor filament id; 1-based here, never remapped.
    bool extrusion; // false is an explicit run break, including unsupported extrusion roles.
};
// Visits [max(begin,1),end), stopping when visitor returns false. No whole-layer allocation.
void visit_layer_segments(const std::vector<GcodeMove>& moves, const GcodeLayer& layer,
                          const std::function<bool(const NativeLayerSegment&)>& visitor);

struct GeometryError : std::runtime_error {
    std::string code;
    GeometryError(std::string code, const std::string& message) : std::runtime_error(message), code(std::move(code)) {}
};
struct LayerGeometryRequest {
    int plate_index = 0;
    std::optional<int> layer;
    std::optional<double> z;
    std::array<bool, k_plan_feature_count> features;
    std::vector<int> filaments;
    size_t max_points = 512;
    nlohmann::json cursor = nullptr;
    LayerGeometryRequest() { features.fill(true); }
};
struct GeometryIdentity {
    std::string session;
    std::string plate;
    int plate_index;
    uint64_t native_result_id;
    std::string application_version;
    std::string orcamcp_version;
    nlohmann::json build_commit;
    MoveFrameProvenance move_frame;
};
struct GeometryPageMetrics {
    double digest_ms = 0.;
    double serialization_ms = 0.;
};
LayerGeometryRequest parse_layer_geometry_request(const nlohmann::json& params);
nlohmann::json layer_geometry_schema();
// Digest scans the complete selected layer, including its predecessor endpoint. A page only retains
// bounded response geometry; the digest intentionally costs O(selected-layer moves) on every call.
nlohmann::json layer_geometry_page(const std::vector<GcodeMove>& moves, const std::vector<GcodeLayer>& layers,
                                   const LayerGeometryRequest& request, const GeometryIdentity& identity,
                                   size_t filament_count, GeometryPageMetrics* metrics = nullptr);

}}} // namespace Slic3r::GUI::OrcaMCP
