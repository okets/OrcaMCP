#include "OrcaMCPLayerGeometry.hpp"
#include "OrcaMCPPhysicalSegments.hpp"
#include "OrcaMCPResultIdentity.hpp"
#include "OrcaMCPDigest.hpp"
#include "OrcaMCPMoveFrame.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace Slic3r { namespace GUI { namespace OrcaMCP {
namespace {
bool is_feature_extrusion(const GcodeMove& m)
{
    return m.type == EMoveType::Extrude && extrusion_feature_of(m.extrusion_role) != ExtrusionFeature::other;
}
// The height layer [begin, end) prints at: where its last extrusion is, else its last move, else the
// height of the layer before it (a layer change with nothing in it).
double layer_height_of(const std::vector<GcodeMove>& moves, size_t begin, size_t end, double previous_z)
{
    for (size_t i = end; i > begin; --i)
        if (is_feature_extrusion(moves[i - 1]))
            return moves[i - 1].position.z();
    return end > begin ? double(moves[end - 1].position.z()) : previous_z;
}


using nlohmann::json;
[[noreturn]] void invalid(const std::string& message) { throw GeometryError("invalid_arguments", message); }
void keys(const json& value, const std::vector<std::string>& allowed)
{
    if (!value.is_object()) invalid("Expected an object");
    for (const auto& item : value.items())
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) invalid("Unknown key: " + item.key());
}
uint64_t integer(const json& value, uint64_t minimum, uint64_t maximum)
{
    if (!value.is_number_integer() || (value.is_number_unsigned() ? value.get<uint64_t>() > maximum :
            value.get<int64_t>() < 0 || uint64_t(value.get<int64_t>()) > maximum)) invalid("Expected an integer in range");
    const uint64_t n = value.get<uint64_t>();
    if (n < minimum) invalid("Expected an integer in range");
    return n;
}
json feature_names(const std::array<bool, k_plan_feature_count>& enabled)
{
    json names = json::array();
    for (size_t i = 0; i < enabled.size(); ++i)
        if (enabled[i]) names.push_back(extrusion_feature_key(ExtrusionFeature(i)));
    return names;
}
json filters_json(const LayerGeometryRequest& request)
{
    return {{"features", feature_names(request.features)},
            {"filaments", request.filaments.empty() ? json("all") : json(request.filaments)}};
}
template<class Segment>
bool matches(const LayerGeometryRequest& request, const Segment& s)
{
    return s.extrusion && request.features[size_t(s.feature)] && (request.filaments.empty() ||
           std::binary_search(request.filaments.begin(), request.filaments.end(), s.filament));
}
// Explicit little-endian integer/IEEE floating encodings, never padding or raw native structs.
std::string layer_digest(const std::vector<GcodeMove>& moves, const GcodeLayer& layer, size_t index, size_t count, const std::string& frame_digest)
{
    Digest hash;
    hash.text("orcamcp-layer-geometry-v3");
    hash.text(frame_digest);
    hash.number(layer.begin); hash.number(layer.end); hash.scalar(layer.z); hash.number(index); hash.number(count);
    const size_t predecessor = physical_predecessor(moves, std::max<size_t>(1, layer.begin));
    hash.number(predecessor);
    for (size_t i = predecessor; i < layer.end; ++i) {
        const auto& m = moves[i];
        hash.xyz(m.position); hash.number(uint64_t(m.type)); hash.number(uint64_t(m.extrusion_role));
        hash.number(m.internal_only ? 1 : 0);
        hash.number(m.extruder_id); hash.scalar(m.width); hash.scalar(m.height);
        hash.number(uint64_t(int64_t(m.object_label_id))); hash.number(m.layer_id);
    }
    return hash.finish();
}
json point(const GcodeMove& m) { return json::array({double(m.position.x()), double(m.position.y()), double(m.position.z())}); }
bool same_run(const GcodeMove& a, const GcodeMove& b)
{
    return is_feature_extrusion(a) && extrusion_feature_of(a.extrusion_role) == extrusion_feature_of(b.extrusion_role) &&
           a.extruder_id == b.extruder_id && a.width == b.width && a.height == b.height && a.object_label_id == b.object_label_id;
}
json cursor_json(const std::string& result_id, const std::string& digest, size_t index, const json& filters, size_t next)
{
    return {{"result_id", result_id}, {"layer_digest", digest}, {"layer", index + 1},
            {"features", filters["features"]}, {"filaments", filters["filaments"]}, {"next_move", next}};
}
}

std::vector<GcodeLayer> gcode_layers(const std::vector<GcodeMove>& moves)
{
    std::vector<GcodeLayer> layers;
    if (moves.empty())
        return layers;
    const unsigned count = moves.back().layer_id + 1;
    layers.reserve(count);
    size_t begin = 0;
    double z     = 0.;
    for (unsigned id = 0; id < count; ++id) {
        const auto end = std::partition_point(moves.begin() + begin, moves.end(), [id](const GcodeMove& m) { return m.layer_id <= id; });
        GcodeLayer layer;
        layer.begin = begin;
        layer.end   = size_t(end - moves.begin());
        layer.z = z = layer_height_of(moves, layer.begin, layer.end, z);
        layers.push_back(layer);
        begin = layer.end;
    }
    return layers;
}

size_t layer_by_number(const std::vector<GcodeLayer>& layers, int number)
{
    if (layers.empty())
        throw std::runtime_error("the plate's G-code has no layers");
    if (number < 1 || size_t(number) > layers.size())
        throw std::runtime_error("layer " + std::to_string(number) + " does not exist: this plate's G-code has layers 1.." +
                                 std::to_string(layers.size()) + ", numbered as the Preview's layer slider numbers them");
    return size_t(number - 1);
}

LayerAtHeight layer_nearest_z(const std::vector<GcodeLayer>& layers, double z)
{
    if (layers.empty())
        throw std::runtime_error("the plate's G-code has no layers");
    // A layer at exactly the height asked for wins. Otherwise the nearest does, and distances within
    // half the thousandth the G-code writes heights to are the same distance, so a height between two
    // layers goes to the lower one; real layers can be a thousandth apart, so that is as wide as it gets.
    const double exact = 1e-6, same = 0.0005;
    double       best  = std::numeric_limits<double>::max();
    for (const GcodeLayer& layer : layers)
        best = std::min(best, std::abs(layer.z - z));
    const double  reach = best < exact ? exact : best + same;
    LayerAtHeight hit;
    bool          found = false;
    for (size_t i = 0; i < layers.size(); ++i)
        if (std::abs(layers[i].z - z) <= reach && (!found || layers[i].z < layers[hit.index].z - exact)) {
            hit.index = i;
            found     = true;
        }
    for (size_t i = 0; i < layers.size(); ++i)
        if (i != hit.index && std::abs(layers[i].z - layers[hit.index].z) < exact)
            hit.also_at.push_back(int(i) + 1);
    return hit;
}


void visit_layer_segments(const std::vector<GcodeMove>& moves, const GcodeLayer& layer,
                          const std::function<bool(const NativeLayerSegment&)>& visitor)
{
    if (layer.begin > layer.end || layer.end > moves.size()) invalid("Invalid native layer bounds");
    for (size_t i = std::max<size_t>(1, layer.begin); i < layer.end; ++i) {
        const auto& m = moves[i];
        if (!visitor({i, moves[i - 1], m, extrusion_feature_of(m.extrusion_role), int(m.extruder_id) + 1, is_feature_extrusion(m)})) break;
    }
}

LayerGeometryRequest parse_layer_geometry_request(const json& params)
{
    keys(params, {"plate_index", "layer", "z", "features", "filaments", "max_points", "cursor"});
    if (!params.contains("plate_index")) invalid("plate_index is required");
    LayerGeometryRequest request;
    request.plate_index = int(integer(params["plate_index"], 0, std::numeric_limits<int>::max()));
    if (params.contains("layer") == params.contains("z")) invalid("Exactly one of layer or z is required");
    if (params.contains("layer")) request.layer = int(integer(params["layer"], 1, std::numeric_limits<int>::max()));
    else {
        if (!params["z"].is_number() || !std::isfinite(params["z"].get<double>())) invalid("z must be finite millimetres");
        request.z = params["z"].get<double>();
    }
    if (params.contains("features")) {
        const auto& features = params["features"];
        if (!features.is_array() || features.empty()) invalid("features must be a nonempty array");
        request.features.fill(false);
        for (const auto& name : features) {
            bool found = false;
            for (size_t i = 0; i < request.features.size(); ++i)
                if (name == extrusion_feature_key(ExtrusionFeature(i))) { request.features[i] = true; found = true; }
            if (!found) invalid("Unknown extrusion feature");
        }
    }
    if (params.contains("filaments")) {
        const auto& slots = params["filaments"];
        if (!slots.is_array() || slots.empty()) invalid("filaments must be a nonempty array");
        std::array<bool, 256> selected{};
        for (const auto& slot : slots) selected[size_t(integer(slot, 1, selected.size()) - 1)] = true;
        for (size_t i = 0; i < selected.size(); ++i)
            if (selected[i]) request.filaments.push_back(int(i + 1));
    }
    if (params.contains("max_points")) request.max_points = size_t(integer(params["max_points"], 2, 8192));
    if (params.contains("cursor")) {
        const auto& cursor = params["cursor"];
        keys(cursor, {"result_id", "layer_digest", "layer", "features", "filaments", "next_move"});
        if (cursor.size() != 6) invalid("cursor requires every output cursor field");
        for (const char* key : {"result_id", "layer_digest"})
            if (!cursor[key].is_string() || cursor[key].get_ref<const std::string&>().size() != 40) invalid("Malformed cursor identity");
        integer(cursor["layer"], 1, std::numeric_limits<int>::max());
        integer(cursor["next_move"], 1, std::numeric_limits<size_t>::max());
        // Compare bounded canonical arrays directly; these must be copied unchanged from the output.
        const auto filters = filters_json(request);
        if (cursor["features"] != filters["features"] || cursor["filaments"] != filters["filaments"])
            invalid("cursor filters must match the canonical query");
        request.cursor = cursor;
    }
    return request;
}

json layer_geometry_page(const std::vector<GcodeMove>& moves, const std::vector<GcodeLayer>& layers,
                         const LayerGeometryRequest& request, const GeometryIdentity& identity, size_t filament_count, GeometryPageMetrics* metrics)
{
    if (layers.empty()) throw GeometryError("no_valid_slice", "The plate has no valid printed G-code layers");
    for (int slot : request.filaments)
        if (size_t(slot) > filament_count) invalid("Filament slot does not exist on this printer");
    LayerAtHeight chosen;
    try { chosen = request.layer ? LayerAtHeight{layer_by_number(layers, *request.layer), {}} : layer_nearest_z(layers, *request.z); }
    catch (const std::runtime_error& e) { invalid(e.what()); }
    const auto& layer = layers[chosen.index];
    if (layer.begin > layer.end || layer.end > moves.size()) invalid("Invalid native layer bounds");
    const auto digest_started = std::chrono::steady_clock::now();
    const std::string result_id = native_result_identity(identity);
    const json coordinate_frame = coordinate_frame_json(identity.move_frame, result_id);
    const std::string digest = layer_digest(moves, layer, chosen.index, layers.size(), coordinate_frame["frame_digest"].get<std::string>());
    const auto digest_finished = std::chrono::steady_clock::now();
    if (metrics) metrics->digest_ms = std::chrono::duration<double, std::milli>(digest_finished - digest_started).count();
    const json filters = filters_json(request);
    size_t start = std::max<size_t>(1, layer.begin);
    if (!request.cursor.is_null()) {
        const auto& cursor = request.cursor;
        if (cursor["result_id"] != result_id || cursor["layer_digest"] != digest || cursor["layer"] != chosen.index + 1)
            throw GeometryError("stale_slice", "Cursor result or layer identity changed; start a new query");
        start = cursor["next_move"].get<size_t>();
        if (start < std::max<size_t>(1, layer.begin) || start >= layer.end) invalid("Cursor move lies outside the layer");
        const auto& m = moves[start];
        if (!is_physical_record(m)) invalid("Cursor must name a physical extrusion segment");
        const size_t predecessor = physical_predecessor(moves, start);
        const auto s = physical_segment(start, predecessor, moves[predecessor], m);
        if (!matches(request, s)) invalid("Cursor must name an eligible extrusion segment");
    }
    json layer_info = {{"number", chosen.index + 1}, {"of", layers.size()}, {"z", std::round(layer.z * 10000.) / 10000. + 0.0}};
    if (request.z) { layer_info["requested_z"] = *request.z; layer_info["also_at"] = chosen.also_at; }
    json out = {{"status", "success"}, {"schema_version", 1}, {"source", "gcode_processor"},
                {"application_version", identity.application_version}, {"orcamcp_version", identity.orcamcp_version},
                {"build_commit", identity.build_commit}, {"plate_index", identity.plate_index}, {"result_id", result_id},
                {"layer_digest", digest}, {"coordinate_frame", coordinate_frame}, {"slice_valid", true}, {"units", "mm"}, {"frame", "bed"},
                {"axes", {"x", "y", "z"}}, {"representation", "extrusion_centerlines"}, {"layer", layer_info},
                {"filters", filters}, {"paths", json::array()}, {"empty_layer", true}, {"nothing_matched", true},
                {"has_more", false}, {"next_cursor", nullptr}};
    bool any = false, matched = false, open = false, stopped = false;
    size_t points = 0, previous_index = 0;
    // Reserve the largest continuation envelope throughout collection, so finishing cannot overflow.
    out["has_more"] = true;
    out["next_cursor"] = cursor_json(result_id, digest, chosen.index, filters, layer.end);
    size_t bytes = out.dump().size() + 16;
    visit_physical_layer_segments(moves, layer, [&](const PhysicalSegment& s) {
        any = any || s.extrusion;
        if (!matches(request, s)) { open = false; return true; }
        matched = true;
        if (s.index < start) { open = false; return true; }
        const bool append = open && previous_index == s.previous_index && same_run(moves[previous_index], s.move);
        const size_t added_points = append ? 1 : 2;
        json fragment;
        size_t added_bytes;
        if (append) added_bytes = point(s.move).dump().size() + 1;
        else {
            const bool continues = !request.cursor.is_null() && s.index == start && s.previous_index >= std::max<size_t>(1, layer.begin) &&
                                   same_run(s.previous, s.move);
            fragment = {{"start_move", s.index}, {"feature", extrusion_feature_key(s.feature)}, {"filament", s.filament},
                        {"width_mm", s.move.width}, {"height_mm", s.move.height}, {"object_label_id", s.move.object_label_id},
                        {"continues", continues}, {"points", {point(s.previous), point(s.move)}}};
            added_bytes = fragment.dump().size() + (out["paths"].empty() ? 0 : 1);
        }
        if (points + added_points > request.max_points || bytes + added_bytes > k_geometry_byte_limit) {
            if (points == 0) throw GeometryError("output_limit", "One segment and its metadata exceed the response limit");
            stopped = true;
            out["next_cursor"] = cursor_json(result_id, digest, chosen.index, filters, s.index);
            return false;
        }
        if (append) out["paths"].back()["points"].push_back(point(s.move));
        else out["paths"].push_back(std::move(fragment));
        bytes += added_bytes; points += added_points; open = true; previous_index = s.index;
        return true;
    });
    out["empty_layer"] = !any; out["nothing_matched"] = !matched;
    out["has_more"] = stopped;
    if (!stopped) out["next_cursor"] = nullptr;
    if (!matched) out["hint"] = any ? "No extrusion matches the requested features and filaments." : "This layer has no supported feature extrusions.";
    if (out.dump().size() > k_geometry_byte_limit) throw GeometryError("output_limit", "Response metadata exceeds the response limit");
    if (metrics) metrics->serialization_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - digest_finished).count();
    return out;
}

json layer_geometry_schema()
{
    const json features = {{"type", "array"}, {"minItems", 1},
        {"items", {{"type", "string"}, {"enum", {"perimeters", "infill", "support", "support_interface", "brim", "skirt", "prime_tower"}}}}};
    const json filaments = {{"type", "array"}, {"minItems", 1},
                           {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}}};
    const json cursor = {{"type", "object"}, {"additionalProperties", false},
        {"required", {"result_id", "layer_digest", "layer", "features", "filaments", "next_move"}},
        {"properties", {{"result_id", {{"type", "string"}, {"minLength", 40}, {"maxLength", 40}}},
            {"layer_digest", {{"type", "string"}, {"minLength", 40}, {"maxLength", 40}}},
            {"layer", {{"type", "integer"}, {"minimum", 1}}}, {"features", features},
            {"filaments", {{"oneOf", {filaments, {{"type", "string"}, {"enum", {"all"}}}}}}},
            {"next_move", {{"type", "integer"}, {"minimum", 1}}}}}};
    return {{"type", "object"}, {"additionalProperties", false}, {"required", {"plate_index"}},
        {"oneOf", {{{"required", {"layer"}}, {"not", {{"required", {"z"}}}}},
                   {{"required", {"z"}}, {"not", {{"required", {"layer"}}}}}}},
        {"properties", {{"plate_index", {{"type", "integer"}, {"minimum", 0}}},
            {"layer", {{"type", "integer"}, {"minimum", 1}}}, {"z", {{"type", "number"}}},
            {"features", features}, {"filaments", filaments},
            {"max_points", {{"type", "integer"}, {"minimum", 2}, {"maximum", 8192}, {"default", 512}}}, {"cursor", cursor}}}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
