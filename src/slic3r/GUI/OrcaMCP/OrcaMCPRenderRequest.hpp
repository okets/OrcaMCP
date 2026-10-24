#ifndef slic3r_OrcaMCPRenderRequest_hpp_
#define slic3r_OrcaMCPRenderRequest_hpp_

// render_plate_view's request, read up front and apart from the app: every option parsed and every
// refusal made before anything is looked up or drawn, so a malformed call is reported as that. No wx
// application, no GL, no scene: unit-tested in tests/slic3rutils/test_render_request.cpp. The schema the
// tool publishes is built here too (OrcaMCPRenderSchema.cpp), one function per part, so each option is
// declared where it is read.

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Point.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderLight.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderOverlay.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderSection.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// What a view's camera frames. Kind::Object is one object's instances on the plate; Box and Point are a region
// in bed mm (plate-local with the view's frame: plate_local), a Point being a sphere of radius_mm.
struct FitRequest
{
    enum class Kind { Plate, Object, Box, Point } kind = Kind::Plate;
    int    object_id = -1;
    Vec3d  box_min   = Vec3d::Zero(), box_max = Vec3d::Zero();   // Box
    Vec3d  point     = Vec3d::Zero();                             // Point
    double radius_mm = 0.;                                        // Point
};

// A `fit` value as a view or a layer plan states it: "plate", {object_id | object_index}, {box}, or {point, radius_mm}.
// Throws std::runtime_error naming what to send for any other value or key.
FitRequest read_fit_request(const nlohmann::json& fit);

// The box a Box or Point fit frames, in bed mm: `frame_origin` is added to the given coordinates (the plate's
// front-left corner for plate_local, else zero). A Point's box is its centre +- radius. Undefined for the
// other kinds.
BoundingBoxf3 region_fit_box(const FitRequest& fit, const Vec3d& frame_origin);

// fit mode: the camera gives only the direction, the picture is zoomed to the fit and the eye may be moved back.
// exact: the eye is where camera_position says, with fov_deg or an orthographic ortho_height_mm.
enum class CameraMode { Fit, Exact };

struct ViewRequest
{
    std::optional<CameraPreset> preset;
    std::string                 preset_name;               // as given; echoed in the answer's camera.preset
    std::optional<Vec3d>        camera_position, target;   // as given, in `input_frame`
    std::string                 input_frame = "bed_mm";    // "bed_mm" or "plate_local"; a preset is always bed_mm
    FitRequest                  fit;
    nlohmann::json              fit_json = "plate";        // echoed in the answer's camera.fit
    CameraMode                  mode = CameraMode::Fit;
    bool                        orthographic = false;      // projection: orthographic
    std::optional<double>       fov_deg;                   // exact perspective (30 when not given)
    std::optional<double>       ortho_height_mm;           // exact orthographic
    // A camera object an earlier render returned, drawn again exactly (camera_position / target are then the
    // object's own, when it carries them).
    std::optional<CameraFrame>  replay;
    std::optional<int>          replay_plate;              // the plate the replayed camera was drawn of, when it says
    // compare_with: the file_path of an earlier picture of this session. The view is then drawn with that picture's
    // camera (resolved by the caller from the records); `replay` is set to a camera the view itself gave, if any.
    std::optional<std::string>  compare_with;

    bool exact() const { return mode == CameraMode::Exact; }
};

// What `color_by` draws each part in: the object's colour from the palette (the default), its filament's, the depth of
// the relief from the surface before the texture (relief), or the angle between neighbouring facets (detail).
enum class ColorBy { Object, Filament, Relief, Detail };

struct RenderRequest
{
    int                  plate_index  = -1;
    bool                 save_to_file = false;
    int                  resolution   = 512;
    bool                 resolution_given = false;         // the call named a resolution
    bool                 use_png      = true;
    OverlayOptions       overlays;
    nlohmann::json       layer_view;                       // null: a 3D render
    std::vector<ViewRequest> views;                        // empty with a layer_view
    std::optional<LightRequest> light;                     // a single raking light; unset: the 3D view's two lights
    std::optional<SectionRequest> section;                 // a plane that cuts the side its normal points to away
    std::vector<std::string> paint;                        // channels to tint, canonical order, no duplicates (support seam fuzzy_skin texture)
    std::vector<int>     texture_layers;                   // texture only: the slots to tint, ascending; empty = every slot with a layer
    ColorBy              color_by = ColorBy::Object;
    std::optional<double> relief_range_mm;                 // color_by relief: the colour scale's half range; unset = the measured extremes
    std::optional<double> detail_range_deg;                // color_by detail: the scale's top; unset = the measured 98th percentile
    bool                 texture_preview = false;          // draw each object's painted texture layers as its bake would make them (nothing is baked)
    bool                 contact_sheet = false;            // no views asked: iso, top and front side by side
};

// A camera frame's lens as a request would state it: ExactCameraSpec of an exact view.
ExactCameraSpec exact_spec_of(const ViewRequest& view);

// The call's payload (render_plate_view's arguments). Nullopt, with `error` the message to answer, when a
// value is malformed. `fit` takes {object_id} or {object_index} (the same number; both given and different
// is an error); any other `fit` is the plate. Frame conversion (plate_local) and object lookups are the
// caller's: the parser keeps what was asked.
std::optional<RenderRequest> read_render_request(const nlohmann::json& payload, std::string& error);

// What the render's shader (mcp_render, resources/shaders/{110,140}) is told for every volume it draws. The
// defaults draw what the thumbnail shader draws, so a call that asks for nothing new is the same picture.
struct ShaderSettings
{
    std::array<double, 4> clipping_plane = {0., 0., 0., 1.};   // a fragment with dot(world_pos, plane) < 0 is cut away
    bool                  use_light_dir  = false;               // the top light from light_dir_eye, not the fixed one
    Vec3d                 light_dir_eye  = Vec3d(0., 0., 1.);   // unit vector in eye space
    float                 ambient        = 0.3f;                // the thumbnail shader's INTENSITY_AMBIENT
    bool                  front_light    = true;                // the second light
    float                 top_gain       = 1.f;                 // scales the top light's diffuse term
    float                 specular       = 1.f;                 // 0 leaves the top light's specular highlight out

    bool is_default() const;
};

// The uniforms the render sets on mcp_render beyond thumbnail's own; the shader files must declare each
// (tests/slic3rutils/test_render_shader_files.cpp).
const std::vector<std::string>& shader_settings_uniform_names();

// The tool's input schema and its description.
nlohmann::json render_plate_view_schema();
std::string    render_plate_view_description();

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
