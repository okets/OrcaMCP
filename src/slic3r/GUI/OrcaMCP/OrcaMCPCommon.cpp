#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPlateUtils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/NotificationManager.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

bool is_hex_color(const std::string& value, bool allow_alpha)
{
    if (value.size() != 7 && !(allow_alpha && value.size() == 9))
        return false;
    if (value.front() != '#')
        return false;
    for (size_t i = 1; i < value.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            return false;
    return true;
}

bool parse_integer_param(const nlohmann::json& value, int& out)
{
    if (value.is_number_integer()) {
        out = value.get<int>();
        return true;
    }
    if (value.is_number_float()) {
        const double d = value.get<double>();
        if (d != std::floor(d) || std::abs(d) > 1e9)
            return false;
        out = int(d);
        return true;
    }
    if (value.is_string()) {
        const std::string str = value.get<std::string>();
        try {
            size_t    pos    = 0;
            const int parsed = std::stoi(str, &pos);
            if (pos != str.size())
                return false;
            out = parsed;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

bool parse_boolean_param(const nlohmann::json& value, bool& out)
{
    if (value.is_boolean()) {
        out = value.get<bool>();
        return true;
    }
    // 0/1 and "true"/"false"/"1"/"0" only: anything else is a caller mistake worth reporting rather
    // than a value to guess at.
    int as_int = 0;
    if (value.is_number() && parse_integer_param(value, as_int) && (as_int == 0 || as_int == 1)) {
        out = as_int == 1;
        return true;
    }
    if (value.is_string()) {
        const std::string str = value.get<std::string>();
        if (str == "true" || str == "1") {
            out = true;
            return true;
        }
        if (str == "false" || str == "0") {
            out = false;
            return true;
        }
    }
    return false;
}

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
