#include "OrcaMCPRenderMath.hpp"
#include "OrcaMCPPlateOccupancy.hpp"  // stable_camera_up
#include "slic3r/GUI/Camera.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <limits>

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

double fit_zoom_to_box(const Vec3d& position, const Vec3d& target, const Vec3d& up, const BoundingBoxf3& box,
                       int width, int height, double margin)
{
    if (!box.defined || width <= 0 || height <= 0 || !(margin > 0.))
        return 0.;
    // Camera::look_at's basis: unit_z points from the target back to the camera.
    const double distance = (position - target).norm();
    const Vec3d  unit_z   = (position - target).normalized();
    const Vec3d  unit_x   = up.cross(unit_z).normalized();
    const Vec3d  unit_y   = unit_z.cross(unit_x).normalized();
    if (!(distance > 0.) || !unit_x.allFinite() || !unit_y.allFinite())
        return 0.;

    // Camera::apply_projection spans (width / 2) / zoom mm either side of the axis at the target's
    // distance, so a point `x` mm off the axis there lands at the image edge when zoom = (width / 2) / x.
    const double half_w = 0.5 * width / margin;
    const double half_h = 0.5 * height / margin;
    double       zoom   = std::numeric_limits<double>::max();
    auto limit = [&zoom](double half_extent, double offset) {
        if (offset > 0.)
            zoom = std::min(zoom, half_extent / offset);
    };
    for (int i = 0; i < 8; ++i) {
        const Vec3d corner((i & 1) ? box.max.x() : box.min.x(),
                           (i & 2) ? box.max.y() : box.min.y(),
                           (i & 4) ? box.max.z() : box.min.z());
        const Vec3d  rel   = corner - position;
        const double x     = std::abs(rel.dot(unit_x));
        const double y     = std::abs(rel.dot(unit_y));
        const double depth = -rel.dot(unit_z);  // along the view, from the camera
        limit(half_w, x);                       // orthographic: the camera far away
        limit(half_h, y);
        if (depth > 1e-9) {                     // perspective: distance / depth times larger here
            limit(half_w, x * distance / depth);
            limit(half_h, y * distance / depth);
        }
    }
    return zoom == std::numeric_limits<double>::max() ? 0. : zoom;
}

void frame_camera(Camera& camera, const Vec3d& position, const Vec3d& target, const BoundingBoxf3& fit,
                  const BoundingBoxf3& scene)
{
    // look_at comes first: the zoom depends on the direction the box is seen from. A zoom sized
    // before it is sized for the Camera's default orientation instead, which is how tall objects
    // were cut off.
    //
    // Not a plain Vec3d::UnitZ() for up: look_at builds its basis from up.cross(view_direction), and
    // for a camera directly above its target that cross product is zero -- an ordinary-looking plan
    // view whose view matrix pick_facet could not invert. stable_camera_up falls back to +Y for
    // exactly that case and returns +Z for every other view.
    const Vec3d up = stable_camera_up(position, target);
    camera.set_scene_box(scene);
    camera.look_at(position, target, up);
    const std::array<int, 4>& viewport = camera.get_viewport();
    const double zoom = fit_zoom_to_box(position, target, up, fit, viewport[2], viewport[3], k_fit_margin);
    if (zoom > 0.)
        camera.set_zoom(zoom);
    // Near and far planes around everything that may be drawn, the fitted box included when it
    // reaches past the plate.
    BoundingBoxf3 depth_box = scene;
    depth_box.merge(fit);
    camera.apply_projection(depth_box);
}

CameraFrame camera_frame_of(const Camera& camera)
{
    CameraFrame frame;
    frame.view       = camera.get_view_matrix().matrix();
    frame.projection = camera.get_projection_matrix().matrix();
    frame.viewport   = camera.get_viewport();
    return frame;
}

namespace {

// Resolved, so a path handed back later compares equal to it whatever links the temp directory
// sits behind (/var is /private/var on macOS), and without the trailing separator $TMPDIR carries,
// so it equals the parent_path() of a file inside it.
boost::filesystem::path image_directory()
{
    boost::filesystem::path directory = boost::filesystem::weakly_canonical(boost::filesystem::temp_directory_path());
    directory.remove_trailing_separator();
    return directory;
}

} // namespace

std::string mcp_image_directory() { return image_directory().string(); }

std::string new_mcp_image_path(const std::string& prefix, const std::string& tag, const std::string& extension)
{
    static std::atomic<unsigned> s_sequence{0};
    const std::string name = prefix + std::to_string(std::time(nullptr)) + "_" + std::to_string(s_sequence.fetch_add(1)) +
                             "_" + tag + extension;
    return (image_directory() / name).string();
}

bool is_mcp_image_file_name(const std::string& file_name)
{
    const bool ours  = boost::starts_with(file_name, k_render_image_prefix) || boost::starts_with(file_name, k_preview_image_prefix);
    const bool image = boost::ends_with(file_name, ".png") || boost::ends_with(file_name, ".jpg");
    return ours && image;
}

bool is_mcp_image_path(const std::string& path)
{
    namespace fs = boost::filesystem;
    const fs::path given(path);
    if (!given.is_absolute())
        return false;
    boost::system::error_code ec;
    const fs::path resolved = fs::weakly_canonical(given, ec);
    return !ec && is_mcp_image_file_name(resolved.filename().string()) && resolved.parent_path() == image_directory();
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
