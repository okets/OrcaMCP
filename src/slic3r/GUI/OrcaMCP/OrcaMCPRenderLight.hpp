#ifndef slic3r_OrcaMCPRenderLight_hpp_
#define slic3r_OrcaMCPRenderLight_hpp_

// render_plate_view's `light`: one directional light that rakes across a surface, so a shallow relief (a 0.3 mm
// texture) casts readable shading on its facets. Pure: the request, the direction in bed axes, and the settings the
// mcp_render shader is given once the camera is known. Unit-tested in tests/slic3rutils/test_render_light.cpp.

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "libslic3r/Point.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

struct ShaderSettings;   // OrcaMCPRenderRequest.hpp

// The light as asked. Bed-axes forms: from the unit vector `toward_light` (bed axes, from the surface to the light);
// `azimuth_deg` and `elevation_deg` describe it (azimuth 0 = from +X, counter-clockwise seen from above; elevation
// above the horizon). Camera-relative form (`from` set): from the left, right, top or bottom of each view's image, or
// the camera itself ("front"), `elevation_deg` above the surface the camera sees (a surface facing the camera is 90).
struct LightRequest
{
    Vec3d       toward_light  = Vec3d::UnitZ();
    double      azimuth_deg   = 0.;
    double      elevation_deg = 90.;
    std::string from;             // "left" | "right" | "top" | "bottom" | "front"; empty: the bed-axes forms
};

// "default" or null: nullopt (the 3D view's two lights). {from, elevation_deg?} (elevation 1 to 90, default 20,
// ignored for "front"), {direction: [x, y, z]} -- the way the light travels, bed axes, from the light to the scene -- or
// {azimuth_deg, elevation_deg} (elevation 1 to 90). Throws std::runtime_error with what to send for anything else.
// An answer's light is accepted back: with `from`, its direction, azimuth_deg and bed_elevation_deg (the bed-axes
// light that view used) are ignored.
std::optional<LightRequest> read_light(const nlohmann::json& light);

// The unit vector from the surface to the light, bed axes, as `view` sees it (a camera-relative light turns with it).
Vec3d light_toward_world(const LightRequest& light, const Eigen::Matrix4d& view);

// The answer's `light` for the picture drawn through `view`: direction (from the light to the scene, bed axes),
// azimuth_deg, elevation_deg, and for a camera-relative light `from` with elevation_deg as asked and the bed-axes
// elevation as bed_elevation_deg.
nlohmann::json light_json(const LightRequest& light, const Eigen::Matrix4d& view);

// A direction in bed axes into eye space through `view`'s rotation (a unit vector for a unit vector).
Vec3d light_dir_eye(const Vec3d& world_dir, const Eigen::Matrix4d& view);

// How bright a face is drawn under the raking light: colour * (ambient + emission + n.l * gain * the thumbnail
// shader's top diffuse). A face square to the light reaches k_raking_lit of its colour and one turned away
// k_raking_fill, so shadowed detail stays readable.
constexpr double k_raking_lit  = 0.9;
constexpr double k_raking_fill = 0.28;
constexpr double k_top_diffuse = 0.8 * 0.6;   // LIGHT_TOP_DIFFUSE in thumbnail.vs: 0.8 * INTENSITY_CORRECTION
double raking_brightness(double n_dot_l, const ShaderSettings& settings, double emission);

// The shader settings of a raking light seen through `view`: the one light, no second light, no specular, and the
// fill and gain that give raking_brightness its range given the render's `emission`.
ShaderSettings raking_light_settings(const LightRequest& light, const Eigen::Matrix4d& view, double emission = 0.3);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
