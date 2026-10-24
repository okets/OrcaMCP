#include "OrcaMCPRenderRequest.hpp"

#include <algorithm>
#include <stdexcept>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// [x, y, z] or {x, y, z}, bed mm.
Vec3d read_vec3(const nlohmann::json& v, const char* what)
{
    if (v.is_array() && v.size() >= 3)
        return Vec3d(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
    if (v.is_object())
        return Vec3d(v.value("x", 0.0), v.value("y", 0.0), v.value("z", 0.0));
    throw std::runtime_error(std::string(what) + " must be [x, y, z]");
}

// fit: {object_id} or {object_index} frames one object; anything else frames the plate.
FitRequest read_fit(const nlohmann::json& fit)
{
    FitRequest out;
    if (!fit.is_object())
        return out;
    const bool has_id = fit.contains("object_id"), has_index = fit.contains("object_index");
    if (!has_id && !has_index)
        return out;
    const int id    = has_id ? fit["object_id"].get<int>() : 0;
    const int index = has_index ? fit["object_index"].get<int>() : 0;
    if (has_id && has_index && id != index)
        throw std::runtime_error("fit.object_id and fit.object_index are the same number; send object_id only");
    out.kind      = FitRequest::Kind::Object;
    out.object_id = has_id ? id : index;
    return out;
}

ViewRequest read_view(const nlohmann::json& view)
{
    if (!view.is_object())
        throw std::runtime_error("each view must be an object");

    ViewRequest out;
    if (view.contains("fit")) {
        out.fit = read_fit(view["fit"]);
        if (out.fit.kind == FitRequest::Kind::Object)
            out.fit_json = view["fit"];
    }

    out.input_frame = view.value("frame", std::string("bed_mm"));
    if (view.contains("preset")) {
        out.preset_name = view["preset"].get<std::string>();
        CameraPreset preset;
        if (!camera_preset_from_string(out.preset_name, preset))
            throw std::runtime_error("unknown preset '" + out.preset_name + "'; use iso, top, front, back, left, right or low");
        out.preset      = preset;
        out.input_frame = "bed_mm";
    } else {
        if (!view.contains("camera_position") || !view.contains("target"))
            throw std::runtime_error("each view needs a preset, or camera_position and target");
        out.camera_position = read_vec3(view["camera_position"], "camera_position");
        out.target          = read_vec3(view["target"], "target");
        if (out.input_frame != "bed_mm" && out.input_frame != "plate_local")
            throw std::runtime_error("frame must be \"bed_mm\" or \"plate_local\"");
    }
    return out;
}

RenderRequest read_request(const nlohmann::json& payload)
{
    if (payload.is_null() || payload.value("plate_index", -1) == -1)
        throw std::runtime_error("Missing required parameter plate_index");

    RenderRequest out;
    out.plate_index  = payload.value("plate_index", -1);
    out.save_to_file = payload.value("save_to_file", false);
    out.resolution   = std::clamp(payload.value("resolution", 512), 32, 2048);
    // Files default to PNG; inline base64 defaults to JPEG, where size matters more than crispness.
    const std::string image_format = payload.value("image_format", std::string(out.save_to_file ? "png" : "jpeg"));
    if (image_format != "png" && image_format != "jpeg")
        throw std::runtime_error("image_format must be \"png\" or \"jpeg\"");
    out.use_png  = image_format == "png";
    // Overlays default on (true/missing); false disables all; an object picks per overlay.
    out.overlays = overlay_options_from_json(payload.value("overlays", nlohmann::json(true)));

    out.layer_view = payload.value("layer_view", nlohmann::json());
    if (out.layer_view == "")  // "" was always read as no layer view
        out.layer_view = nlohmann::json();
    if (!out.layer_view.is_null())
        return out;  // a layer plan ignores views

    // No views: a contact sheet of the three presets an agent reaches for first, fitted to the plate.
    nlohmann::json views = payload.value("views", nlohmann::json());
    out.contact_sheet    = !views.is_array();
    if (out.contact_sheet)
        views = nlohmann::json::array({{{"preset", "iso"}}, {{"preset", "top"}}, {{"preset", "front"}}});
    if (views.empty())
        throw std::runtime_error("views must not be empty");
    for (const nlohmann::json& view : views)
        out.views.push_back(read_view(view));
    return out;
}

} // namespace

bool ShaderSettings::is_default() const
{
    const ShaderSettings d;
    return clipping_plane == d.clipping_plane && use_light_dir == d.use_light_dir && light_dir_eye == d.light_dir_eye &&
           ambient == d.ambient && front_light == d.front_light;
}

const std::vector<std::string>& shader_settings_uniform_names()
{
    static const std::vector<std::string> names = {"clipping_plane", "use_light_dir", "light_dir_eye", "ambient", "front_light"};
    return names;
}

std::optional<RenderRequest> read_render_request(const nlohmann::json& payload, std::string& error)
{
    try {
        return read_request(payload);
    } catch (const nlohmann::json::exception& e) {
        error = std::string("render_plate_view: a value has the wrong type (") + e.what() + ")";
    } catch (const std::exception& e) {
        error = e.what();
    }
    return std::nullopt;
}

}}} // namespace Slic3r::GUI::OrcaMCP
