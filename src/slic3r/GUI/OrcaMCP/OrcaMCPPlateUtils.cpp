#include "OrcaMCPPlateUtils.hpp"
#include "OrcaMCPPlateOccupancy.hpp"
#include "OrcaMCPCommon.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderOverlay.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPFirstLayerPlan.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPFilamentModel.hpp"
#include <glad/gl.h>
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <wx/mstream.h>
#include <wx/dcmemory.h>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <openssl/md5.h>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace Slic3r { namespace GUI {

namespace {
    nlohmann::json bbox_to_json(const BoundingBoxf3& bbox) {
        return {
            {"min", {
                {"x", bbox.min.x()},
                {"y", bbox.min.y()},
                {"z", bbox.min.z()}
            }},
            {"max", {
                {"x", bbox.max.x()},
                {"y", bbox.max.y()},
                {"z", bbox.max.z()}
            }}
        };
    }

    // One shape for every rectangle on the bed, so an agent comparing a tower against an object
    // against an exclusion area is comparing four numbers of the same kind in the same frame.
    nlohmann::json rect_to_json(const BoundingBoxf& box) {
        return {
            {"min_x", box.min.x()}, {"min_y", box.min.y()},
            {"max_x", box.max.x()}, {"max_y", box.max.y()},
            {"size_x", box.max.x() - box.min.x()}, {"size_y", box.max.y() - box.min.y()}
        };
    }

    BoundingBoxf translated(const BoundingBoxf& box, const Vec2d& offset) {
        BoundingBoxf out;
        out.min = box.min + offset;
        out.max = box.max + offset;
        out.defined = box.defined;
        return out;
    }

    // A per-object override if the object carries one, the global print setting otherwise. The two
    // differ far more often than not for brim keys -- brim_width is exactly the kind of thing set
    // per object -- and reading only the global one would report the wrong printed area for the
    // object most likely to have a brim at all.
    double object_or_global_float(const ModelObject& obj, const DynamicPrintConfig& print_cfg,
                                  const char* key, double fallback) {
        if (obj.config.has(key)) return obj.config.opt_float(key);
        if (const auto* opt = print_cfg.option<ConfigOptionFloat>(key)) return opt->value;
        return fallback;
    }

    std::string object_or_global_enum(const ModelObject& obj, const DynamicPrintConfig& print_cfg,
                                      const char* key, const std::string& fallback) {
        if (obj.config.has(key)) return obj.config.opt_serialize(key);
        if (print_cfg.has(key)) return print_cfg.opt_serialize(key);
        return fallback;
    }
}

static void z_debug_output_thumbnail(const ThumbnailData& thumbnail_data, std::string file_name)
{
    // debug export of generated image
    wxImage image(thumbnail_data.width, thumbnail_data.height);
    image.InitAlpha();

    for (unsigned int r = 0; r < thumbnail_data.height; ++r)
    {
        unsigned int rr = (thumbnail_data.height - 1 - r) * thumbnail_data.width;
        for (unsigned int c = 0; c < thumbnail_data.width; ++c)
        {
            unsigned char* px = (unsigned char*)thumbnail_data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }

    std::string file_name_path = "/Users/kenneth/Desktop/" + file_name + ".png";
    image.SaveFile(file_name_path, wxBITMAP_TYPE_PNG);
}

static std::string encode_image_to_base64(const wxImage& image, bool use_png);
static std::string encode_thumbnail_to_base64(const ThumbnailData& thumbnail_data, bool use_png = true)
{
    return encode_image_to_base64(OrcaMCP::thumbnail_to_wximage(thumbnail_data), use_png);
}
static std::string encode_image_to_base64(const wxImage& image, bool use_png) {

    // Convert wxImage to memory stream
    wxMemoryOutputStream stream;
    if (use_png) {
        image.SaveFile(stream, wxBITMAP_TYPE_PNG);
    } else {
        image.SaveFile(stream, wxBITMAP_TYPE_JPEG);
    }

    // Get the binary data
    wxStreamBuffer* buf = stream.GetOutputStreamBuffer();
    const size_t data_size = buf->GetBufferSize();
    std::vector<unsigned char> buffer(data_size);
    std::memcpy(buffer.data(), buf->GetBufferStart(), data_size);

    // Convert to base64
    std::string base64_data;
    base64_data.resize(boost::beast::detail::base64::encoded_size(data_size));
    boost::beast::detail::base64::encode(base64_data.data(), buffer.data(), data_size);

    // Add appropriate data URI prefix
    std::string data_uri = "data:image/";
    data_uri += (use_png ? "png" : "jpeg");
    data_uri += ";base64,";
    data_uri += base64_data;

    return data_uri;
}

static std::string save_image_to_file(const wxImage& image, int view_index, bool use_png);
static std::string save_thumbnail_to_file(const ThumbnailData& thumbnail_data, int view_index, bool use_png)
{
    return save_image_to_file(OrcaMCP::thumbnail_to_wximage(thumbnail_data), view_index, use_png);
}
static std::string save_image_to_file(const wxImage& image, int view_index, bool use_png) {

    // Generate unique filename in temp directory
    // Time alone collided: three renders in one second overwrote each other. The sequence number
    // makes every file this process writes distinct.
    static std::atomic<unsigned> s_sequence{0};
    std::string filename = "/tmp/orcamcp_render_" +
                          std::to_string(std::time(nullptr)) + "_" +
                          std::to_string(s_sequence.fetch_add(1)) + "_" +
                          std::to_string(view_index) + (use_png ? ".png" : ".jpg");

    // Save as JPEG
    image.SaveFile(filename, use_png ? wxBITMAP_TYPE_PNG : wxBITMAP_TYPE_JPEG);  // PNG keeps 1 px overlays crisp and alpha intact

    return filename;
}

// The numbers that make a picture checkable without looking at it. `frame` says which coordinates
// everything is in; `plate_origin` lets an agent convert plate-local numbers itself;
// `objects_in_frame` names what is visible and where in the image; `uniform_image` says the
// picture shows nothing, and `hint` says why -- the case that cost fifteen blind renders once.
static void append_render_report(nlohmann::json& entry, const RenderReport& report, const OrcaMCP::CameraFrame& camera, int plate_index)
{
    const BoundingBoxf3& plate = report.plate_box;
    entry["frame"]        = "bed_mm";
    entry["plate_origin"] = {plate.min.x(), plate.min.y()};

    nlohmann::json in_frame = nlohmann::json::array();
    for (const RenderedVolume& v : report.drawn) {
        const OrcaMCP::ScreenBBox sb = OrcaMCP::screen_bbox_of(camera, v.world_bbox);
        if (!sb.visible)
            continue;
        in_frame.push_back({{"object_index", v.object_index},
                            {"name", v.name},
                            {"screen_bbox", {std::round(sb.x0), std::round(sb.y0), std::round(sb.x1), std::round(sb.y1)}},
                            {"clipped", sb.clipped}});
    }
    entry["objects_in_frame"] = in_frame;
    entry["uniform_image"]    = report.uniform_image;
    if (report.uniform_image) {
        char buf[256];
        if (report.drawn.empty())
            std::snprintf(buf, sizeof(buf), "plate %d has no printable volumes; nothing to draw", plate_index);
        else
            std::snprintf(buf, sizeof(buf),
                          "%zu volume(s) on plate %d but none inside this view; plate %d spans x [%.0f, %.0f] y [%.0f, %.0f] bed mm -- aim the camera there or use a preset",
                          report.drawn.size(), plate_index, plate_index, plate.min.x(), plate.max.x(), plate.min.y(), plate.max.y());
        entry["hint"] = buf;
    }
}

// The plate footprint, as tall as its tallest object (at least 10 mm, so an empty plate still frames).
static BoundingBoxf3 plate_contents_box(PartPlate& plate, const BoundingBoxf3& plate_box)
{
    double top = plate_box.min.z() + 10.0;
    for (const ModelObject* mo : plate.get_objects_on_this_plate())
        for (size_t i = 0; i < mo->instances.size(); ++i)
            top = std::max(top, mo->instance_bounding_box(i).max.z());
    return BoundingBoxf3(plate_box.min, Vec3d(plate_box.max.x(), plate_box.max.y(), top));
}

// [x, y, z] or {x, y, z}, bed mm.
static Vec3d read_vec3(const nlohmann::json& v, const char* what)
{
    if (v.is_array() && v.size() >= 3)
        return Vec3d(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
    if (v.is_object())
        return Vec3d(v.value("x", 0.0), v.value("y", 0.0), v.value("z", 0.0));
    throw std::runtime_error(std::string(what) + " must be [x, y, z]");
}

nlohmann::json OrcaMCPPlateUtils::RenderPlateView(const nlohmann::json& params) {
    nlohmann::json payload = params.value("payload", nlohmann::json::object());
    if (payload.is_null() || payload.value("plate_index", -1) == -1) {
        BOOST_LOG_TRIVIAL(error) << "RenderPlateView: missing plate_index";
        throw std::runtime_error("Missing required parameter plate_index");
    }
    const int         plate_index  = payload.value("plate_index", -1);
    const bool        save_to_file = payload.value("save_to_file", false);
    const int         resolution   = std::clamp(payload.value("resolution", 512), 32, 2048);
    // Files default to PNG; inline base64 defaults to JPEG, where size matters more than crispness.
    const std::string image_format = payload.value("image_format", std::string(save_to_file ? "png" : "jpeg"));
    if (image_format != "png" && image_format != "jpeg")
        throw std::runtime_error("image_format must be \"png\" or \"jpeg\"");
    const bool use_png = image_format == "png";
    // Overlays default on (true/missing); false disables all; an object picks per overlay.
    const OrcaMCP::OverlayOptions overlay_options = OrcaMCP::overlay_options_from_json(payload.value("overlays", nlohmann::json(true)));

    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_index);
    if (plate == nullptr)
        throw std::runtime_error("plate_index out of range");
    const BoundingBoxf3              plate_box      = plate->get_plate_box();
    const std::vector<BoundingBoxf3> excluded_areas = plate->get_exclude_areas();

    // The first-layer plan is a different picture altogether: top-down, drawn on the CPU from the
    // sliced first layer (or footprints when unsliced), with brim, support and the wipe tower.
    const std::string layer_view = payload.value("layer_view", std::string());
    if (!layer_view.empty()) {
        if (layer_view != "first_layer")
            throw std::runtime_error("layer_view must be \"first_layer\"");
        const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
        const OrcaMCP::FirstLayerPlan plan    = OrcaMCP::collect_first_layer(*plate, full_config);
        const OrcaMCP::PlanMapping    mapping = OrcaMCP::plan_mapping(plate_box, resolution);
        const OrcaMCP::CameraFrame    camera  = OrcaMCP::plan_camera(mapping);
        wxImage image = OrcaMCP::draw_first_layer_plan(plan, mapping, plate_box, excluded_areas, overlay_options);

        nlohmann::json entry;
        entry["layer_view"]   = "first_layer";
        entry["source"]       = plan.source;
        entry["frame"]        = "bed_mm";
        entry["plate_origin"] = {plate_box.min.x(), plate_box.min.y()};
        entry["camera"]       = OrcaMCP::camera_frame_to_json(camera);
        entry["camera"]["type"] = "orthographic";
        entry["camera"]["pixel_origin"] = "top_left";
        entry["camera"]["mm_per_pixel"] = 1.0 / mapping.scale;
        nlohmann::json objects = nlohmann::json::array();
        for (const OrcaMCP::PlanObject& o : plan.objects) {
            if (o.body.empty()) continue;
            const BoundingBox   bb = get_extents(o.body);
            const BoundingBoxf3 mm(Vec3d(unscale<double>(bb.min.x()), unscale<double>(bb.min.y()), 0.), Vec3d(unscale<double>(bb.max.x()), unscale<double>(bb.max.y()), 0.));
            const OrcaMCP::ScreenBBox sb = OrcaMCP::screen_bbox_of(camera, mm);
            objects.push_back({{"object_index", o.object_index}, {"name", o.name},
                               {"screen_bbox", {std::round(sb.x0), std::round(sb.y0), std::round(sb.x1), std::round(sb.y1)}},
                               {"has_brim", !o.brim.empty()}});
        }
        entry["objects_in_frame"] = objects;
        entry["support_present"]  = !plan.support.empty();
        entry["wipe_tower_present"] = plan.wipe_tower.has_value();
        entry["overlays"] = OrcaMCP::overlay_options_to_json(overlay_options);
        if (save_to_file) entry["file_path"] = save_image_to_file(image, 0, use_png);
        else              entry["base64"]    = encode_image_to_base64(image, use_png);
        return nlohmann::json::array({entry});
    }

    // No views: a contact sheet of the three presets an agent reaches for first, fitted to the plate.
    nlohmann::json views         = payload.value("views", nlohmann::json());
    const bool     contact_sheet = !views.is_array();
    if (contact_sheet)
        views = nlohmann::json::array({{{"preset", "iso"}}, {{"preset", "top"}}, {{"preset", "front"}}});
    if (views.empty())
        throw std::runtime_error("views must not be empty");

    struct Rendered { wxImage image; nlohmann::json entry; };
    std::vector<Rendered> rendered;
    int view_index = 0;
    for (const auto& view : views) {
        if (!view.is_object())
            throw std::runtime_error("each view must be an object");

        // What the camera frames: the plate, or one object (a closer camera beats more pixels).
        // "plate" is the plate's footprint at the height of what is on it -- the full build volume
        // would push the parts into the bottom third of every picture.
        BoundingBoxf3  fit_box  = plate_contents_box(*plate, plate_box);
        nlohmann::json fit_json = "plate";
        if (view.contains("fit") && view["fit"].is_object() && view["fit"].contains("object_index")) {
            const int              oi   = view["fit"]["object_index"].get<int>();
            const ModelObjectPtrs& objs = wxGetApp().model().objects;
            if (oi < 0 || size_t(oi) >= objs.size())
                throw std::runtime_error("fit.object_index out of range");
            fit_box  = objs[oi]->instance_bounding_box(0);
            fit_json = view["fit"];
        }

        Vec3d       camera_position, target;
        std::string preset_name;
        std::string input_frame = view.value("frame", std::string("bed_mm"));
        if (view.contains("preset")) {
            OrcaMCP::CameraPreset preset;
            preset_name = view["preset"].get<std::string>();
            if (!OrcaMCP::camera_preset_from_string(preset_name, preset))
                throw std::runtime_error("unknown preset '" + preset_name + "'; use iso, top, front, back, left, right or low");
            OrcaMCP::preset_camera(preset, fit_box, camera_position, target);
            input_frame = "bed_mm";
        } else {
            if (!view.contains("camera_position") || !view.contains("target"))
                throw std::runtime_error("each view needs a preset, or camera_position and target");
            camera_position = read_vec3(view["camera_position"], "camera_position");
            target          = read_vec3(view["target"], "target");
            if (input_frame == "plate_local") {
                // Relative to this plate's front-left corner; converted so the renderer sees bed mm.
                const Vec3d origin(plate_box.min.x(), plate_box.min.y(), 0.);
                camera_position += origin;
                target += origin;
            } else if (input_frame != "bed_mm") {
                throw std::runtime_error("frame must be \"bed_mm\" or \"plate_local\"");
            }
        }

        ThumbnailData data;
        data.set(resolution, resolution);
        RenderCameraInfo cam;
        RenderReport     report;
        RenderOptions options;
        if (!preset_name.empty() || !fit_json.is_string())
            options.zoom_box = fit_box;  // presets and object fits frame exactly what they were asked to
        RenderThumbnail(data, camera_position, target, plate_index, &cam, options, &report);

        // Overlays go on the finished pixels, projected through the very camera that drew them.
        wxImage image = OrcaMCP::thumbnail_to_wximage(data);
        std::vector<OrcaMCP::OverlayLabel> labels;
        for (const RenderedVolume& v : report.drawn)
            if (!v.wipe_tower)
                labels.push_back({std::to_string(v.object_index), v.world_bbox.center(), v.color});
        OrcaMCP::draw_overlays(image, cam.frame, report.plate_box, excluded_areas, labels, overlay_options);

        // Everything pick_facet needs to turn a pixel of this image back into a ray. The three
        // required keys are written by the same function pick_facet parses with, so the two halves
        // cannot disagree about names or row order; the rest is for the reader. Pixel row 0 is the
        // top of the image and unproject_pixel_to_ray assumes that.
        nlohmann::json camera_json     = OrcaMCP::camera_frame_to_json(cam.frame);
        camera_json["type"]            = cam.perspective ? "perspective" : "orthographic";
        camera_json["pixel_origin"]    = "top_left";
        camera_json["camera_position"] = {camera_position.x(), camera_position.y(), camera_position.z()};  // bed mm, after any conversion
        camera_json["target"]          = {target.x(), target.y(), target.z()};
        camera_json["preset"]          = preset_name.empty() ? nlohmann::json(nullptr) : nlohmann::json(preset_name);
        camera_json["fit"]             = fit_json;
        camera_json["input_frame"]     = input_frame;

        nlohmann::json entry;
        entry["camera"] = camera_json;
        append_render_report(entry, report, cam.frame, plate_index);
        entry["overlays"] = OrcaMCP::overlay_options_to_json(overlay_options);
        rendered.push_back({std::move(image), std::move(entry)});
        ++view_index;
    }

    nlohmann::json result = nlohmann::json::array();
    if (!contact_sheet) {
        int idx = 0;
        for (Rendered& r : rendered) {
            if (save_to_file) r.entry["file_path"] = save_image_to_file(r.image, idx, use_png);
            else              r.entry["base64"]    = encode_image_to_base64(r.image, use_png);
            result.push_back(std::move(r.entry));
            ++idx;
        }
        return result;
    }

    // One image, the views side by side, each column's metadata under "views".
    wxImage sheet(resolution * int(rendered.size()), resolution);
    sheet.InitAlpha();
    nlohmann::json columns = nlohmann::json::array();
    for (size_t i = 0; i < rendered.size(); ++i) {
        sheet.Paste(rendered[i].image, int(i) * resolution, 0);
        rendered[i].entry["column"] = int(i);
        rendered[i].entry["x_offset"] = int(i) * resolution;  // add to a pixel x before handing it to pick_facet
        columns.push_back(std::move(rendered[i].entry));
    }
    nlohmann::json entry;
    entry["contact_sheet"] = true;
    entry["columns"]       = int(rendered.size());
    entry["views"]         = std::move(columns);
    if (save_to_file) entry["file_path"] = save_image_to_file(sheet, 0, use_png);
    else              entry["base64"]    = encode_image_to_base64(sheet, use_png);
    result.push_back(std::move(entry));
    return result;
}

// Offscreen render target for RenderThumbnail.
//
// The renderer used to draw into whatever framebuffer happened to be current on the GUI thread and
// glReadPixels it straight back. That is the wxGLCanvas's default framebuffer, and what it holds
// after a draw depends on state the tool does not control: with the Preview tab in front, the 3D
// canvas is hidden and every read came back as the clear colour -- a solid black image, for every
// plate and every camera, with nothing logged. Orca's own plate thumbnails never had this problem
// because GLCanvas3D::render_thumbnail_framebuffer draws into a framebuffer object it owns. This
// does the same, in the same non-multisampled shape, and puts the previous binding and viewport
// back so the on-screen canvas is untouched. If the framebuffer cannot be completed, or the driver
// only offers the EXT flavour, `ok` stays false and the caller draws exactly as before.
struct OffscreenRenderTarget
{
    GLuint fbo = 0, color = 0, depth = 0;
    GLint  prev_fbo = 0;
    GLint  prev_viewport[4] = {0, 0, 0, 0};
    bool   ok = false;

    OffscreenRenderTarget(unsigned int w, unsigned int h)
    {
        if (OpenGLManager::get_framebuffers_type() != OpenGLManager::EFramebufferType::Arb)
            return;
        glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo));
        glsafe(::glGetIntegerv(GL_VIEWPORT, prev_viewport));

        glsafe(::glGenFramebuffers(1, &fbo));
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));

        glsafe(::glGenTextures(1, &color));
        glsafe(::glBindTexture(GL_TEXTURE_2D, color));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0));

        glsafe(::glGenRenderbuffers(1, &depth));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, depth));
        glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, w, h));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth));

        const GLenum draw_bufs[] = {GL_COLOR_ATTACHMENT0};
        glsafe(::glDrawBuffers(1, draw_bufs));

        ok = ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!ok) {
            BOOST_LOG_TRIVIAL(warning) << "RenderThumbnail: offscreen framebuffer incomplete, drawing to the current framebuffer instead";
            release();
            return;
        }
        glsafe(::glViewport(0, 0, (GLsizei) w, (GLsizei) h));
    }

    ~OffscreenRenderTarget() { release(); }

    void release()
    {
        if (fbo != 0) {
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo));
            glsafe(::glDeleteFramebuffers(1, &fbo));
            fbo = 0;
            glsafe(::glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]));
        }
        if (color != 0) { glsafe(::glDeleteTextures(1, &color)); color = 0; }
        if (depth != 0) { glsafe(::glDeleteRenderbuffers(1, &depth)); depth = 0; }
    }
};

void OrcaMCPPlateUtils::RenderThumbnail(ThumbnailData& thumbnail_data,
    const Vec3d& camera_position, const Vec3d& target, int plate_index,
    RenderCameraInfo* out_camera)
{
    RenderThumbnail(thumbnail_data, camera_position, target, plate_index, out_camera, RenderOptions{}, nullptr);
}

void OrcaMCPPlateUtils::RenderThumbnail(ThumbnailData& thumbnail_data,
    const Vec3d& camera_position, const Vec3d& target, int plate_index,
    RenderCameraInfo* out_camera, const RenderOptions& options, RenderReport* report)
{
    using OrcaMCP::object_palette_color;
    using OrcaMCP::wipe_tower_color;
    using OrcaMCP::is_uniform_rgba;
    const Camera::EType camera_type = Camera::EType::Perspective;  // Fixed camera type
    const ThumbnailsParams thumbnail_params = { {}, false, true, true, true, 0};  // Fixed params

    GLShaderProgram* shader = wxGetApp().get_shader("thumbnail");
    if (shader == nullptr) {
        BOOST_LOG_TRIVIAL(info) << "RenderThumbnail: shader is null, returning directly";
        return;
    }

    ModelObjectPtrs& model_objects = GUI::wxGetApp().model().objects;
    std::vector<ColorRGBA> extruder_colors = wxGetApp().plater()->get_extruders_colors();
    auto canvas3D = wxGetApp().plater()->canvas3D();
    const GLVolumeCollection& volumes = canvas3D->get_volumes();
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_index);

    bool ban_light = false;
    static ColorRGBA curr_color;

    // Calculate visible volumes
    GLVolumePtrs visible_volumes;
    int plate_idx = thumbnail_params.plate_id;
    BoundingBoxf3 plate_build_volume = plate->get_plate_box();
    plate_build_volume.min -= Vec3d(1,1,1) * Slic3r::BuildVolume::SceneEpsilon;
    plate_build_volume.max += Vec3d(1,1,1) * Slic3r::BuildVolume::SceneEpsilon;

    auto is_visible = [plate_idx, plate_build_volume](const GLVolume& v) {
        bool ret = v.printable;
        if (plate_idx >= 0) {
            BoundingBoxf3 plate_bbox = plate_build_volume;
            plate_bbox.min(2) = -1e10;
            const BoundingBoxf3& volume_bbox = v.transformed_convex_hull_bounding_box();
            ret &= plate_bbox.contains(volume_bbox) && (volume_bbox.max(2) > 0);
        } else {
            ret &= (!v.shader_outside_printer_detection_enabled || !v.is_outside);
        }
        return ret;
    };

    // The prime tower is drawn, not skipped. It is printed plastic occupying bed area, and leaving
    // it out of the picture is the same blindness the structured report had: a plan view used to
    // check a layout showed a clear band where the tower was standing. Each plate's tower is a
    // separate volume (obj_idx 1000 + plate_id) and is_visible's containment test against this
    // plate's build volume is what keeps the other plates' towers out.
    for (const GLVolume* vol : volumes.volumes) {
        if (!vol->is_modifier && (!thumbnail_params.parts_only || vol->composite_id.volume_id >= 0)) {
            if (is_visible(*vol)) {
                visible_volumes.emplace_back(const_cast<GLVolume*>(vol));
            }
        }
    }

    // Calculate volumes bounding box
    BoundingBoxf3 volumes_box;
    volumes_box.min.z() = volumes_box.max.z() = 0;
    if (!visible_volumes.empty()) {
        for (const GLVolume* vol : visible_volumes) {
            volumes_box.merge(vol->transformed_bounding_box());
        }
        // Add padding (20% on each side for comfortable framing)
        Vec3d size = volumes_box.size();
        Vec3d padding = size * 0.20;
        volumes_box.min -= padding;
        volumes_box.max += padding;
        volumes_box.min.z() = -Slic3r::BuildVolume::SceneEpsilon;
    }

    // Draw into our own framebuffer, not the canvas's (see OffscreenRenderTarget). It lives until the
    // end of this function, so glReadPixels below reads from it and the canvas gets its state back.
    OffscreenRenderTarget offscreen(thumbnail_data.width, thumbnail_data.height);

    // Setup camera
    Camera camera;
    camera.set_type(camera_type);
    camera.set_scene_box(plate_build_volume);
    camera.set_viewport(0, 0, thumbnail_data.width, thumbnail_data.height);
    camera.apply_viewport();

    // Zoom to objects if present, otherwise fall back to plate
    BoundingBoxf3 zoom_box = options.zoom_box.has_value() ? *options.zoom_box
                           : (!visible_volumes.empty() && volumes_box.defined) ? volumes_box : plate_build_volume;
    zoom_box.min.z() = zoom_box.max.z() = 0.0;
    camera.zoom_to_box(zoom_box, 1.0);
    // Not a plain Vec3d::UnitZ(): look_at builds its basis from up.cross(view_direction), and for a
    // camera directly above its target that cross product is zero. Eigen's normalized() hands a
    // zero vector back unchanged rather than failing, so the plan view an agent asks for first used
    // to render a perfectly ordinary image alongside a view matrix whose 3x3 basis was all zeros --
    // which pick_facet then could not invert. stable_camera_up falls back to +Y for exactly that
    // case and returns +Z for every other view, so no existing render changes.
    camera.look_at(camera_position, target, OrcaMCP::stable_camera_up(camera_position, target));

    const Transform3d& view_matrix = camera.get_view_matrix();
    camera.apply_projection(plate_build_volume);
    const Transform3d& projection_matrix = camera.get_projection_matrix();

    // Copied out here, not returned by reference: `camera` is a local and both matrices above are
    // references into it. This is also the only point at which the projection is final.
    if (out_camera != nullptr) {
        out_camera->frame.view       = view_matrix.matrix();
        out_camera->frame.projection = projection_matrix.matrix();
        out_camera->frame.viewport   = camera.get_viewport();
        out_camera->perspective      = camera_type == Camera::EType::Perspective;
    }

    // Clear background
    glsafe(::glClearColor(options.background.r(), options.background.g(), options.background.b(), options.background.a()));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    if (ban_light) {
        glsafe(::glDisable(GL_BLEND));
    }

    // Note: render_grid is private in upstream OrcaSlicer, skipping grid rendering
    // The thumbnail will render without the plate grid background

    // Render volumes
    shader->start_using();
    shader->set_uniform("emission_factor", options.emission);
    shader->set_uniform("ban_light", false);

    for (GLVolume* vol : visible_volumes) {
        // GLWipeTowerVolume keeps its per-filament colours in members this file cannot reach, and
        // its own render() is not the one called here -- simple_render draws the plain shell mesh.
        // A fixed light grey reads unmistakably as "the tower" next to the objects' filament
        // colours, which is what a caller checking a layout needs it to do.
        // Palette by object index (see RenderOptions): the question a picture answers is "which
        // object is that", and filament colours often coincide.
        const int object_index = vol->composite_id.object_id;
        curr_color = vol->is_wipe_tower ? wipe_tower_color()
                   : (options.palette_colors ? object_palette_color(object_index) : vol->color);
        if (report != nullptr) {
            RenderedVolume drawn;
            drawn.object_index = vol->is_wipe_tower ? -1 : object_index;
            drawn.name         = vol->is_wipe_tower ? std::string("wipe tower")
                               : (object_index >= 0 && size_t(object_index) < model_objects.size() ? model_objects[object_index]->name : std::string());
            drawn.world_bbox   = vol->transformed_bounding_box();
            drawn.color        = curr_color;
            drawn.wipe_tower   = vol->is_wipe_tower;
            report->drawn.push_back(std::move(drawn));
        }
        ColorRGBA new_color = adjust_color_for_rendering(curr_color);
        vol->model.set_color(new_color);

        const bool is_active = vol->is_active;
        vol->is_active = true;

        const Transform3d model_matrix = vol->world_matrix();
        shader->set_uniform("volume_world_matrix", model_matrix);
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        shader->set_uniform("projection_matrix", projection_matrix);

        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) *
            model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);

        vol->simple_render(shader, model_objects, extruder_colors, false);
        vol->is_active = is_active;
    }

    shader->stop_using();
    glsafe(::glDisable(GL_DEPTH_TEST));

    // Read pixels
    glsafe(::glReadPixels(0, 0, thumbnail_data.width, thumbnail_data.height, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));
    if (report != nullptr) {
        report->uniform_image = is_uniform_rgba(thumbnail_data.pixels, thumbnail_data.width, thumbnail_data.height);
        report->plate_box     = plate->get_plate_box();
    }
    BOOST_LOG_TRIVIAL(info) << "RenderThumbnail: read " << thumbnail_data.width << "x" << thumbnail_data.height
                            << " from " << (offscreen.ok ? "offscreen framebuffer" : "current framebuffer")
                            << ", " << visible_volumes.size() << " visible volume(s)";

    // Debug output
    // std::string file_name = "zzh_" +
    //     std::to_string(int(camera_position.x()*100)) + "_" +
    //     std::to_string(int(camera_position.y()*100)) + "_" +
    //     std::to_string(int(camera_position.z()*100));
    // z_debug_output_thumbnail(thumbnail_data, file_name);

    BOOST_LOG_TRIVIAL(info) << "RenderThumbnail: finished";
}


ObjectFootprint OrcaMCPPlateUtils::GetObjectFootprint(const ModelObject& object, const DynamicPrintConfig& print_cfg)
{
    ObjectFootprint out;
    out.brim_type = object_or_global_enum(object, print_cfg, "brim_type", "no_brim");
    out.brim      = OrcaMCP::object_brim_extent(out.brim_type,
                                                object_or_global_float(object, print_cfg, "brim_width", 0.0),
                                                object_or_global_float(object, print_cfg, "brim_object_gap", 0.0));
    out.body = OrcaMCP::footprint_of(OrcaMCP::object_world_box(object));
    out.rect = OrcaMCP::expand_footprint(out.body, out.brim.extent_mm);
    return out;
}

PrimeTowerState OrcaMCPPlateUtils::GetPrimeTowerState(int plate_index, const DynamicPrintConfig& full_config)
{
    PrimeTowerState state;

    Plater*        plater = wxGetApp().plater();
    PartPlateList& ppl    = plater->get_partplate_list();
    if (plate_index < 0 || plate_index >= ppl.get_plate_count()) return state;

    PartPlate*    plate = ppl.get_plate(plate_index);
    PresetBundle* pb    = wxGetApp().preset_bundle;

    const DynamicPrintConfig& print_cfg = pb->prints.get_edited_preset().config;
    const DynamicPrintConfig& proj_cfg  = pb->project_config;

    state.plate_origin = plate->get_origin();
    state.plate_size   = plate->get_size();

    // The same conditions GLCanvas3D::reload_scene decides by when it builds (or does not build) a
    // wipe-tower volume for a plate, so "no tower here" means the same thing in this report as in
    // the 3D view. Mirrored into a pure predicate rather than re-derived; see OrcaMCPPlateOccupancy.
    OrcaMCP::PrimeTowerConditions conditions;
    conditions.is_fff = plater->printer_technology() == ptFFF;
    if (const auto* opt = print_cfg.option<ConfigOptionBool>("enable_prime_tower"))
        conditions.enable_prime_tower = opt->value;
    if (const auto* opt = print_cfg.option<ConfigOptionEnum<TimelapseType>>("timelapse_type"))
        conditions.timelapse_smooth = opt->value == TimelapseType::tlSmooth;
    if (const auto* opt = print_cfg.option<ConfigOptionBool>("enable_wrapping_detection"))
        conditions.wrapping_detection = opt->value;
    if (const auto* opt = proj_cfg.option<ConfigOptionStrings>("filament_colour"))
        conditions.project_filament_count = int(opt->values.size());
    conditions.gcode_only_mode = plater->only_gcode_mode() || plater->is_gcode_3mf();

    conditions.plate_has_objects         = !plate->get_objects_on_this_plate().empty();
    conditions.plate_filament_count      = int(plate->get_extruders(true).size());
    conditions.plate_printable_instances = plate->printable_instance_size();

    const PrintSequence plate_seq = plate->get_print_seq();
    if (plate_seq == PrintSequence::ByObject) {
        conditions.plate_sequential = true;
    } else if (plate_seq == PrintSequence::ByDefault) {
        const auto* global_seq = print_cfg.option<ConfigOptionEnum<PrintSequence>>("print_sequence");
        conditions.plate_sequential = global_seq != nullptr && global_seq->value == PrintSequence::ByObject;
    }

    state.verdict = OrcaMCP::prime_tower_verdict(conditions);
    state.printed = state.verdict == OrcaMCP::PrimeTowerVerdict::Printed;

    // The stored position, straight out of the project config, before any clamping. Reported even
    // when no tower is printed, because set_prime_tower_position writes exactly this pair and a
    // caller has to be able to read back what it wrote.
    if (const auto* x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x")) {
        if (size_t(plate_index) < x_opt->values.size()) state.stored_position.x() = x_opt->values[plate_index];
    }
    if (const auto* y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y")) {
        if (size_t(plate_index) < y_opt->values.size()) state.stored_position.y() = y_opt->values[plate_index];
    }

    // estimate_wipe_tower_polygon is the slicer's own answer for both the position (clamped onto the
    // bed) and the size, and its returned contour is the brim-inclusive rectangle. Taking the brim
    // from that contour rather than re-resolving prime_tower_brim_width keeps this from disagreeing
    // with the arranger about where the tower ends -- the automatic brim is negative in the config
    // and resolved from the tower's height, which is not a calculation worth having twice.
    Vec3d wt_pos = Vec3d::Zero();
    Vec3d wt_size = Vec3d::Zero();
    const arrangement::ArrangePolygon ap =
        plate->estimate_wipe_tower_polygon(full_config, plate_index, wt_pos, wt_size);

    state.size = wt_size;

    const BoundingBox contour_bb = ap.poly.contour.bounding_box();
    BoundingBoxf      local_footprint;
    local_footprint.min = Vec2d(unscale_(contour_bb.min.x()), unscale_(contour_bb.min.y()));
    local_footprint.max = Vec2d(unscale_(contour_bb.max.x()), unscale_(contour_bb.max.y()));
    local_footprint.defined = true;

    state.brim_width = std::max(0.0, wt_pos.x() - local_footprint.min.x());

    const Vec2d origin_2d(state.plate_origin.x(), state.plate_origin.y());
    state.corner    = Vec2d(wt_pos.x(), wt_pos.y()) + origin_2d;
    state.body      = OrcaMCP::prime_tower_footprint(state.corner, Vec2d(wt_size.x(), wt_size.y()), 0.0);
    state.footprint = translated(local_footprint, origin_2d);

    double raw_brim = 0.0;
    if (const auto* opt = full_config.option<ConfigOptionFloat>("prime_tower_brim_width")) raw_brim = opt->value;
    state.legal_range = OrcaMCP::prime_tower_position_range(state.plate_size.x(), state.plate_size.y(),
                                                            wt_size.x(), wt_size.y(),
                                                            WIPE_TOWER_MARGIN + raw_brim, state.brim_width);
    return state;
}

PrimeTowerState OrcaMCPPlateUtils::GetPrimeTowerState(int plate_index)
{
    return GetPrimeTowerState(plate_index, wxGetApp().preset_bundle->full_config());
}

nlohmann::json OrcaMCPPlateUtils::PrimeTowerJson(const PrimeTowerState& state)
{
    nlohmann::json j;
    j["printed"] = state.printed;
    j["reason"]  = OrcaMCP::prime_tower_verdict_token(state.verdict);
    j["reason_detail"] = OrcaMCP::prime_tower_verdict_explanation(state.verdict);
    j["frame"] = "plate_mm";
    j["stored_position"] = {{"x", state.stored_position.x()}, {"y", state.stored_position.y()},
                            {"frame", "plate_local_mm"}};

    if (!state.printed) return j;

    j["position"] = {{"x", state.corner.x()}, {"y", state.corner.y()}};
    j["position_is"] = "front_left_corner_of_tower_body";
    j["size"] = {{"x", state.size.x()}, {"y", state.size.y()}, {"z", state.size.z()}};
    j["brim_width_mm"] = state.brim_width;
    j["body"] = rect_to_json(state.body);
    j["footprint"] = rect_to_json(state.footprint);
    j["footprint_includes_brim"] = true;
    j["note"] = "footprint is the bed area the tower occupies, brim included; body is the tower "
                "alone. Both are in plate millimetres, the frame object bounding boxes use. Place "
                "parts clear of footprint, not of body.";
    return j;
}

nlohmann::json OrcaMCPPlateUtils::GetPlates(bool with_model_object_features) {
    nlohmann::json j = nlohmann::json::array();

    Plater* plater = wxGetApp().plater();  // Get plater instance
    const Model& model = plater->model();

    // Built once, not per plate: assembling the full config is the expensive half of reading the
    // prime tower's size, and every plate's tower is measured from the same one.
    const DynamicPrintConfig  full_config = wxGetApp().preset_bundle->full_config();
    const DynamicPrintConfig& print_cfg   = wxGetApp().preset_bundle->prints.get_edited_preset().config;

    for (const auto& plate : plater->get_partplate_list().get_plate_list()) {
        nlohmann::json plate_info;
        plate_info["name"] = plate->get_plate_name();
        plate_info["index"] = plate->get_index();
        plate_info["bounding_box"] = bbox_to_json(plate->get_plate_box());

        // Everything standing on this plate, in one list and one frame: the model objects, the
        // prime tower, and the printer's own excluded bed areas. An agent looking for free space
        // reads this; reading model_objects alone is what left one placing parts around an obstacle
        // it could not see.
        nlohmann::json occupancy = nlohmann::json::array();

        // Loop through each ModelObject (now deduplicated)
        nlohmann::json objects_info = nlohmann::json::array();
        for (const auto& obj : plate->get_objects_on_this_plate()) {
            // Find the object's index in model.objects (used for transform operations)
            int object_index = -1;
            for (size_t i = 0; i < model.objects.size(); ++i) {
                if (model.objects[i] == obj) {
                    object_index = static_cast<int>(i);
                    break;
                }
            }
            // Identity, transform and bounding box, exactly as load_model's loaded_objects reports them.
            nlohmann::json object_info = OrcaMCP::model_object_summary_json(*obj, object_index);
            const Vec3d    size        = OrcaMCP::object_world_box(*obj).size();

            // The bounding box is the model; the brim is printed plastic beyond it. A neighbour
            // placed flush against the bounding box collides with the brim, so the printed extent
            // is reported alongside it rather than left for the caller to work out.
            const ObjectFootprint footprint = GetObjectFootprint(*obj, print_cfg);
            object_info["brim"] = {
                {"type", footprint.brim_type},
                {"extent_mm", footprint.brim.extent_mm},
                {"extent_upper_bound_mm", footprint.brim.upper_bound_mm},
                {"extent_is_exact", footprint.brim.exact}
            };
            object_info["printed_footprint"] = rect_to_json(footprint.rect);
            object_info["printed_footprint_includes_brim"] = footprint.brim.extent_mm > 0.0;

            // Variable Layer Height status
            object_info["vlh_enabled"] = !obj->layer_height_profile.empty();
            object_info["vlh_profile_points"] = obj->layer_height_profile.empty() ? 0 :
                static_cast<int>(obj->layer_height_profile.get().size() / 2);

            if (with_model_object_features) {
                object_info["features"] = GetModelObjectFeaturesJson(obj);
            }

            auto object_grid_config = &(obj->config);
            int extruder_id = -1;  // Default extruder ID
            auto extruder_id_ptr = static_cast<const ConfigOptionInt*>(object_grid_config->option("extruder"));
            if (extruder_id_ptr) {
                extruder_id = *extruder_id_ptr;
            }
            object_info["extruder_id"] = extruder_id;
            // extruder_id is the object's own setting, which a volume's own slot overrides
            // (ModelVolume::extruder_id). filaments_used is what the object prints with -- the
            // per-object half of the rule the plate applies for its prime tower -- and is the
            // field to read; the two agree only when filament_override_count is 0. Added after an
            // agent read extruder_id == 3 on 45 objects whose modifiers were all still on slot 1.
            object_info["filaments_used"]          = OrcaMCP::effective_object_filaments(*obj);
            object_info["filament_override_count"] = OrcaMCP::volume_filament_override_count(*obj);

            objects_info.push_back(object_info);

            occupancy.push_back({
                {"kind", "object"},
                {"name", obj->name},
                {"object_index", object_index},
                {"footprint", rect_to_json(footprint.rect)},
                {"includes_brim", footprint.brim.extent_mm > 0.0},
                {"footprint_is_exact", footprint.brim.exact},
                {"height_mm", size.z()}
            });
        }
        plate_info["model_objects"] = objects_info;

        // The prime tower. It is printed plastic standing on the bed exactly as the objects above
        // are, and until this was reported an agent enumerating the plate's occupants simply did
        // not know it was there.
        const PrimeTowerState tower = GetPrimeTowerState(plate->get_index(), full_config);
        plate_info["prime_tower"] = PrimeTowerJson(tower);
        if (tower.printed) {
            occupancy.push_back({
                {"kind", "prime_tower"},
                {"name", "Prime tower"},
                {"footprint", rect_to_json(tower.footprint)},
                {"includes_brim", tower.brim_width > 0.0},
                {"footprint_is_exact", true},
                {"height_mm", tower.size.z()}
            });
        }

        // Bed exclusion areas: not plastic, but bed an object may not stand on either, and just as
        // invisible to a caller reading only the object list. Empty on most printers; real on the
        // ones that cut filament in a corner of the bed.
        nlohmann::json excluded = nlohmann::json::array();
        for (const BoundingBoxf3& area : plate->get_exclude_areas()) {
            const BoundingBoxf rect = OrcaMCP::footprint_of(area);
            if (!rect.defined) continue;
            excluded.push_back(rect_to_json(rect));
            occupancy.push_back({
                {"kind", "excluded_area"},
                {"name", "Excluded bed area"},
                {"footprint", rect_to_json(rect)},
                {"includes_brim", false},
                {"footprint_is_exact", true}
            });
        }
        plate_info["excluded_areas"] = excluded;

        plate_info["occupancy"] = occupancy;
        plate_info["occupancy_frame"] = "plate_mm";

        j.push_back(plate_info);
    }

    return j;
}

nlohmann::json OrcaMCPPlateUtils::GetModelObjectFeaturesJson(const ModelObject* obj) {
    // Simplified: feature analysis not available in upstream OrcaSlicer
    // Return empty object - features would require additional orientation analysis code
    return nlohmann::json::object();
}

std::string sorted_volumes_hash_code(const Model& model) {
    nlohmann::json volumes = nlohmann::json::array();

    // Iterate through all objects and their volumes
    for (const ModelObject* obj : model.objects) {
        for (const ModelVolume* volume : obj->volumes) {
            const TriangleMeshStats& stats = volume->mesh().stats();
            nlohmann::json volume_info = {
                {"name", volume->name},
                {"stats", {
                    {"number_of_facets", stats.number_of_facets},
                    {"max", {stats.max[0], stats.max[1], stats.max[2]}},
                    {"min", {stats.min[0], stats.min[1], stats.min[2]}},
                    {"size", {stats.size[0], stats.size[1], stats.size[2]}},
                    {"volume", stats.volume},
                    {"number_of_parts", stats.number_of_parts}
                }}
            };
            volumes.push_back(volume_info);
        }
    }

    // Sort by name
    std::sort(volumes.begin(), volumes.end(),
        [](const nlohmann::json& a, const nlohmann::json& b) {
            return a["name"] < b["name"];
        });

    // Convert to compact string (no spaces)
    std::string json_str = volumes.dump(-1); // -1 means no indentation

    // Calculate MD5
    MD5_CTX md5Context;
    MD5_Init(&md5Context);
    MD5_Update(&md5Context, json_str.c_str(), json_str.length());
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &md5Context);

    // Convert MD5 to hex string
    std::stringstream md5_ss;
    for(int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        md5_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return md5_ss.str();
}

nlohmann::json OrcaMCPPlateUtils::GetCurrentProject(bool with_model_object_features) {
    Plater* plater = wxGetApp().plater();
    const Model& model = plater->model();  // Get model from plater

    std::string hash_code = sorted_volumes_hash_code(model);

    // Get bed dimensions from current plate
    auto plate = plater->get_partplate_list().get_curr_plate();
    BoundingBoxf3 bed_box = plate->get_plate_box();

    // Check if sequential (by object) printing is enabled
    bool sequential_print = false;
    const Print& print = plater->fff_print();
    if (print.config().print_sequence == PrintSequence::ByObject) {
        sequential_print = true;
    }

    nlohmann::json j;
    j["hash_code"] = hash_code;
    j["sequential_print_enabled"] = sequential_print;
    j["bed"] = {
        {"origin", "corner"},
        {"min_x", bed_box.min.x()},
        {"min_y", bed_box.min.y()},
        {"max_x", bed_box.max.x()},
        {"max_y", bed_box.max.y()},
        {"max_z", bed_box.max.z()}
    };
    j["plates"] = GetPlates(with_model_object_features);

    // Objects that belong to no plate at all. `plates` is walked plate by plate, so anything sitting
    // outside every one of them was simply invisible here -- which is where deleting a plate leaves
    // its objects: PartPlateList::delete_plate moves them to the unprintable area rather than
    // deleting them or re-homing them. An agent could not see them, could not arrange them, and had
    // no reason to suspect they were still in the model.
    nlohmann::json unplaced = nlohmann::json::array();
    PartPlateList& plate_list = plater->get_partplate_list();
    for (size_t i = 0; i < model.objects.size(); ++i) {
        if (plate_list.find_instance(int(i), 0) >= 0)
            continue;

        const ModelObject* object = model.objects[i];
        const BoundingBoxf3 bbox  = OrcaMCP::object_world_box(*object);
        unplaced.push_back(nlohmann::json{
            {"object_index", int(i)},
            {"id", std::to_string(object->id().id)},
            {"name", object->name},
            {"position", {{"x", bbox.center().x()}, {"y", bbox.center().y()}, {"z", bbox.center().z()}}},
            {"reason", "Not on any plate. Deleting a plate moves its objects here rather than "
                       "removing them; use delete_object, or move it onto a plate."}
        });
    }
    j["unplaced_objects"] = std::move(unplaced);

    return j;
}

void OrcaMCPPlateUtils::CleanupPreviews() {
    namespace fs = boost::filesystem;
    try {
        fs::path tmp_dir("/tmp");
        if (!fs::exists(tmp_dir)) return;

        for (fs::directory_iterator it(tmp_dir); it != fs::directory_iterator(); ++it) {
            if (fs::is_regular_file(*it)) {
                std::string filename = it->path().filename().string();
                if (filename.find("orcamcp_preview_") == 0 &&
                    filename.find(".jpg") != std::string::npos) {
                    fs::remove(*it);
                }
            }
        }
        BOOST_LOG_TRIVIAL(info) << "OrcaSlicer: Cleaned up old preview files";
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaSlicer: Failed to cleanup preview files: " << e.what();
    }
}

nlohmann::json OrcaMCPPlateUtils::CaptureTurntablePreview(int plate_index, int view_count, int resolution) {
    // Get plate and calculate object bounding box for adaptive camera
    Plater* plater = wxGetApp().plater();
    PartPlate* plate = plater->get_partplate_list().get_plate(plate_index);
    if (!plate) {
        return {{"error", "Invalid plate index"}};
    }

    // Calculate bounding box of objects on this plate
    BoundingBoxf3 objects_box;
    for (const auto& obj : plate->get_objects_on_this_plate()) {
        objects_box.merge(OrcaMCP::object_world_box(*obj));
    }

    // Determine target (center of objects) and camera distance
    Vec3d target;
    double radius;
    double height;

    if (objects_box.defined && objects_box.size().norm() > 0) {
        // Adaptive camera based on object bounding box
        Vec3d obj_center = objects_box.center();
        Vec3d obj_size = objects_box.size();

        // Target the center of objects
        target = Vec3d(obj_center.x(), obj_center.y(), obj_center.z() * 0.4);

        // Calculate camera distance based on object size
        // Use the largest horizontal dimension + height to determine framing
        double max_horizontal = std::max(obj_size.x(), obj_size.y());
        double diagonal = std::sqrt(max_horizontal * max_horizontal + obj_size.z() * obj_size.z());

        // Camera distance: ~2.5x the diagonal for good framing at 45° angle
        radius = diagonal * 2.5;
        // Ensure minimum distance for very small objects
        radius = std::max(radius, 80.0);

        // Height proportional to object height, maintaining ~40° elevation angle
        height = target.z() + radius * 0.7;
    } else {
        // Fallback to plate center if no objects
        BoundingBoxf3 plate_box = plate->get_plate_box();
        target = Vec3d(
            (plate_box.min.x() + plate_box.max.x()) / 2.0,
            (plate_box.min.y() + plate_box.max.y()) / 2.0,
            30.0
        );
        radius = 300.0;
        height = 250.0;
    }

    // Calculate angle step based on view count
    double angle_step = 360.0 / view_count;

    // View names for cardinal directions (camera position relative to plate)
    // 0° = camera at +X looking toward center = "Front" view
    // 90° = camera at +Y = "Right" view
    // 180° = camera at -X = "Back" view
    // 270° = camera at -Y = "Left" view
    std::vector<std::string> view_names_4 = {"Front", "Right", "Back", "Left"};
    std::vector<std::string> view_names_8 = {"Front", "Front-Right", "Right", "Back-Right",
                                              "Back", "Back-Left", "Left", "Front-Left"};

    // Render each view and store angles
    std::vector<ThumbnailData> thumbnails;
    std::vector<int> angles;
    std::vector<std::string> view_names;
    for (int i = 0; i < view_count; ++i) {
        double angle_deg = i * angle_step;
        angles.push_back(static_cast<int>(angle_deg));

        // Assign view name
        if (view_count == 4) {
            view_names.push_back(view_names_4[i % 4]);
        } else if (view_count == 8) {
            view_names.push_back(view_names_8[i % 8]);
        } else {
            view_names.push_back("");
        }

        double angle_rad = angle_deg * M_PI / 180.0;

        Vec3d camera_position(
            target.x() + radius * cos(angle_rad),
            target.y() + radius * sin(angle_rad),
            height
        );

        ThumbnailData data;
        data.set(resolution, resolution);
        RenderThumbnail(data, camera_position, target, plate_index);
        thumbnails.push_back(std::move(data));
    }

    // Combine thumbnails into grid
    // 4 views: 2x2, 8 views: 4x2
    int cols = (view_count <= 4) ? 2 : 4;
    int rows = (view_count + cols - 1) / cols;

    int grid_width = cols * resolution;
    int title_height = std::max(24, resolution / 8);  // Title bar height
    int grid_height = rows * resolution + title_height;

    wxImage grid_image(grid_width, grid_height);
    grid_image.SetRGB(wxRect(0, 0, grid_width, grid_height), 0, 0, 0);  // Black background

    for (int i = 0; i < view_count && i < (int)thumbnails.size(); ++i) {
        const ThumbnailData& thumb = thumbnails[i];
        int col = i % cols;
        int row = i / cols;
        int x_offset = col * resolution;
        int y_offset = title_height + row * resolution;  // Offset by title bar

        // Copy thumbnail pixels to grid
        for (unsigned int r = 0; r < thumb.height; ++r) {
            unsigned int rr = (thumb.height - 1 - r) * thumb.width;  // Flip Y
            for (unsigned int c = 0; c < thumb.width; ++c) {
                unsigned char* px = (unsigned char*)thumb.pixels.data() + 4 * (rr + c);
                grid_image.SetRGB(x_offset + c, y_offset + r, px[0], px[1], px[2]);
            }
        }
    }

    // Draw title and angle labels on the grid
    wxBitmap bitmap(grid_image);
    wxMemoryDC dc(bitmap);

    // Get plate name
    wxString plate_name = plate->get_plate_name();
    if (plate_name.IsEmpty()) {
        plate_name = wxString::Format("Plate %d", plate_index + 1);
    }

    // Draw title bar
    int title_font_size = std::max(12, title_height * 2 / 3);
    wxFont title_font(title_font_size, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    dc.SetFont(title_font);
    dc.SetTextForeground(*wxWHITE);
    dc.SetBackgroundMode(wxTRANSPARENT);

    // Center the title
    wxSize title_size = dc.GetTextExtent(plate_name);
    int title_x = (grid_width - title_size.GetWidth()) / 2;
    int title_y = (title_height - title_size.GetHeight()) / 2;
    dc.DrawText(plate_name, title_x, title_y);

    // Set font for view labels (angle + name)
    int font_size = std::max(10, resolution / 14);
    wxFont font(font_size, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    dc.SetFont(font);

    // Smaller font for cardinal direction labels on plate edges
    int edge_font_size = std::max(8, resolution / 20);
    wxFont edge_font(edge_font_size, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);

    // Cardinal directions: which edge label appears where based on camera angle
    // For camera at angle i*90°, the directions rotate
    // Index 0=near(bottom), 1=far(top), 2=left, 3=right
    std::vector<std::vector<std::string>> edge_labels_by_view = {
        {"Front", "Back", "Left", "Right"},   // 0° Front view
        {"Right", "Left", "Front", "Back"},   // 90° Right view
        {"Back", "Front", "Right", "Left"},   // 180° Back view
        {"Left", "Right", "Back", "Front"}    // 270° Left view
    };

    for (int i = 0; i < view_count && i < (int)angles.size(); ++i) {
        int col = i % cols;
        int row = i / cols;
        int x_offset = col * resolution;
        int y_offset = title_height + row * resolution;
        int padding = std::max(4, resolution / 32);

        // Draw view label (e.g., "0° Front View")
        dc.SetFont(font);
        wxString view_label;
        if (i < (int)view_names.size() && !view_names[i].empty()) {
            view_label = wxString::Format("%d", angles[i]) + wxString::FromUTF8("\xC2\xB0 ")
                       + wxString(view_names[i]) + " View";
        } else {
            view_label = wxString::Format("%d", angles[i]) + wxString::FromUTF8("\xC2\xB0");
        }
        dc.DrawText(view_label, x_offset + padding, y_offset + padding);

        // Draw cardinal direction labels on plate edges (only for 4-view mode)
        if (view_count == 4) {
            dc.SetFont(edge_font);
            dc.SetTextForeground(wxColour(200, 200, 200));  // Slightly dimmer

            const auto& edges = edge_labels_by_view[i % 4];
            wxSize text_size;

            // Near edge (bottom center)
            text_size = dc.GetTextExtent(edges[0]);
            dc.DrawText(edges[0], x_offset + (resolution - text_size.GetWidth()) / 2,
                       y_offset + resolution - text_size.GetHeight() - padding);

            // Far edge (top center) - skip, overlaps with view label
            // text_size = dc.GetTextExtent(edges[1]);
            // dc.DrawText(edges[1], x_offset + (resolution - text_size.GetWidth()) / 2,
            //            y_offset + padding + font_size + 4);

            // Left edge (middle left, rotated text not easy, so just put it)
            text_size = dc.GetTextExtent(edges[2]);
            dc.DrawText(edges[2], x_offset + padding,
                       y_offset + (resolution - text_size.GetHeight()) / 2);

            // Right edge (middle right)
            text_size = dc.GetTextExtent(edges[3]);
            dc.DrawText(edges[3], x_offset + resolution - text_size.GetWidth() - padding,
                       y_offset + (resolution - text_size.GetHeight()) / 2);

            dc.SetTextForeground(*wxWHITE);  // Reset color
        }
    }

    dc.SelectObject(wxNullBitmap);

    // Convert back to image and save
    wxImage final_image = bitmap.ConvertToImage();
    std::string filename = "/tmp/orcamcp_preview_" +
                          std::to_string(std::time(nullptr)) + ".jpg";
    final_image.SaveFile(filename, wxBITMAP_TYPE_JPEG);

    return {
        {"preview_path", filename},
        {"view_count", view_count},
        {"resolution", resolution},
        {"grid_size", {{"width", grid_width}, {"height", grid_height}}}
    };
}

}} // namespace Slic3r::GUI
