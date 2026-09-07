#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPlateUtils.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Helper to get active warnings as JSON object (always includes count, even if 0)
nlohmann::json get_active_warnings_json(Plater* plater) {
    nlohmann::json result;
    nlohmann::json warnings_array = nlohmann::json::array();

    if (plater) {
        auto* notification_manager = plater->get_notification_manager();
        if (notification_manager) {
            auto warnings = notification_manager->get_active_warnings();
            for (const auto& warning : warnings) {
                warnings_array.push_back({
                    {"level", warning.level},
                    {"message", warning.message},
                    {"type", warning.type}
                });
            }
        }
    }

    result["count"] = warnings_array.size();
    result["warnings"] = warnings_array;
    return result;
}

// Helper to add turntable preview to result if requested
void add_turntable_preview_if_requested(nlohmann::json& result, bool include_preview,
                                         int view_count, int resolution) {
    if (!include_preview) return;

    Plater* plater = wxGetApp().plater();
    int plate_index = plater->get_partplate_list().get_curr_plate_index();

    nlohmann::json preview = OrcaMCPPlateUtils::CaptureTurntablePreview(plate_index, view_count, resolution);
    if (preview.contains("preview_path")) {
        result["preview_path"] = preview["preview_path"];
    }
}

}}} // namespace Slic3r::GUI::OrcaMCP
