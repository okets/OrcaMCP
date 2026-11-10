#include "OrcaMCPMoveFrame.hpp"
#include "OrcaMCPDigest.hpp"
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {
namespace {
const char* reason_name(MoveFrameReason reason)
{
    switch (reason) {
    case MoveFrameReason::None: return nullptr;
    case MoveFrameReason::NotCaptured: return "not_captured";
    case MoveFrameReason::ConfigurationUnknown: return "configuration_unknown";
    case MoveFrameReason::UnsupportedTransform: return "unsupported_transform";
    case MoveFrameReason::InvalidSlotCount: return "invalid_slot_count";
    case MoveFrameReason::UnresolvedOffsets: return "unresolved_offsets";
    case MoveFrameReason::NonfiniteInputs: return "nonfinite_inputs";
    case MoveFrameReason::InputsChangedDuringProcessing: return "inputs_changed_during_processing";
    case MoveFrameReason::InvalidFilamentSlot: return "invalid_filament_slot";
    case MoveFrameReason::ReloadFailedSourceMismatch: return "reload_failed_source_mismatch";
    }
    return "configuration_unknown";
}
const char* config_kind(MoveFrameConfigKind kind)
{
    switch (kind) {
    case MoveFrameConfigKind::TypedPrintConfig: return "typed_print_config";
    case MoveFrameConfigKind::ExplicitGcodeConfigBlock: return "explicit_gcode_config_block";
    case MoveFrameConfigKind::NoneOrDefaulted: return "none_or_defaulted";
    }
    return "none_or_defaulted";
}
const char* file_state(MoveFrameSourceFileState state)
{
    switch (state) {
    case MoveFrameSourceFileState::ReloadSucceeded: return "reload_succeeded";
    case MoveFrameSourceFileState::ReloadFailed: return "reload_failed";
    case MoveFrameSourceFileState::NotVerified: return "not_verified";
    }
    return "not_verified";
}
}

nlohmann::json coordinate_frame_json(const MoveFrameProvenance& frame, const std::string& result_id)
{
    using nlohmann::json;
    bool captured = frame.captured;
    MoveFrameReason reason = frame.reason;
    const bool provenance_known = frame.config_kind == MoveFrameConfigKind::TypedPrintConfig ||
        (frame.config_kind == MoveFrameConfigKind::ExplicitGcodeConfigBlock && (frame.explicit_frame_keys & 1));
    if (captured && (!provenance_known || !frame.belt_config_known || frame.basis == MoveFrameBasis::None ||
        !frame.start_custom_z_pinned || !frame.machine_transform_known)) {
        captured = false; reason = MoveFrameReason::ConfigurationUnknown;
    }
    if (captured && (frame.extruder_offsets_mm.empty() || frame.extruder_offsets_mm.size() > MAXIMUM_EXTRUDER_NUMBER)) {
        captured = false; reason = MoveFrameReason::InvalidSlotCount;
    }
    if (captured) {
        bool finite = std::isfinite(frame.xy_mm[0]) && std::isfinite(frame.xy_mm[1]) && std::isfinite(frame.z_offset_mm) &&
            std::isfinite(frame.first_layer_height_mm) && std::isfinite(frame.belt_tilt_angle) && std::isfinite(frame.belt_z_origin);
        for (const auto& offset : frame.extruder_offsets_mm) finite = finite && offset.allFinite();
        if (!finite) { captured = false; reason = MoveFrameReason::NonfiniteInputs; }
    }
    if (frame.source_file_state == MoveFrameSourceFileState::ReloadFailed) reason = MoveFrameReason::ReloadFailedSourceMismatch;
    const bool supported = captured && frame.classification == MoveFrameClassification::OrdinaryNonbeltTranslation &&
        frame.belt_config_known && !frame.belt_configured && !frame.processor_belt_printer && frame.machine_transform_known && !frame.machine_transform_active &&
        !frame.belt_header_observed && frame.belt_tilt_angle == 0.f && frame.belt_z_origin == 0.f;
    if (captured && reason == MoveFrameReason::None && !supported) reason = MoveFrameReason::UnsupportedTransform;
    if (!captured && reason == MoveFrameReason::None) reason = MoveFrameReason::NotCaptured;
    const bool available = supported && reason == MoveFrameReason::None;
    Digest hash;
    hash.text("orcamcp-move-frame-v1"); hash.number(captured); hash.number(uint64_t(frame.basis));
    hash.number(uint64_t(frame.config_kind)); hash.number(frame.explicit_frame_keys);
    hash.number(uint64_t(frame.classification)); hash.number(uint64_t(reason)); hash.number(uint64_t(frame.source_file_state));
    hash.number(frame.belt_config_known); hash.number(frame.belt_configured); hash.number(frame.processor_belt_printer); hash.number(frame.machine_transform_known);
    hash.number(frame.machine_transform_active); hash.number(frame.belt_header_observed); hash.number(frame.start_custom_z_pinned);
    json offsets = nullptr, start = nullptr;
    if (captured) {
        hash.scalar(frame.xy_mm[0]); hash.scalar(frame.xy_mm[1]); hash.scalar(frame.z_offset_mm);
        hash.scalar(frame.first_layer_height_mm); hash.scalar(frame.belt_tilt_angle); hash.scalar(frame.belt_z_origin);
        hash.number(frame.extruder_offsets_mm.size());
        json slots = json::array();
        for (const auto& offset : frame.extruder_offsets_mm) {
            hash.xyz(offset);
            slots.push_back({double(offset.x()), double(offset.y()), double(offset.z())});
        }
        offsets = {{"xy_mm", frame.xy_mm}, {"z_offset_mm", double(frame.z_offset_mm)}, {"extruder_offsets_mm", slots}};
        start = {{"policy", "first_layer_height_plus_extruder_z"}, {"first_layer_height_mm", double(frame.first_layer_height_mm)}};
    }
    json keys = json::array();
    constexpr const char* names[] = {"belt_printer", "z_offset", "extruder_offset", "initial_layer_print_height"};
    for (unsigned i = 0; i < 4; ++i) if (frame.explicit_frame_keys & (1u << i)) keys.push_back(names[i]);
    const char* why = reason_name(reason);
    return {{"version", 1}, {"frame", "gcode_processor_preview"}, {"binding", captured ? "captured_with_moves" : "unavailable"},
        {"config_provenance", {{"kind", config_kind(frame.config_kind)}, {"explicit_frame_keys", keys}}},
        {"result_id", result_id}, {"frame_digest", hash.finish()},
        {"normalization", {{"availability", available ? "available" : "unavailable"},
            {"kind", available ? json("per_filament_translation") : json(nullptr)}, {"reason", why ? json(why) : json(nullptr)}}},
        {"processor_offsets", offsets}, {"start_custom_z", start}, {"source_file_state", file_state(frame.source_file_state)}};
}
}}}
