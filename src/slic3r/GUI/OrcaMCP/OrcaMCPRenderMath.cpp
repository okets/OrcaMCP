#include "OrcaMCPRenderMath.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

bool project_point_to_pixel(const CameraFrame& camera, const Vec3d& world, Vec2d& pixel)
{
    const Eigen::Vector4d clip = camera.projection * (camera.view * Eigen::Vector4d(world.x(), world.y(), world.z(), 1.0));
    // w <= 0 is behind the camera (perspective) or a degenerate projection; either way there is no pixel.
    if (!(clip.w() > 1e-12))
        return false;
    const double nx = clip.x() / clip.w();
    const double ny = clip.y() / clip.w();
    const double vx = camera.viewport[0], vy = camera.viewport[1];
    const double vw = camera.viewport[2], vh = camera.viewport[3];
    if (vw <= 0. || vh <= 0.)
        return false;
    pixel.x() = vx + (nx + 1.0) * 0.5 * vw;
    pixel.y() = vy + (1.0 - (ny + 1.0) * 0.5) * vh;  // GL's y is up; image rows count down
    return true;
}

ScreenBBox screen_bbox_of(const CameraFrame& camera, const BoundingBoxf3& box)
{
    ScreenBBox out;
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    int    projected = 0, behind = 0;
    for (int i = 0; i < 8; ++i) {
        const Vec3d corner((i & 1) ? box.max.x() : box.min.x(),
                           (i & 2) ? box.max.y() : box.min.y(),
                           (i & 4) ? box.max.z() : box.min.z());
        Vec2d p;
        if (!project_point_to_pixel(camera, corner, p)) {
            ++behind;
            continue;
        }
        ++projected;
        x0 = std::min(x0, p.x()); y0 = std::min(y0, p.y());
        x1 = std::max(x1, p.x()); y1 = std::max(y1, p.y());
    }
    if (projected == 0)
        return out;
    const double vx0 = camera.viewport[0], vy0 = camera.viewport[1];
    const double vx1 = vx0 + camera.viewport[2], vy1 = vy0 + camera.viewport[3];
    out.clipped = behind > 0 || x0 < vx0 || y0 < vy0 || x1 > vx1 || y1 > vy1;
    out.x0 = std::max(x0, vx0); out.y0 = std::max(y0, vy0);
    out.x1 = std::min(x1, vx1); out.y1 = std::min(y1, vy1);
    out.visible = out.x1 > out.x0 && out.y1 > out.y0;
    return out;
}

bool is_uniform_rgba(const std::vector<unsigned char>& pixels, unsigned int width, unsigned int height)
{
    const size_t n = size_t(width) * size_t(height) * 4;
    if (n == 0 || pixels.size() < n)
        return true;
    for (size_t i = 4; i < n; i += 4)
        if (pixels[i] != pixels[0] || pixels[i + 1] != pixels[1] || pixels[i + 2] != pixels[2] || pixels[i + 3] != pixels[3])
            return false;
    return true;
}

ColorRGBA object_palette_color(int object_index)
{
    static const ColorRGBA k_palette[12] = {
        {0.20f, 0.47f, 0.85f, 1.f}, {0.89f, 0.35f, 0.24f, 1.f}, {0.22f, 0.66f, 0.36f, 1.f}, {0.93f, 0.62f, 0.13f, 1.f},
        {0.58f, 0.34f, 0.78f, 1.f}, {0.10f, 0.62f, 0.66f, 1.f}, {0.80f, 0.25f, 0.55f, 1.f}, {0.52f, 0.58f, 0.12f, 1.f},
        {0.36f, 0.36f, 0.76f, 1.f}, {0.85f, 0.45f, 0.45f, 1.f}, {0.16f, 0.50f, 0.44f, 1.f}, {0.62f, 0.45f, 0.25f, 1.f},
    };
    return k_palette[((object_index % 12) + 12) % 12];
}

ColorRGBA wipe_tower_color() { return ColorRGBA(0.75f, 0.76f, 0.80f, 1.0f); }

bool camera_preset_from_string(const std::string& name, CameraPreset& out)
{
    static const std::pair<const char*, CameraPreset> k_names[] = {
        {"iso", CameraPreset::Iso},   {"top", CameraPreset::Top},     {"front", CameraPreset::Front}, {"back", CameraPreset::Back},
        {"left", CameraPreset::Left}, {"right", CameraPreset::Right}, {"low", CameraPreset::Low},
    };
    for (const auto& [n, p] : k_names)
        if (name == n) { out = p; return true; }
    return false;
}

std::string camera_preset_name(CameraPreset preset)
{
    switch (preset) {
    case CameraPreset::Iso:   return "iso";
    case CameraPreset::Top:   return "top";
    case CameraPreset::Front: return "front";
    case CameraPreset::Back:  return "back";
    case CameraPreset::Left:  return "left";
    case CameraPreset::Right: return "right";
    case CameraPreset::Low:   return "low";
    }
    return "iso";
}

void preset_camera(CameraPreset preset, const BoundingBoxf3& fit, Vec3d& position, Vec3d& target)
{
    target = fit.center();
    const Vec3d  size     = fit.size();
    const double distance = std::max(50.0, 2.2 * size.norm());
    switch (preset) {
    case CameraPreset::Iso:   position = target + Vec3d(0.60, -0.60, 0.52).normalized() * distance; break;
    // A hair off vertical, so the renderer's look_at has a defined up vector (stable_camera_up handles
    // the exact-vertical case too; this keeps the picture's orientation predictable: +Y is up).
    case CameraPreset::Top:   position = Vec3d(target.x(), target.y() - 1e-3 * distance, fit.max.z() + distance); break;
    case CameraPreset::Front: position = target + Vec3d(0., -1., 0.35).normalized() * distance; break;
    case CameraPreset::Back:  position = target + Vec3d(0., 1., 0.35).normalized() * distance; break;
    case CameraPreset::Left:  position = target + Vec3d(-1., 0., 0.35).normalized() * distance; break;
    case CameraPreset::Right: position = target + Vec3d(1., 0., 0.35).normalized() * distance; break;
    case CameraPreset::Low:
        // From the front, a little above the first layers, looking slightly down at them: brims,
        // support feet and the bottom edges read without the view collapsing to a silhouette.
        position   = Vec3d(target.x(), fit.min.y() - distance, fit.min.z() + 0.30 * size.z() + 10.0);
        target.z() = fit.min.z() + 0.15 * size.z();
        break;
    }
}

std::vector<GridSegment> grid_segments(const BoundingBoxf3& plate, double step_mm, int major_every)
{
    std::vector<GridSegment> out;
    if (!(step_mm > 0.))
        return out;
    const double z     = plate.min.z();
    const int    major = std::max(1, major_every);
    auto is_major      = [&](double v) { return std::lround(v / step_mm) % major == 0; };

    for (double x = std::ceil(plate.min.x() / step_mm - 1e-9) * step_mm; x <= plate.max.x() + 1e-9; x += step_mm)
        out.push_back({Vec3d(x, plate.min.y(), z), Vec3d(x, plate.max.y(), z), is_major(x)});
    for (double y = std::ceil(plate.min.y() / step_mm - 1e-9) * step_mm; y <= plate.max.y() + 1e-9; y += step_mm)
        out.push_back({Vec3d(plate.min.x(), y, z), Vec3d(plate.max.x(), y, z), is_major(y)});
    return out;
}

}}} // namespace Slic3r::GUI::OrcaMCP
