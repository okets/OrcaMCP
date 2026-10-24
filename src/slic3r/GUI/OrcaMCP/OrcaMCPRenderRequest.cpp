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

bool is_number_array3(const nlohmann::json& v)
{
    return v.is_array() && v.size() == 3 && std::all_of(v.begin(), v.end(), [](const nlohmann::json& n) { return n.is_number(); });
}

Vec3d read_point3(const nlohmann::json& v, const std::string& what)
{
    if (!is_number_array3(v))
        throw std::runtime_error(what + " must be [x, y, z] numbers in mm");
    return Vec3d(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
}

// fit: {object_id} or {object_index} frames one object; {box: {min, max}} a region; {point, radius_mm} a sphere;
// anything else frames the plate.
FitRequest read_fit(const nlohmann::json& fit);

} // namespace

FitRequest read_fit_request(const nlohmann::json& fit) { return read_fit(fit); }

namespace {

FitRequest read_fit(const nlohmann::json& fit)
{
    FitRequest out;
    if (fit.is_null() || (fit.is_string() && fit == "plate"))
        return out;
    static const char* const k_usage = "fit must be \"plate\", {object_id: n}, {box: {min, max}} or {point, radius_mm}";
    if (!fit.is_object())
        throw std::runtime_error(k_usage);
    for (const auto& item : fit.items())
        if (item.key() != "object_id" && item.key() != "object_index" && item.key() != "box" && item.key() != "point" &&
            item.key() != "radius_mm")
            throw std::runtime_error("fit has no key \"" + item.key() + "\"; " + k_usage);
    if (fit.empty())
        throw std::runtime_error(k_usage);
    const bool has_id = fit.contains("object_id"), has_index = fit.contains("object_index");
    const bool has_object = has_id || has_index, has_box = fit.contains("box");
    const bool has_point = fit.contains("point"), has_radius = fit.contains("radius_mm");
    const int  forms = int(has_object) + int(has_box) + int(has_point);
    if (forms > 1)
        throw std::runtime_error("fit takes one of {object_id}, {box: {min, max}} or {point, radius_mm}");
    if (has_radius && !has_point)
        throw std::runtime_error("fit.radius_mm goes with fit.point");
    if (has_box) {
        const nlohmann::json& box = fit["box"];
        if (!box.is_object() || !box.contains("min") || !box.contains("max"))
            throw std::runtime_error("fit.box must be {min: [x, y, z], max: [x, y, z]} in mm");
        out.kind    = FitRequest::Kind::Box;
        out.box_min = read_point3(box["min"], "fit.box.min");
        out.box_max = read_point3(box["max"], "fit.box.max");
        if (!(out.box_min.array() <= out.box_max.array()).all())
            throw std::runtime_error("fit.box.min must not exceed fit.box.max on any axis");
    } else if (has_point) {
        if (!has_radius)
            throw std::runtime_error("fit.point needs fit.radius_mm (a number above 0)");
        out.kind = FitRequest::Kind::Point;
        out.point = read_point3(fit["point"], "fit.point");
        if (!fit["radius_mm"].is_number() || !(fit["radius_mm"].get<double>() > 0.))
            throw std::runtime_error("fit.radius_mm must be a number above 0");
        out.radius_mm = fit["radius_mm"].get<double>();
    } else if (has_object) {
        const int id    = has_id ? fit["object_id"].get<int>() : 0;
        const int index = has_index ? fit["object_index"].get<int>() : 0;
        if (has_id && has_index && id != index)
            throw std::runtime_error("fit.object_id and fit.object_index are the same number; send object_id only");
        out.kind      = FitRequest::Kind::Object;
        out.object_id = has_id ? id : index;
    }
    return out;
}

std::string read_choice(const nlohmann::json& view, const char* key, const char* first, const char* second)
{
    if (!view[key].is_string() || (view[key] != first && view[key] != second))
        throw std::runtime_error(std::string(key) + " must be \"" + first + "\" or \"" + second + "\"");
    return view[key].get<std::string>();
}

double read_number(const nlohmann::json& view, const char* key)
{
    if (!view[key].is_number())
        throw std::runtime_error(std::string(key) + " must be a number");
    return view[key].get<double>();
}

// What a camera object a render returned carries beyond the three keys a replay needs (the numbers that describe the
// picture, and the pose it was asked for). Accepted back and ignored: the matrices are what is drawn.
bool is_camera_answer_key(const std::string& key)
{
    static const char* const keys[] = {"view_matrix", "projection_matrix", "viewport", "type", "pixel_origin", "projection",
                                       "camera_mode", "fov_deg", "mm_per_pixel", "moved", "requested_position", "replayed",
                                       "camera_position", "target", "preset", "fit", "input_frame", "plate_index", "near_mm"};
    for (const char* k : keys)
        if (key == k)
            return true;
    return false;
}

// A view that is a camera an earlier render returned, either as views[i].camera or as the camera object itself:
// drawn exactly, so no other camera key goes with the first form; the second takes only the keys a camera carries.
ViewRequest read_replay_view(const nlohmann::json& view, const nlohmann::json& camera)
{
    const bool inline_camera = &view == &camera;
    for (const auto& item : view.items())
        if (inline_camera ? !is_camera_answer_key(item.key()) : item.key() != "camera")
            throw std::runtime_error(inline_camera
                                         ? "a view made of a returned camera takes only the keys a camera carries (got " + item.key() + ")"
                                         : "camera draws a view exactly as an earlier render returned it; a view with camera takes no other "
                                           "key (got " + item.key() + ")");
    ViewRequest out;
    CameraFrame frame;
    std::string error;
    if (!parse_camera_frame(camera, frame, error))
        throw std::runtime_error(error);
    if (frame.viewport[2] != frame.viewport[3] || frame.viewport[2] < 32 || frame.viewport[2] > 2048)
        throw std::runtime_error("camera.viewport must be a square of 32 to 2048 pixels, as render_plate_view draws");
    out.replay = frame;
    out.mode   = CameraMode::Exact;
    out.orthographic = !frame_is_perspective(frame);
    if (camera.contains("camera_position") && is_number_array3(camera["camera_position"]))
        out.camera_position = read_point3(camera["camera_position"], "camera.camera_position");
    if (camera.contains("target") && is_number_array3(camera["target"]))
        out.target = read_point3(camera["target"], "camera.target");
    if (camera.contains("plate_index") && camera["plate_index"].is_number_integer())
        out.replay_plate = camera["plate_index"].get<int>();
    out.fit_json = nlohmann::json();
    return out;
}

// camera_mode, projection, fov_deg, ortho_height_mm of a view whose camera is not a replay.
void read_lens(const nlohmann::json& view, ViewRequest& out)
{
    if (view.contains("camera_mode"))
        out.mode = read_choice(view, "camera_mode", "fit", "exact") == "exact" ? CameraMode::Exact : CameraMode::Fit;
    if (view.contains("projection"))
        out.orthographic = read_choice(view, "projection", "perspective", "orthographic") == "orthographic";
    if (view.contains("fov_deg")) {
        if (!out.exact())
            throw std::runtime_error("fov_deg is for camera_mode exact; in fit mode the picture is zoomed to the fit");
        if (out.orthographic)
            throw std::runtime_error("fov_deg is for a perspective camera; an orthographic one takes ortho_height_mm");
        const double fov = read_number(view, "fov_deg");
        if (!(fov >= 5. && fov <= 120.))
            throw std::runtime_error("fov_deg must be between 5 and 120");
        out.fov_deg = fov;
    }
    if (view.contains("ortho_height_mm")) {
        if (!out.exact())
            throw std::runtime_error("ortho_height_mm is for camera_mode exact with projection orthographic");
        if (!out.orthographic)
            throw std::runtime_error("ortho_height_mm is for projection orthographic; a perspective camera takes fov_deg");
        const double height = read_number(view, "ortho_height_mm");
        if (!(height > 0.))
            throw std::runtime_error("ortho_height_mm must be above 0");
        out.ortho_height_mm = height;
    }
    if (!out.exact())
        return;
    if ((view.contains("preset") && !view["preset"].is_null()) || view.contains("fit"))
        throw std::runtime_error("camera_mode exact honours camera_position and target; frame a region with fit instead, "
                                 "or leave camera_mode out and send a preset");
    if (!view.contains("camera_position") || !view.contains("target"))
        throw std::runtime_error("camera_mode exact needs camera_position and target");
    if (out.orthographic && !out.ortho_height_mm)
        throw std::runtime_error("an exact orthographic camera needs ortho_height_mm, the height of the image in mm");
    if (!out.orthographic && !out.fov_deg)
        out.fov_deg = 30.;
}

ViewRequest read_view(const nlohmann::json& view)
{
    if (!view.is_object())
        throw std::runtime_error("each view must be an object");
    if (view.contains("camera"))
        return read_replay_view(view, view["camera"]);
    if (view.contains("view_matrix"))
        return read_replay_view(view, view);

    ViewRequest out;
    if (view.contains("fit")) {
        out.fit = read_fit(view["fit"]);
        if (out.fit.kind != FitRequest::Kind::Plate)
            out.fit_json = view["fit"];
    }
    read_lens(view, out);

    out.input_frame = view.value("frame", std::string("bed_mm"));
    if (out.input_frame != "bed_mm" && out.input_frame != "plate_local")
        throw std::runtime_error("frame must be \"bed_mm\" or \"plate_local\"");
    if (view.contains("preset") && !view["preset"].is_null()) {
        out.preset_name = view["preset"].get<std::string>();
        CameraPreset preset;
        if (!camera_preset_from_string(out.preset_name, preset))
            throw std::runtime_error("unknown preset '" + out.preset_name + "'; use iso, top, front, back, left, right or low");
        out.preset = preset;
        // A preset's own position is bed mm; the frame matters only for a fit region given in it.
        const bool region = out.fit.kind == FitRequest::Kind::Box || out.fit.kind == FitRequest::Kind::Point;
        if (!region)
            out.input_frame = "bed_mm";
    } else {
        if (!view.contains("camera_position") || !view.contains("target"))
            throw std::runtime_error("each view needs a preset, or camera_position and target");
        out.camera_position = read_vec3(view["camera_position"], "camera_position");
        out.target          = read_vec3(view["target"], "target");
        if ((*out.camera_position - *out.target).norm() < 1e-9)
            throw std::runtime_error("camera_position and target are the same point; the camera needs a direction");
    }
    return out;
}

// A replayed camera is drawn at its own viewport: that is the resolution unless the call asked for another, which
// would stretch the matrices over a different image.
void adopt_replay_resolution(const nlohmann::json& payload, RenderRequest& request)
{
    std::optional<int> replayed;
    for (const ViewRequest& view : request.views) {
        if (!view.replay)
            continue;
        const int size = view.replay->viewport[2];
        if (replayed && *replayed != size)
            throw std::runtime_error("the cameras to replay were drawn at different resolutions (" + std::to_string(*replayed) + " and " +
                                     std::to_string(size) + "); replay them in separate calls");
        replayed = size;
    }
    if (!replayed)
        return;
    if (payload.contains("resolution") && request.resolution != *replayed)
        throw std::runtime_error("resolution " + std::to_string(request.resolution) + " differs from the camera's viewport (" +
                                 std::to_string(*replayed) + "); a replayed camera is drawn at its own, so leave resolution out");
    request.resolution = *replayed;
}

// paint (channels), texture_layers (slots) and color_by, read together because they refuse each other's mistakes.
void read_paint_options(const nlohmann::json& payload, RenderRequest& out)
{
    const std::vector<std::string> channels = {"support", "seam", "fuzzy_skin", "texture"};
    const std::string              channel_list = "support, seam, fuzzy_skin, texture";
    if (payload.contains("paint")) {
        const nlohmann::json& paint = payload["paint"];
        if (!paint.is_array())
            throw std::runtime_error("paint must be a list of channels to tint: " + channel_list);
        for (const nlohmann::json& item : paint) {
            const std::string name = item.is_string() ? item.get<std::string>() : std::string();
            if (std::find(channels.begin(), channels.end(), name) == channels.end())
                throw std::runtime_error("paint: " + (item.is_string() ? "\"" + name + "\"" : item.dump()) +
                                         " is not a channel; the channels are " + channel_list +
                                         " (colour paint is always drawn in its filaments' colours)");
        }
        for (const std::string& channel : channels)   // canonical order, duplicates dropped
            for (const nlohmann::json& item : paint)
                if (item.get<std::string>() == channel) {
                    out.paint.push_back(channel);
                    break;
                }
    }
    if (payload.contains("texture_layers")) {
        const nlohmann::json& layers = payload["texture_layers"];
        if (std::find(out.paint.begin(), out.paint.end(), "texture") == out.paint.end())
            throw std::runtime_error("texture_layers chooses which texture layers paint draws; add \"texture\" to paint, or leave texture_layers out");
        if (!layers.is_array())
            throw std::runtime_error("texture_layers must be a list of layer slots, each an integer 0 to 7");
        for (const nlohmann::json& slot : layers) {
            if (!slot.is_number_integer() || slot.get<long long>() < 0 || slot.get<long long>() > 7)
                throw std::runtime_error("texture_layers: " + slot.dump() + " is not a layer slot; a slot is an integer 0 to 7");
            out.texture_layers.push_back(slot.get<int>());
        }
        std::sort(out.texture_layers.begin(), out.texture_layers.end());
        out.texture_layers.erase(std::unique(out.texture_layers.begin(), out.texture_layers.end()), out.texture_layers.end());
    }
    if (payload.contains("color_by")) {
        const std::string by = payload["color_by"].is_string() ? payload["color_by"].get<std::string>() : std::string();
        if (by == "object")        out.color_by = ColorBy::Object;
        else if (by == "filament") out.color_by = ColorBy::Filament;
        else throw std::runtime_error("color_by must be \"object\" (one colour per object, the default) or \"filament\" (each part in its filament's colour)");
    }
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

    if (payload.contains("light"))
        out.light = read_light(payload["light"]);
    if (payload.contains("section") && !payload["section"].is_null())
        out.section = read_section(payload["section"]);
    read_paint_options(payload, out);
    if (payload.contains("texture_preview")) {
        if (!payload["texture_preview"].is_boolean())
            throw std::runtime_error("texture_preview must be true or false");
        out.texture_preview = payload["texture_preview"].get<bool>();
    }
    out.layer_view = payload.value("layer_view", nlohmann::json());
    if (out.layer_view == "")  // "" was always read as no layer view
        out.layer_view = nlohmann::json();
    if (!out.layer_view.is_null()) {
        if (out.light)
            throw std::runtime_error("light is for 3D views; a layer_view is a plan drawn from the G-code. Leave light out, or leave layer_view out");
        if (out.section)
            throw std::runtime_error("section is for 3D views; a layer_view is a plan drawn from the G-code. Leave section out, or leave layer_view out");
        if (!out.paint.empty() || payload.contains("color_by") || payload.contains("texture_layers"))
            throw std::runtime_error("paint, texture_layers and color_by are for 3D views; a layer_view is a plan drawn from the G-code. Leave them out, or leave layer_view out");
        if (out.texture_preview)
            throw std::runtime_error("texture_preview is for 3D views; a layer_view is a plan drawn from the G-code. Leave it out, or leave layer_view out");
        return out;  // a layer plan ignores views
    }

    if (out.texture_preview && std::find(out.paint.begin(), out.paint.end(), "texture") != out.paint.end())
        throw std::runtime_error("texture paint is drawn on the unbaked surface and texture_preview draws the displaced one; render the paint and the preview separately");
    if (out.texture_preview && out.section)
        throw std::runtime_error("a section cuts the unbaked meshes and cannot cut or cap a texture preview; render them separately (after a bake the section cuts the real relief)");

    // No views: a contact sheet of the three presets an agent reaches for first, fitted to the plate.
    nlohmann::json views = payload.value("views", nlohmann::json());
    out.contact_sheet    = !views.is_array();
    if (out.contact_sheet)
        views = nlohmann::json::array({{{"preset", "iso"}}, {{"preset", "top"}}, {{"preset", "front"}}});
    if (views.empty())
        throw std::runtime_error("views must not be empty");
    for (const nlohmann::json& view : views)
        out.views.push_back(read_view(view));
    adopt_replay_resolution(payload, out);
    for (const ViewRequest& view : out.views)
        if (view.replay_plate && *view.replay_plate != out.plate_index)
            throw std::runtime_error("this camera was drawn of plate " + std::to_string(*view.replay_plate) + ", not plate " +
                                     std::to_string(out.plate_index) + "; replay it with plate_index " + std::to_string(*view.replay_plate) +
                                     ", or frame this plate with a preset");
    return out;
}

} // namespace

BoundingBoxf3 region_fit_box(const FitRequest& fit, const Vec3d& frame_origin)
{
    if (fit.kind == FitRequest::Kind::Box)
        return BoundingBoxf3(fit.box_min + frame_origin, fit.box_max + frame_origin);
    if (fit.kind == FitRequest::Kind::Point) {
        const Vec3d radius = Vec3d::Constant(fit.radius_mm);
        return BoundingBoxf3(fit.point + frame_origin - radius, fit.point + frame_origin + radius);
    }
    return BoundingBoxf3();
}

ExactCameraSpec exact_spec_of(const ViewRequest& view)
{
    ExactCameraSpec spec;
    spec.perspective     = !view.orthographic;
    spec.fov_deg         = view.fov_deg.value_or(30.);
    spec.ortho_height_mm = view.ortho_height_mm.value_or(0.);
    return spec;
}

bool ShaderSettings::is_default() const
{
    const ShaderSettings d;
    return clipping_plane == d.clipping_plane && use_light_dir == d.use_light_dir && light_dir_eye == d.light_dir_eye &&
           ambient == d.ambient && front_light == d.front_light && specular == d.specular &&
           top_gain == d.top_gain;
}

const std::vector<std::string>& shader_settings_uniform_names()
{
    static const std::vector<std::string> names = {"clipping_plane", "use_light_dir", "light_dir_eye", "ambient", "front_light", "specular", "top_gain"};
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
