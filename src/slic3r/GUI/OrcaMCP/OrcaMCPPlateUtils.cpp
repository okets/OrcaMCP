#include "OrcaMCPPlateUtils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include <wx/mstream.h>
#include <wx/dcmemory.h>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <openssl/md5.h>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cmath>

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

static std::string encode_thumbnail_to_base64(const ThumbnailData& thumbnail_data, bool use_png = true) {
    // Create wxImage from thumbnail data
    wxImage image(thumbnail_data.width, thumbnail_data.height);
    image.InitAlpha();

    for (unsigned int r = 0; r < thumbnail_data.height; ++r) {
        unsigned int rr = (thumbnail_data.height - 1 - r) * thumbnail_data.width;
        for (unsigned int c = 0; c < thumbnail_data.width; ++c) {
            unsigned char* px = (unsigned char*)thumbnail_data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }

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

static std::string save_thumbnail_to_file(const ThumbnailData& thumbnail_data, int view_index) {
    // Create wxImage from thumbnail data
    wxImage image(thumbnail_data.width, thumbnail_data.height);
    image.InitAlpha();

    for (unsigned int r = 0; r < thumbnail_data.height; ++r) {
        unsigned int rr = (thumbnail_data.height - 1 - r) * thumbnail_data.width;
        for (unsigned int c = 0; c < thumbnail_data.width; ++c) {
            unsigned char* px = (unsigned char*)thumbnail_data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }

    // Generate unique filename in temp directory
    std::string filename = "/tmp/orcamcp_render_" +
                          std::to_string(std::time(nullptr)) + "_" +
                          std::to_string(view_index) + ".jpg";

    // Save as JPEG
    image.SaveFile(filename, wxBITMAP_TYPE_JPEG);

    return filename;
}

nlohmann::json OrcaMCPPlateUtils::RenderPlateView(const nlohmann::json& params) {
    nlohmann::json payload = params.value("payload", nlohmann::json::object());
    if (payload.is_null() ||
        payload.value("plate_index", -1) == -1 ||
        !payload.contains("views")) {
        BOOST_LOG_TRIVIAL(error) << "RenderPlateView: missing required parameters";
        throw std::runtime_error("Missing required parameters");
    }

    int plate_index = payload.value("plate_index", -1);
    bool save_to_file = payload.value("save_to_file", false);
    int resolution = payload.value("resolution", 512);
    auto views = payload["views"];

    if (!views.is_array()) {
        BOOST_LOG_TRIVIAL(error) << "RenderPlateView: views must be an array";
        throw std::runtime_error("Views must be an array");
    }

    nlohmann::json result = nlohmann::json::array();
    int view_index = 0;

    // Generate thumbnails for each view
    for (const auto& view : views) {
        if (!view.contains("camera_position") || !view.contains("target")) {
            BOOST_LOG_TRIVIAL(error) << "RenderPlateView: each view must contain camera_position and target";
            throw std::runtime_error("Invalid view format");
        }

        auto camera_pos_json = view["camera_position"];
        auto target_json = view["target"];

        // Support both array [x, y, z] and object {"x": x, "y": y, "z": z} formats
        Vec3d camera_position;
        if (camera_pos_json.is_array() && camera_pos_json.size() >= 3) {
            camera_position = Vec3d(
                camera_pos_json[0].get<double>(),
                camera_pos_json[1].get<double>(),
                camera_pos_json[2].get<double>()
            );
        } else {
            camera_position = Vec3d(
                camera_pos_json.value("x", 0.0),
                camera_pos_json.value("y", 0.0),
                camera_pos_json.value("z", 0.0)
            );
        }

        Vec3d target;
        if (target_json.is_array() && target_json.size() >= 3) {
            target = Vec3d(
                target_json[0].get<double>(),
                target_json[1].get<double>(),
                target_json[2].get<double>()
            );
        } else {
            target = Vec3d(
                target_json.value("x", 0.0),
                target_json.value("y", 0.0),
                target_json.value("z", 0.0)
            );
        }

        ThumbnailData data;
        data.set(resolution, resolution);
        RenderThumbnail(data, camera_position, target, plate_index);

        if (save_to_file) {
            // Save to file and return path
            std::string file_path = save_thumbnail_to_file(data, view_index);
            result.push_back({
                {"file_path", file_path}
            });
        } else {
            // Convert to base64-encoded image
            std::string base64_image = encode_thumbnail_to_base64(data, false);
            result.push_back({
                {"base64", base64_image}
            });
        }
        view_index++;
    }

    return result;
}

void OrcaMCPPlateUtils::RenderThumbnail(ThumbnailData& thumbnail_data,
    const Vec3d& camera_position, const Vec3d& target, int plate_index)
{
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

    for (const GLVolume* vol : volumes.volumes) {
        if (!vol->is_modifier && !vol->is_wipe_tower && (!thumbnail_params.parts_only || vol->composite_id.volume_id >= 0)) {
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
        // Add padding
        Vec3d size = volumes_box.size();
        volumes_box.min -= size * 0.01;
        volumes_box.max += size * 0.01;
        volumes_box.min.z() = -Slic3r::BuildVolume::SceneEpsilon;
    }

    // Setup camera
    Camera camera;
    camera.set_type(camera_type);
    camera.set_scene_box(plate_build_volume);
    camera.set_viewport(0, 0, thumbnail_data.width, thumbnail_data.height);
    camera.apply_viewport();

    plate_build_volume.min.z() = plate_build_volume.max.z() = 0.0;
    camera.zoom_to_box(plate_build_volume, 1.0);
    camera.look_at(camera_position, target, Vec3d::UnitZ());

    const Transform3d& view_matrix = camera.get_view_matrix();
    camera.apply_projection(plate_build_volume);
    const Transform3d& projection_matrix = camera.get_projection_matrix();

    // Clear background
    glsafe(::glClearColor(0.f, 0.f, 0.f, 0.f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    if (ban_light) {
        glsafe(::glDisable(GL_BLEND));
    }

    // Note: render_grid is private in upstream OrcaSlicer, skipping grid rendering
    // The thumbnail will render without the plate grid background

    // Render volumes
    shader->start_using();
    shader->set_uniform("emission_factor", 0.1f);
    shader->set_uniform("ban_light", false);

    for (GLVolume* vol : visible_volumes) {
        curr_color = vol->color;
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

    // Debug output
    // std::string file_name = "zzh_" +
    //     std::to_string(int(camera_position.x()*100)) + "_" +
    //     std::to_string(int(camera_position.y()*100)) + "_" +
    //     std::to_string(int(camera_position.z()*100));
    // z_debug_output_thumbnail(thumbnail_data, file_name);

    BOOST_LOG_TRIVIAL(info) << "RenderThumbnail: finished";
}


nlohmann::json OrcaMCPPlateUtils::GetPlates(bool with_model_object_features) {
    nlohmann::json j = nlohmann::json::array();

    Plater* plater = wxGetApp().plater();  // Get plater instance
    const Model& model = plater->model();

    for (const auto& plate : plater->get_partplate_list().get_plate_list()) {
        nlohmann::json plate_info;
        plate_info["name"] = plate->get_plate_name();
        plate_info["index"] = plate->get_index();
        plate_info["bounding_box"] = bbox_to_json(plate->get_plate_box());

        // Loop through each ModelObject (now deduplicated)
        nlohmann::json objects_info = nlohmann::json::array();
        for (const auto& obj : plate->get_objects_on_this_plate()) {
            nlohmann::json object_info;
            object_info["id"] = std::to_string(obj->id().id);
            object_info["name"] = obj->name;

            // Find the object's index in model.objects (used for transform operations)
            int object_index = -1;
            for (size_t i = 0; i < model.objects.size(); ++i) {
                if (model.objects[i] == obj) {
                    object_index = static_cast<int>(i);
                    break;
                }
            }
            object_info["object_index"] = object_index;
            object_info["instance_count"] = static_cast<int>(obj->instances.size());

            // Get bounding box first (used for position and size)
            BoundingBoxf3 bbox = obj->bounding_box_approx();
            Vec3d center = bbox.center();
            Vec3d size = bbox.size();

            // Position uses bounding box center (accurate after transforms)
            object_info["position"] = {
                {"x", center.x()},
                {"y", center.y()},
                {"z", center.z()}
            };

            // Add transform info from first instance (primary instance)
            // Note: rotation_degrees reflects UI/initial rotation only - MCP rotations are applied to geometry
            if (!obj->instances.empty()) {
                auto* inst = obj->instances[0];
                Vec3d rotation = inst->get_rotation();
                Vec3d scale = inst->get_scaling_factor();

                object_info["rotation_degrees"] = {
                    {"x", Geometry::rad2deg(rotation.x())},
                    {"y", Geometry::rad2deg(rotation.y())},
                    {"z", Geometry::rad2deg(rotation.z())}
                };
                object_info["scale"] = {
                    {"x", scale.x()},
                    {"y", scale.y()},
                    {"z", scale.z()}
                };
            }
            object_info["bounding_box"] = {
                {"size_x", size.x()},
                {"size_y", size.y()},
                {"size_z", size.z()},
                {"min", {{"x", bbox.min.x()}, {"y", bbox.min.y()}, {"z", bbox.min.z()}}},
                {"max", {{"x", bbox.max.x()}, {"y", bbox.max.y()}, {"z", bbox.max.z()}}}
            };

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

            objects_info.push_back(object_info);
        }
        plate_info["model_objects"] = objects_info;

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
                if (filename.find("jusprin_preview_") == 0 &&
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
    // Get plate center for camera target
    Plater* plater = wxGetApp().plater();
    PartPlate* plate = plater->get_partplate_list().get_plate(plate_index);
    if (!plate) {
        return {{"error", "Invalid plate index"}};
    }

    BoundingBoxf3 plate_box = plate->get_plate_box();
    Vec3d plate_center(
        (plate_box.min.x() + plate_box.max.x()) / 2.0,
        (plate_box.min.y() + plate_box.max.y()) / 2.0,
        30.0  // Look at point slightly above bed
    );

    // Camera distance and height for 45° viewing angle
    double radius = 300.0;
    double height = 250.0;

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
            plate_center.x() + radius * cos(angle_rad),
            plate_center.y() + radius * sin(angle_rad),
            height
        );

        ThumbnailData data;
        data.set(resolution, resolution);
        RenderThumbnail(data, camera_position, plate_center, plate_index);
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
