#include "OrcaMCPPlateAnalysis.hpp"
#include "OrcaMCPSliceEstimate.hpp"
#include "OrcaMCPPhysicalSegments.hpp"
#include "OrcaMCPMoveFrame.hpp"
#include "libslic3r/Config.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {
namespace {
using nlohmann::json;
using Clock = std::chrono::steady_clock;
[[noreturn]] void refuse(const char* code, const std::string& message) { throw GeometryError(code, message); }
void exact_keys(const json& object, const std::vector<std::string>& names)
{
    if (!object.is_object()) refuse("invalid_arguments", "Expected an object");
    for (const auto& item : object.items())
        if (std::find(names.begin(), names.end(), item.key()) == names.end())
            refuse("invalid_arguments", "Unknown key: " + item.key());
}
uint64_t integral(const json& value, uint64_t low, uint64_t high)
{
    uint64_t number = 0;
    if (value.is_number_unsigned()) number = value.get<uint64_t>();
    else if (value.is_number_integer()) {
        const int64_t signed_number = value.get<int64_t>();
        if (signed_number < 0) refuse("invalid_arguments", "Integral number is outside the supported range");
        number = static_cast<uint64_t>(signed_number);
    } else if (value.is_number_float()) {
        const double floating = value.get<double>();
        // On arm64 long double has double precision too. Reject 2^64 before conversion rather than
        // comparing against a rounded UINT64_MAX and invoking an out-of-range floating cast.
        if (!std::isfinite(floating) || std::floor(floating) != floating || floating < 0. || floating >= std::ldexp(1., 64))
            refuse("invalid_arguments", "Expected an integral number in range");
        number = static_cast<uint64_t>(floating);
    } else refuse("invalid_arguments", "Expected an integral number");
    if (number < low || number > high) refuse("invalid_arguments", "Integral number is outside the supported range");
    return number;
}
[[noreturn]] void output_limit(size_t budget)
{
    refuse("output_limit", budget < 10000 ? "No physical record fits this page; increase max_moves" :
        "Source cannot be paged under this contract's move/byte limits");
}
json accounting() { return {{"supported", true}, {"basis", "native_pipeline_steps_complete"}}; }
json common(const GeometryIdentity& identity, bool detail)
{
    const std::string result_id = native_result_identity(identity);
    return {{"status", "success"}, {"schema_version", 1}, {"source", "gcode_processor"},
        {"application_version", identity.application_version}, {"orcamcp_version", identity.orcamcp_version},
        {"build_commit", identity.build_commit}, {"plate_index", identity.plate_index},
        {"result_id", result_id}, {"coordinate_frame", coordinate_frame_json(identity.move_frame, result_id)}, {"slice_valid", true},
        {"section", detail ? "detail" : "summary"}, {"has_more", false}, {"next_cursor", nullptr}};
}
json cursor(const json& out, size_t count, unsigned tail_id, size_t next)
{
    return {{"result_id", out["result_id"]}, {"section", "detail"}, {"total_moves", count},
            {"tail_gcode_id", tail_id}, {"next_move", next}};
}
void validate_physical(const GcodeMove& move)
{
    if (!move.position.allFinite() || !std::isfinite(move.width) || !std::isfinite(move.height) ||
        !std::isfinite(move.delta_extruder)) refuse("unsupported_source", "Physical move has nonfinite geometry or extrusion fields");
}
const char* role_name(ExtrusionRole role)
{
#define ORCA_ROLE(name) case name: return #name
    switch (role) {
    ORCA_ROLE(erNone); ORCA_ROLE(erPerimeter); ORCA_ROLE(erExternalPerimeter); ORCA_ROLE(erOverhangPerimeter);
    ORCA_ROLE(erInternalInfill); ORCA_ROLE(erSolidInfill); ORCA_ROLE(erTopSolidInfill); ORCA_ROLE(erBottomSurface);
    ORCA_ROLE(erIroning); ORCA_ROLE(erBridgeInfill); ORCA_ROLE(erInternalBridgeInfill); ORCA_ROLE(erGapFill);
    ORCA_ROLE(erSkirt); ORCA_ROLE(erBrim); ORCA_ROLE(erSupportMaterial); ORCA_ROLE(erSupportMaterialInterface);
    ORCA_ROLE(erSupportTransition); ORCA_ROLE(erWipeTower); ORCA_ROLE(erCustom); ORCA_ROLE(erMixed);
    default: refuse("unsupported_source", "Physical extrusion has an unrepresentable native role");
    }
#undef ORCA_ROLE
}
void add_metric(json& row, const char* key, double value)
{
    if (row.contains(key) && row[key].is_null()) return;
    const double sum = value + (row.contains(key) ? row[key].get<double>() : 0.);
    if (std::isfinite(sum)) row[key] = sum;
    else { row[key] = nullptr; row["unknown"][key] = "nonfinite_result"; }
}
// Changes are staged as at most one row per reducer. No whole-page copy or full-result allocation.
size_t replacement_bytes(const json& rows, const json& row, bool replace)
{
    return replace ? row.dump().size() : row.dump().size() + (rows.empty() ? 0 : 1);
}
size_t previous_bytes(const json& rows, bool replace) { return replace ? rows.back().dump().size() : 0; }
void commit_row(json& rows, json row, bool replace)
{
    if (replace) rows.back() = std::move(row);
    else rows.push_back(std::move(row));
}
json detail_page(const GCodeProcessorResult& result, const PlateAnalysisRequest& request, json out)
{
    const size_t total = result.moves.size();
    // Fixed inspection 1: cache tail, including metadata/shape fence, and never reread it.
    const GcodeMove tail = result.moves.back();
    size_t inspected = 1, begin = 1;
    if (!request.cursor.is_null()) {
        const auto& c = request.cursor;
        if (c["result_id"] != out["result_id"] || c["total_moves"] != total || c["tail_gcode_id"] != tail.gcode_id)
            refuse("stale_slice", "Result identity or native move shape changed; start a new query");
        begin = c["next_move"].get<size_t>();
        if (begin < 1 || begin >= total) refuse("invalid_arguments", "next_move must be smaller than total_moves; terminal pages have no cursor");
    }
    if (!is_physical_record(tail)) refuse("unsupported_source", "Native move tail is not a physical record");
    // Fixed inspection 2, even when tail and predecessor happen to have the same index.
    GcodeMove previous = result.moves[begin - 1];
    ++inspected;
    if (!is_physical_record(previous)) refuse("invalid_arguments", "Cursor must immediately follow a committed physical record");
    validate_physical(previous);
    out.update({{"partial", true}, {"range", {{"begin_move", begin}, {"end_move_exclusive", total}}},
        {"inspected_moves", request.max_moves}, {"total_moves", total}, {"final_layer", uint64_t(tail.layer_id) + 1},
        {"accounting", accounting()}, {"layer_parts", json::array()}, {"occupancy_parts", json::array()},
        {"events", json::array()}, {"event_mapping", {{"physical_extruder", "unavailable"},
        {"previous_tool_filament", "derive_from_ordered_tool_change_events"}}}});
    out["has_more"] = true;
    out["next_cursor"] = cursor(out, total, tail.gcode_id, total);
    // Reserve the timing fields and largest range/cursor values, including final false spelling.
    out["timing_ms"] = {{"gui_total", std::numeric_limits<double>::max()},
        {"scan", std::numeric_limits<double>::max()}, {"serialization", std::numeric_limits<double>::max()}};
    size_t bytes = out.dump().size() + 32, committed_end = begin;
    out.erase("timing_ms");
    for (size_t index = begin; index < total && inspected < request.max_moves; ++index) {
        const GcodeMove move = result.moves[index];
        ++inspected;
        if (!is_physical_record(move)) continue;
        validate_physical(move);
        if (move.layer_id < previous.layer_id) refuse("unsupported_source", "Physical native layer IDs are not monotonic");
        const uint64_t layer = uint64_t(move.layer_id) + 1;
        auto& layers = out["layer_parts"];
        const bool replace_layer = !layers.empty() && layers.back()["layer"] == layer;
        json layer_row = replace_layer ? layers.back() : json{{"layer", layer}, {"first_move", index},
            {"last_supported_endpoint_z_mm", nullptr}, {"last_supported_move", nullptr}};
        layer_row["last_move"] = index;
        layer_row["last_endpoint_z_mm"] = move.position.z();
        const auto segment = physical_segment(index, committed_end - 1, previous, move);
        const auto feature = segment.feature;
        if (move.type == EMoveType::Extrude && feature != ExtrusionFeature::other) {
            layer_row["last_supported_move"] = index;
            layer_row["last_supported_endpoint_z_mm"] = move.position.z();
        }
        size_t before = previous_bytes(layers, replace_layer);
        size_t after = replacement_bytes(layers, layer_row, replace_layer);
        json occupancy, event;
        bool replace_occupancy = false;
        if (move.type == EMoveType::Extrude) {
            const char* raw_role = role_name(move.extrusion_role);
            auto& rows = out["occupancy_parts"];
            replace_occupancy = !rows.empty() && rows.back()["last_move"] == committed_end - 1 &&
                rows.back()["layer"] == layer && rows.back()["feature"] == extrusion_feature_key(feature) &&
                rows.back()["raw_role_name"] == raw_role && rows.back()["filament"] == int(move.extruder_id) + 1;
            // Internal records do not split consecutive physical-key fragments.
            occupancy = replace_occupancy ? rows.back() : json{{"layer", layer}, {"feature", extrusion_feature_key(feature)},
                {"raw_role_name", raw_role}, {"filament", int(move.extruder_id) + 1}, {"segment_count", 0},
                {"first_move", index}, {"unknown", json::object()}, {"z_min_mm", std::min(double(previous.position.z()), double(move.position.z()))},
                {"z_max_mm", std::max(double(previous.position.z()), double(move.position.z()))}};
            const Vec3d delta = segment.move.position.cast<double>() - segment.previous.position.cast<double>();
            const double xy = delta.head<2>().norm();
            occupancy["segment_count"] = occupancy["segment_count"].get<size_t>() + 1;
            add_metric(occupancy, "path_length_xyz_mm", delta.norm());
            add_metric(occupancy, "projected_length_xy_mm", xy);
            add_metric(occupancy, "swept_area_xy_mm2", xy * double(move.width));
            add_metric(occupancy, "extruded_filament_mm", move.delta_extruder);
            occupancy["z_min_mm"] = std::min(occupancy["z_min_mm"].get<double>(), std::min(double(previous.position.z()), double(move.position.z())));
            occupancy["z_max_mm"] = std::max(occupancy["z_max_mm"].get<double>(), std::max(double(previous.position.z()), double(move.position.z())));
            occupancy["last_move"] = index;
            before += previous_bytes(rows, replace_occupancy);
            after += replacement_bytes(rows, occupancy, replace_occupancy);
        }
        if (move.type == EMoveType::Tool_change || move.type == EMoveType::Color_change) {
            const bool tool = move.type == EMoveType::Tool_change;
            event = {{"move_index", index}, {"layer", layer}, {"type", tool ? "tool_change" : "color_change"},
                {"native_type", tool ? "Tool_change" : "Color_change"}, {"native_gcode_id", move.gcode_id},
                {"filament", int(move.extruder_id) + 1}, {"cp_color_id", move.cp_color_id}, {"native_z_mm", move.position.z()}};
            after += replacement_bytes(out["events"], event, false);
        }
        if (bytes - before + after > k_geometry_byte_limit) break;
        commit_row(layers, std::move(layer_row), replace_layer);
        if (!occupancy.is_null()) commit_row(out["occupancy_parts"], std::move(occupancy), replace_occupancy);
        if (!event.is_null()) out["events"].push_back(std::move(event));
        bytes = bytes - before + after;
        committed_end = index + 1;
        previous = move;
    }
    if (committed_end == begin) output_limit(request.max_moves);
    out["range"]["end_move_exclusive"] = committed_end;
    out["inspected_moves"] = inspected;
    out["has_more"] = committed_end < total;
    out["next_cursor"] = committed_end < total ? cursor(out, total, tail.gcode_id, committed_end) : json(nullptr);
    return out;
}

struct Property { std::optional<double> value; std::string reason; };
Property property(const std::vector<float>& values, size_t slot, bool price = false)
{
    if (slot >= values.size()) return {std::nullopt, "missing"};
    const double value = values[slot];
    if (!std::isfinite(value)) return {std::nullopt, "nonfinite"};
    if (price ? value < 0. : value <= 0.) return {std::nullopt, price ? "negative" : "nonpositive"};
    return {value, ""};
}
json optional_number(const std::optional<double>& value)
{
    return value && std::isfinite(*value) ? json(*value) : json(nullptr);
}
json measurement(const json& value, const char* unit, const char* availability, const json& source, const json& reason = nullptr)
{
    return {{"value", value}, {"unit", unit}, {"availability", availability}, {"source", source}, {"reason", reason}};
}
void validate_volumes(const std::map<size_t, double>& volumes, size_t slots)
{
    for (const auto& item : volumes)
        if (item.first >= slots || !std::isfinite(item.second) || item.second < 0.)
            refuse("unsupported_source", "Native material volume or filament slot is outside the accounting domain");
}
json conversion(double volume, const std::optional<double>& value, const Property& input, const char* unit, const char* name)
{
    if (volume == 0.) return measurement(0., unit, "derived", "filament_conversion");
    if (!input.value) return measurement(nullptr, unit, "unknown", "filament_conversion", std::string(name) + "_" + input.reason);
    if (!value || !std::isfinite(*value)) return measurement(nullptr, unit, "unknown", "filament_conversion", "nonfinite_result");
    return measurement(*value, unit, "derived", "filament_conversion");
}
json summary_page(const GCodeProcessorResult& result, const json& controls, json out)
{
    const auto& ids = result.settings_ids;
    size_t metadata_bytes = ids.print.size() + ids.printer.size();
    if (metadata_bytes > k_geometry_byte_limit || ids.filament.size() > k_geometry_byte_limit / 3)
        refuse("output_limit", "Result profile metadata exceeds the success-object byte limit");
    for (const auto& name : ids.filament) {
        if (name.size() > k_geometry_byte_limit - metadata_bytes)
            refuse("output_limit", "Result profile metadata exceeds the success-object byte limit");
        metadata_bytes += name.size();
    }
    const auto& statistics = result.print_statistics;
    const std::array<const std::map<size_t, double>*, 5> maps{{&statistics.model_volumes_per_extruder,
        &statistics.support_volumes_per_extruder, &statistics.wipe_tower_volumes_per_extruder,
        &statistics.flush_per_filament, &statistics.total_volumes_per_extruder}};
    constexpr std::array<const char*, 5> categories{{"model", "support", "prime_tower", "flush", "total"}};
    constexpr std::array<const char*, 5> sources{{"model_volume", "support_volume", "prime_tower_volume", "flush_volume", "total_volume"}};
    std::array<SliceEstimate, 5> estimates;
    for (size_t i = 0; i < maps.size(); ++i) {
        validate_volumes(*maps[i], result.filaments_count);
        estimates[i] = compute_slice_estimate(*maps[i], result.filament_diameters, result.filament_densities, result.filament_costs);
    }
    out["profile_inputs"] = {{"provenance", "result_profile_values"}, {"slots", json::array()}};
    out["material_categories"] = json::array();
    out["material_category_totals"] = json::object();
    json used = json::array();
    for (size_t slot = 0; slot < result.filaments_count; ++slot) {
        const auto diameter = property(result.filament_diameters, slot), density = property(result.filament_densities, slot),
                   price = property(result.filament_costs, slot, true);
        json profile = {{"filament", slot + 1}, {"preset", slot < result.settings_ids.filament.size() ? json(result.settings_ids.filament[slot]) : json(nullptr)},
            {"diameter_mm", optional_number(diameter.value)}, {"density_g_cm3", optional_number(density.value)},
            {"price_per_kg", optional_number(price.value)}, {"price_status", !price.value ? "invalid" : *price.value == 0. ? "zero" : "ok"},
            {"unavailable", json::object()}};
        for (const auto& item : std::array<std::pair<const char*, Property>, 3>{{{"diameter_mm", diameter}, {"density_g_cm3", density}, {"price_per_kg", price}}})
            if (!item.second.value) profile["unavailable"][item.first] = item.second.reason;
        if (slot >= result.settings_ids.filament.size()) profile["unavailable"]["preset"] = "missing";
        out["profile_inputs"]["slots"].push_back(std::move(profile));
        json row = {{"filament", slot + 1}};
        for (size_t category = 0; category < maps.size(); ++category) {
            const auto it = maps[category]->find(slot);
            const double volume = it == maps[category]->end() ? 0. : it->second;
            const FilamentUsage* usage = nullptr;
            for (const auto& candidate : estimates[category].per_filament)
                if (candidate.filament_id == slot) { usage = &candidate; break; }
            const auto length = usage ? usage->length_mm : std::optional<double>{};
            const auto weight = usage ? usage->weight_g : std::optional<double>{};
            const auto cost = usage ? usage->cost : std::optional<double>{};
            Property cost_input = !density.value ? Property{std::nullopt, "density_" + density.reason} :
                !price.value ? Property{std::nullopt, "price_" + price.reason} : price;
            json values = {{"volume_mm3", measurement(volume, "mm3", volume == 0. ? "recorded_zero" : "recorded", sources[category])},
                {"length_mm", conversion(volume, length, diameter, "mm", "diameter")},
                {"weight_g", conversion(volume, weight, density, "g", "density")},
                {"profile_material_cost", conversion(volume, cost, cost_input, "profile_money", "cost")}};
            // Cost depends on both density and price; retain the actual unavailable input name.
            if (volume > 0. && !cost_input.value) values["profile_material_cost"]["reason"] = cost_input.reason;
            row[categories[category]] = values;
            auto& totals = out["material_category_totals"][categories[category]];
            if (totals.is_null()) {
                totals = values;
            } else for (const char* metric : {"volume_mm3", "length_mm", "weight_g", "profile_material_cost"}) {
                auto& total_measure = totals[metric];
                const auto& next_measure = values[metric];
                if (total_measure["value"].is_null() || next_measure["value"].is_null()) {
                    total_measure["value"] = nullptr; total_measure["availability"] = "unknown";
                    total_measure["reason"] = "one_or_more_slots_unknown";
                } else {
                    const double sum = total_measure["value"].get<double>() + next_measure["value"].get<double>();
                    if (!std::isfinite(sum)) {
                        total_measure["value"] = nullptr; total_measure["availability"] = "unknown"; total_measure["reason"] = "nonfinite_result";
                    } else {
                        total_measure["value"] = sum;
                        if (std::string(metric) == "volume_mm3") total_measure["availability"] = sum == 0. ? "recorded_zero" : "recorded";
                    }
                }
            }
            if (category == 4 && volume > 0.) used.push_back({{"filament", slot + 1}, {"volume_mm3", volume},
                {"length_mm", values["length_mm"]["value"]}, {"weight_grams", values["weight_g"]["value"]},
                {"cost", values["profile_material_cost"]["value"]}});
        }
        out["material_categories"].push_back(std::move(row));
    }
    const auto& totals = out["material_category_totals"]["total"];
    const double normal = normal_mode_print_time_s(statistics);
    const double silent = statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Stealth)].time;
    out["native_estimate"] = {{"estimated_time_seconds", std::isfinite(normal) && normal >= 0. ? json(normal) : json(nullptr)},
        {"estimated_time_silent_seconds", std::isfinite(silent) && silent > 0. ? json(silent) : json(nullptr)},
        {"filament_changes", statistics.total_filament_changes}, {"extruder_changes", statistics.total_extruder_changes},
        {"filament", {{"total_length_mm", totals["length_mm"]["value"]}, {"total_volume_mm3", totals["volume_mm3"]["value"]},
        {"total_weight_grams", totals["weight_g"]["value"]}, {"total_cost", totals["profile_material_cost"]["value"]}, {"per_filament", used}}}};
    out["pricing"] = {{"currency", nullptr}, {"currency_status", "not_recorded"}, {"price_unit", "profile_money/kg"},
        {"cost_unit", "profile_money"}, {"cost_includes_time_cost", false}};
    out["accounting"] = accounting();
    out["accounting"]["flush"] = {{"availability", "native_tag_accounting"}, {"coverage", "not_proven_complete"},
        {"basis", "FLUSH_regions_and_native_purge_commands"}};
    out["accounting"]["move_statistics_consistency"] = "not_verified";
    out["slice_inputs"] = {{"source", "live_print_config_after_slice"}, {"binding", "not_proven_slice_time"},
        {"settings_ids", {{"print", result.settings_ids.print}, {"printer", result.settings_ids.printer}, {"filament", result.settings_ids.filament}}},
        {"controls", controls}};
    out["sources"] = {{"model_volume", "PrintEstimatedStatistics.model_volumes_per_extruder"},
        {"support_volume", "PrintEstimatedStatistics.support_volumes_per_extruder"}, {"prime_tower_volume", "PrintEstimatedStatistics.wipe_tower_volumes_per_extruder"},
        {"flush_volume", "PrintEstimatedStatistics.flush_per_filament"}, {"total_volume", "PrintEstimatedStatistics.total_volumes_per_extruder"},
        {"filament_conversion", "compute_slice_estimate(volume,filament_diameters,filament_densities,filament_costs)"},
        {"print_config", "plate.fff_print()->full_print_config()"}, {"result_settings_ids", "GCodeProcessorResult.settings_ids"}};
    out["unattributed"] = json::array();
    for (const char* key : {"start_code_purge", "flush_into_infill"})
        out["unattributed"].push_back({{"key", key}, {"volume", measurement(nullptr, "mm3", "unavailable", nullptr, "not_separately_attributed")}});
    return out;
}
} // namespace

void require_analysis_source(const AnalysisSourceState& state)
{
    if (state.only_gcode || state.gcode_3mf || !state.has_print || !state.has_objects || !state.slicing_finished || !state.export_finished)
        refuse("unsupported_source", "Native pipeline steps are incomplete; loaded accounting is unsupported");
}
PlateAnalysisRequest parse_plate_analysis_request(const json& params)
{
    exact_keys(params, {"plate_index", "section", "max_moves", "cursor"});
    if (!params.contains("plate_index")) refuse("invalid_arguments", "plate_index is required");
    PlateAnalysisRequest request;
    request.plate_index = int(integral(params["plate_index"], 0, std::numeric_limits<int>::max()));
    if (params.contains("section") && params["section"] != "summary" && params["section"] != "detail")
        refuse("invalid_arguments", "section must be summary or detail");
    request.detail = params.value("section", json("summary")) == "detail";
    if (!request.detail && (params.contains("max_moves") || params.contains("cursor")))
        refuse("invalid_arguments", "max_moves and cursor are only permitted for detail");
    if (params.contains("max_moves")) request.max_moves = size_t(integral(params["max_moves"], 5, 10000));
    if (params.contains("cursor")) {
        const auto& c = params["cursor"];
        exact_keys(c, {"result_id", "section", "total_moves", "tail_gcode_id", "next_move"});
        if (c.size() != 5 || c["section"] != "detail" || !c["result_id"].is_string() ||
            c["result_id"].get_ref<const std::string&>().empty() || c["result_id"].get_ref<const std::string&>().size() > 256)
            refuse("invalid_arguments", "Malformed detail cursor");
        integral(c["total_moves"], 2, std::numeric_limits<size_t>::max());
        integral(c["tail_gcode_id"], 0, std::numeric_limits<unsigned>::max());
        integral(c["next_move"], 1, std::numeric_limits<size_t>::max());
        request.cursor = c;
    }
    return request;
}
json plate_analysis_schema()
{
    const json cursor_schema = {{"type", "object"}, {"additionalProperties", false},
        {"required", {"result_id", "section", "total_moves", "tail_gcode_id", "next_move"}},
        {"properties", {{"result_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
            {"section", {{"type", "string"}, {"enum", {"detail"}}}}, {"total_moves", {{"type", "integer"}, {"minimum", 2}}},
            {"tail_gcode_id", {{"type", "integer"}, {"minimum", 0}}}, {"next_move", {{"type", "integer"}, {"minimum", 1}}}}}};
    return {{"type", "object"}, {"additionalProperties", false}, {"required", {"plate_index"}},
        {"properties", {{"plate_index", {{"type", "integer"}, {"minimum", 0}}},
        {"section", {{"type", "string"}, {"enum", {"summary", "detail"}}, {"default", "summary"}}},
        {"max_moves", {{"type", "integer"}, {"minimum", 5}, {"maximum", 10000}, {"default", 10000}}}, {"cursor", cursor_schema}}},
        {"allOf", {{{"if", {{"anyOf", {{{"required", {"max_moves"}}}, {{"required", {"cursor"}}}}}}},
            {"then", {{"required", {"section"}}, {"properties", {{"section", {{"enum", {"detail"}}}}}}}}}}}};
}
json analysis_controls(const ConfigOptionResolver& config)
{
    json controls = json::object();
    for (const char* key : {"flush_multiplier", "flush_multiplier_fast", "prime_volume_mode", "flush_volumes_matrix",
                           "nozzle_volume", "enable_prime_tower", "time_cost"}) {
        const char* unit = std::string(key) == "time_cost" ? "profile_money/hour" :
            std::string(key).find("multiplier") != std::string::npos ? "ratio" :
            std::string(key) == "flush_volumes_matrix" || std::string(key) == "nozzle_volume" ? "mm3" : nullptr;
        const ConfigOption* option = config.option(key);
        json value = nullptr, reason = nullptr;
        if (!option) reason = "not_in_print_config";
        else if (std::string(key) == "nozzle_volume") {
            const auto* values = dynamic_cast<const ConfigOptionFloatsNullable*>(option);
            if (!values) refuse("unsupported_source", "nozzle_volume has an unexpected native type");
            if (values->values.size() > k_geometry_byte_limit / 2)
                refuse("output_limit", "Nullable control vector exceeds the success-object byte limit");
            value = json::array();
            for (size_t i = 0; i < values->values.size(); ++i) {
                if (values->is_nil(i)) { value.push_back(nullptr); reason = "contains_nil"; }
                else if (std::isfinite(values->values[i])) value.push_back(values->values[i]);
                else refuse("unsupported_source", "nozzle_volume contains a nonfinite non-nil value");
            }
        } else if (option->type() == coEnum) value = option->serialize();
        else if (option->type() == coBool) value = option->getBool();
        else {
            // Native numeric serialize() is canonical; retain typed numbers/arrays, never JSON text.
            if (option->is_vector() && dynamic_cast<const ConfigOptionVectorBase&>(*option).size() > k_geometry_byte_limit / 2)
                refuse("output_limit", "Control vector exceeds the success-object byte limit");
            const std::string text = option->serialize();
            const auto parsed = json::parse(option->is_vector() ? "[" + text + "]" : text, nullptr, false);
            if (parsed.is_discarded()) refuse("unsupported_source", std::string(key) + " cannot be represented as native JSON numbers");
            value = parsed;
        }
        controls[key] = {{"value", value}, {"unit", unit ? json(unit) : json(nullptr)},
            {"availability", option ? "recorded" : "unavailable"}, {"source", "print_config"}, {"reason", reason}};
        if (controls.dump().size() > k_geometry_byte_limit) refuse("output_limit", "Control metadata exceeds the success-object byte limit");
    }
    return controls;
}
json plate_analysis_page(const GCodeProcessorResult& result, const PlateAnalysisRequest& request,
    const GeometryIdentity& identity, const json& controls, Clock::time_point gui_started)
{
    if (result.filaments_count < 1 || result.filaments_count > MAXIMUM_EXTRUDER_NUMBER)
        refuse("unsupported_source", "Native configured filament count is outside the supported accounting domain");
    if (result.moves.size() <= 1) refuse("no_valid_slice", "The native result has no physical move stream");
    auto out = common(identity, request.detail);
    const auto scan_started = Clock::now();
    out = request.detail ? detail_page(result, request, std::move(out)) : summary_page(result, controls, std::move(out));
    const auto scan_finished = Clock::now();
    const auto serialization_started = Clock::now();
    const auto without_timing = out.dump();
    const auto serialization_finished = Clock::now();
    using Ms = std::chrono::duration<double, std::milli>;
    out["timing_ms"] = {{"gui_total", Ms(serialization_finished - gui_started).count()},
        {"scan", request.detail ? Ms(scan_finished - scan_started).count() : 0.},
        {"serialization", Ms(serialization_finished - serialization_started).count()}};
    if (without_timing.size() > k_geometry_byte_limit || out.dump().size() > k_geometry_byte_limit)
        refuse("output_limit", "Success metadata exceeds the 64 KiB complete JSON limit");
    return out;
}

}}} // namespace Slic3r::GUI::OrcaMCP
