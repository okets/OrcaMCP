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
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderOverlay.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// What a view's camera frames. Kind::Object is one object's instances on the plate.
struct FitRequest
{
    enum class Kind { Plate, Object } kind = Kind::Plate;
    int object_id = -1;
};

struct ViewRequest
{
    std::optional<CameraPreset> preset;
    std::string                 preset_name;               // as given; echoed in the answer's camera.preset
    std::optional<Vec3d>        camera_position, target;   // as given, in `input_frame`
    std::string                 input_frame = "bed_mm";    // "bed_mm" or "plate_local"; a preset is always bed_mm
    FitRequest                  fit;
    nlohmann::json              fit_json = "plate";        // echoed in the answer's camera.fit
};

struct RenderRequest
{
    int                  plate_index  = -1;
    bool                 save_to_file = false;
    int                  resolution   = 512;
    bool                 use_png      = true;
    OverlayOptions       overlays;
    nlohmann::json       layer_view;                       // null: a 3D render
    std::vector<ViewRequest> views;                        // empty with a layer_view
    bool                 contact_sheet = false;            // no views asked: iso, top and front side by side
};

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
