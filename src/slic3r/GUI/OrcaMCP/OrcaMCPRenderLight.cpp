#include "OrcaMCPRenderLight.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

Vec3d read_vector(const nlohmann::json& v)
{
    if (!v.is_array() || v.size() != 3 || !std::all_of(v.begin(), v.end(), [](const nlohmann::json& n) { return n.is_number(); }))
        throw std::runtime_error("light.direction must be [x, y, z], the way the light travels in bed axes");
    return Vec3d(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
}

double read_angle(const nlohmann::json& light, const char* key)
{
    if (!light.contains(key) || !light[key].is_number())
        throw std::runtime_error(std::string("light needs ") + key + " (a number of degrees) with the other of azimuth_deg and elevation_deg");
    return light[key].get<double>();
}

LightRequest from_toward_light(const Vec3d& toward)
{
    LightRequest out;
    out.toward_light   = toward.normalized();
    out.elevation_deg  = std::asin(std::clamp(out.toward_light.z(), -1., 1.)) * 180. / PI;
    out.azimuth_deg    = std::atan2(out.toward_light.y(), out.toward_light.x()) * 180. / PI;
    if (out.azimuth_deg < 0.)
        out.azimuth_deg += 360.;
    return out;
}

} // namespace

std::optional<LightRequest> read_light(const nlohmann::json& light)
{
    if (light.is_null() || (light.is_string() && light == "default"))
        return std::nullopt;
    static const char* const k_usage =
        "light must be \"default\", {from: left|right|top|bottom|front, elevation_deg}, {direction: [x, y, z]} or {azimuth_deg, elevation_deg}";
    if (!light.is_object())
        throw std::runtime_error(k_usage);
    for (const auto& item : light.items())
        if (item.key() != "direction" && item.key() != "azimuth_deg" && item.key() != "elevation_deg" && item.key() != "applied" &&
            item.key() != "warning" && item.key() != "from" && item.key() != "bed_elevation_deg")
            throw std::runtime_error("light has no key \"" + item.key() + "\"; " + k_usage);

    if (light.contains("from")) {
        const std::string from = light["from"].is_string() ? light["from"].get<std::string>() : std::string();
        if (from != "left" && from != "right" && from != "top" && from != "bottom" && from != "front")
            throw std::runtime_error("light.from must be \"left\", \"right\", \"top\", \"bottom\" or \"front\"");
        LightRequest out;
        out.from          = from;
        out.elevation_deg = 20.;
        if (light.contains("elevation_deg")) {
            out.elevation_deg = read_angle(light, "elevation_deg");
            if (!(out.elevation_deg >= 1. && out.elevation_deg <= 90.))
                throw std::runtime_error("light.elevation_deg must be between 1 and 90 degrees above the surface the camera sees");
        }
        return out;
    }

    const bool has_direction = light.contains("direction");
    const bool has_angles    = light.contains("azimuth_deg") || light.contains("elevation_deg");
    if (!has_direction && !has_angles)
        throw std::runtime_error(k_usage);

    if (has_direction) {
        const Vec3d travel = read_vector(light["direction"]);
        if (!(travel.norm() > 1e-9))
            throw std::runtime_error("light.direction must not be the zero vector");
        const LightRequest from_direction = from_toward_light(-travel);
        // The light an answer returns carries all three; they are accepted back when they agree. Straight above,
        // the azimuth means nothing and is not compared.
        if (has_angles) {
            const double azimuth = read_angle(light, "azimuth_deg"), elevation = read_angle(light, "elevation_deg");
            double       turn    = std::fmod(std::abs(azimuth - from_direction.azimuth_deg), 360.);
            turn = std::min(turn, 360. - turn);
            const bool overhead = std::abs(from_direction.toward_light.z()) > 0.99999;
            if ((!overhead && turn > 0.01) || std::abs(elevation - from_direction.elevation_deg) > 0.01)
                throw std::runtime_error("light.direction and azimuth_deg / elevation_deg describe different lights; send one of them");
        }
        return from_direction;
    }
    const double azimuth   = read_angle(light, "azimuth_deg");
    const double elevation = read_angle(light, "elevation_deg");
    if (!(elevation >= 1. && elevation <= 90.))
        throw std::runtime_error("light.elevation_deg must be between 1 and 90: a light at the horizon lights nothing, and one below it lights the underside");
    const double az = azimuth * PI / 180., el = elevation * PI / 180.;
    LightRequest out;
    out.toward_light  = Vec3d(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el));
    out.azimuth_deg   = std::fmod(std::fmod(azimuth, 360.) + 360., 360.);
    out.elevation_deg = elevation;
    return out;
}

namespace {

// A camera-relative light in eye space (x right, y up, z toward the camera): `elevation` above the surface square to
// the view, coming from the named side.
Vec3d relative_light_eye(const LightRequest& light)
{
    if (light.from == "front")
        return Vec3d::UnitZ();
    const double el = light.elevation_deg * PI / 180.;
    const double c = std::cos(el), s = std::sin(el);
    if (light.from == "left")   return Vec3d(-c, 0., s);
    if (light.from == "right")  return Vec3d(c, 0., s);
    if (light.from == "top")    return Vec3d(0., c, s);
    return Vec3d(0., -c, s);   // bottom
}

} // namespace

Vec3d light_toward_world(const LightRequest& light, const Eigen::Matrix4d& view)
{
    if (light.from.empty())
        return light.toward_light;
    return (view.block<3, 3>(0, 0).transpose() * relative_light_eye(light)).normalized();
}

nlohmann::json light_json(const LightRequest& light, const Eigen::Matrix4d& view)
{
    const LightRequest bed = from_toward_light(light_toward_world(light, view));
    const Vec3d travel = -bed.toward_light;
    nlohmann::json out = {{"direction", {travel.x(), travel.y(), travel.z()}},
                          {"azimuth_deg", bed.azimuth_deg},
                          {"elevation_deg", light.from.empty() ? bed.elevation_deg : light.elevation_deg}};
    if (!light.from.empty()) {
        out["from"]              = light.from;
        out["bed_elevation_deg"] = bed.elevation_deg;
    }
    return out;
}

Vec3d light_dir_eye(const Vec3d& world_dir, const Eigen::Matrix4d& view)
{
    return (view.block<3, 3>(0, 0) * world_dir).normalized();
}

double raking_brightness(double n_dot_l, const ShaderSettings& settings, double emission)
{
    return settings.ambient + emission + std::max(n_dot_l, 0.) * k_top_diffuse * settings.top_gain;
}

ShaderSettings raking_light_settings(const LightRequest& light, const Eigen::Matrix4d& view, double emission)
{
    ShaderSettings settings;
    settings.use_light_dir = true;
    settings.light_dir_eye = light.from.empty() ? light_dir_eye(light.toward_light, view) : relative_light_eye(light);
    settings.ambient       = float(std::max(0., k_raking_fill - emission));
    settings.top_gain      = float((k_raking_lit - (settings.ambient + emission)) / k_top_diffuse);
    settings.front_light   = false;
    settings.specular      = 0.f;
    return settings;
}

}}} // namespace Slic3r::GUI::OrcaMCP
