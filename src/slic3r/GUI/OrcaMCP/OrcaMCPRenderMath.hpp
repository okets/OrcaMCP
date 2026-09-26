#ifndef slic3r_OrcaMCPRenderMath_hpp_
#define slic3r_OrcaMCPRenderMath_hpp_

// Pure geometry, colour and camera decisions behind render_plate_view. Nothing here draws, touches
// wx or reads or writes files: frame_camera uses Camera's matrix arithmetic, none of its GL calls.
// So every function is unit-tested in tests/slic3rutils/test_render_math.cpp, without a GL context.
// The renderer and the 2D overlay code call these; they never duplicate the arithmetic. Where the
// rendered images are written is OrcaMCPImageFiles.

#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp"  // CameraFrame

namespace Slic3r { namespace GUI {
class Camera;
namespace OrcaMCP {

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

// The room a framed render leaves around what it frames: the box spans at most 1/1.08 of the image.
constexpr double k_fit_margin = 1.08;

// The zoom -- Camera's, in pixels per mm on the plane through `target` square to the view -- at which
// every corner of `box` lands inside a `width` x `height` image with `margin` to spare, seen by a
// perspective camera at `position` looking at `target` with the basis Camera::look_at builds from
// `up`. The smaller of two limits: the perspective one at this distance, where corners nearer the
// camera than the target project larger, and the orthographic one the view tends to as the camera
// backs away. Camera::apply_projection may back the camera away along its axis to keep the near
// plane 100 mm out; the box's projection moves monotonically between those two limits as it does,
// so it fits wherever the camera ends up. 0 when there is nothing to fit.
double fit_zoom_to_box(const Vec3d& position, const Vec3d& target, const Vec3d& up, const BoundingBoxf3& box,
                       int width, int height, double margin);

// Points `camera` from `position` at `target` and zooms it so the whole of `fit` is in frame, with
// a depth range covering both `scene` and `fit`. Call it after set_type and set_viewport. The single
// camera set-up behind every render_plate_view picture and turntable view. It uses Camera's matrix
// arithmetic only, none of its GL calls, so it is tested without a GL context.
void frame_camera(Camera& camera, const Vec3d& position, const Vec3d& target, const BoundingBoxf3& fit,
                  const BoundingBoxf3& scene);

// The view, projection and viewport `camera` draws with, as pick_facet and the overlays read them.
CameraFrame camera_frame_of(const Camera& camera);

// Whether a volume of the 3D view belongs in one plate's picture: printable, on that plate (its
// instance is one the plate holds, partly outside it or not, or it is that plate's own wipe tower),
// and reaching above the bed. The same membership get_scene_info reports plates from, so an object
// listed on plate N is drawn in plate N's picture.
bool belongs_in_plate_view(bool printable, bool on_plate, const BoundingBoxf3& volume_box);

// The 3D view's scene a render drew from.
struct RenderScene
{
    size_t model_volumes = 0;     // model volumes in it, on any plate
    bool   current       = true;  // false: the hidden 3D view could not be brought up to date
};

// {"scene_current": bool}, and a "warning" line when it is false: the fields every render reports,
// blank or not, since a picture of a stale scene is a picture of the wrong thing.
nlohmann::json render_scene_json(const RenderScene& scene);

// Why a picture came out as one flat colour: the 3D view had no model volumes at all, none of them
// were on this plate (`drawn` is 0), or they were drawn and the camera looked elsewhere.
// `plate_box` is bed mm.
std::string uniform_image_hint(const RenderScene& scene, size_t drawn, int plate_index, const BoundingBoxf3& plate_box);

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
