#ifndef slic3r_OrcaMCPRenderMath_hpp_
#define slic3r_OrcaMCPRenderMath_hpp_

// Pure geometry, colour and camera decisions behind render_plate_view. Nothing here touches GL or
// wx, so every function is unit-tested in tests/slic3rutils/test_render_math.cpp. The renderer and
// the 2D overlay code call these; they never duplicate the arithmetic.

#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp"  // CameraFrame

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Where a 3D bounding box lands in the image. Pixels have a top-left origin, matching the images
// the tool writes and the pixel_origin it reports. The box is intersected with the viewport.
struct ScreenBBox
{
    double x0 = 0., y0 = 0., x1 = 0., y1 = 0.;
    bool   visible = false;  // some part of the box projects inside the viewport
    bool   clipped = false;  // some corner fell outside the viewport or behind the camera
};

// Pixel of a bed-mm point through `camera`. False when the point is behind the camera or the
// matrices are degenerate.
bool project_point_to_pixel(const CameraFrame& camera, const Vec3d& world, Vec2d& pixel);

ScreenBBox screen_bbox_of(const CameraFrame& camera, const BoundingBoxf3& box);

// True when every RGBA pixel equals the first: a picture of nothing.
bool is_uniform_rgba(const std::vector<unsigned char>& pixels, unsigned int width, unsigned int height);

// Twelve colours chosen to stay apart from each other on a light background; object i gets
// palette[i mod 12], so an object keeps its colour for as long as its index is stable.
ColorRGBA object_palette_color(int object_index);
// The wipe tower's fixed grey, never returned by the palette.
ColorRGBA wipe_tower_color();

enum class CameraPreset { Iso, Top, Front, Back, Left, Right, Low };

bool        camera_preset_from_string(const std::string& name, CameraPreset& out);
std::string camera_preset_name(CameraPreset preset);

// A camera that frames `fit` (bed mm) from the preset's direction. The distance is 2.2x the box
// diagonal and never under 50 mm, so a small object still gets a sensible frame.
void preset_camera(CameraPreset preset, const BoundingBoxf3& fit, Vec3d& position, Vec3d& target);

// Grid lines at z = plate.min.z inside the plate's footprint, every `step_mm`. A line is `major`
// when its coordinate is a multiple of step_mm * major_every (the origin line counts).
struct GridSegment
{
    Vec3d a, b;
    bool  major = false;
};
std::vector<GridSegment> grid_segments(const BoundingBoxf3& plate, double step_mm, int major_every);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
