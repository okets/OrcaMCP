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

// The light as asked: from the unit vector `toward_light` (bed axes, from the surface to the light). `azimuth_deg` and
// `elevation_deg` describe it (azimuth 0 = from +X, counter-clockwise seen from above; elevation above the horizon).
struct LightRequest
{
    Vec3d  toward_light = Vec3d::UnitZ();
    double azimuth_deg   = 0.;
    double elevation_deg = 90.;
};

// "default" or null: nullopt (the 3D view's two lights). {direction: [x, y, z]} -- the way the light travels, bed axes,
// from the light to the scene -- or {azimuth_deg, elevation_deg} (elevation 1 to 90). Throws std::runtime_error with
// what to send for anything else: a zero direction, elevation outside 1..90, both forms, an unknown key.
std::optional<LightRequest> read_light(const nlohmann::json& light);

// The answer's `light`: direction (from the light to the scene), azimuth_deg, elevation_deg.
nlohmann::json light_json(const LightRequest& light);

// A direction in bed axes into eye space through `view`'s rotation (a unit vector for a unit vector).
Vec3d light_dir_eye(const Vec3d& world_dir, const Eigen::Matrix4d& view);

// The shader settings of a raking light seen through `view`: the one light from light_dir_eye, little fill (ambient
// 0.15), no second light, no specular.
ShaderSettings raking_light_settings(const LightRequest& light, const Eigen::Matrix4d& view);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
