#ifndef slic3r_GUI_OrcaMCPPlateUtils_hpp_
#define slic3r_GUI_OrcaMCPPlateUtils_hpp_

#include <nlohmann/json.hpp>
#include <optional>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "libslic3r/Color.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateOccupancy.hpp"

namespace Slic3r { namespace GUI {

// The camera a thumbnail was rendered with, so a pixel of that image can be turned back into a
// ray later. The matrices and viewport are kept in pick_facet's own CameraFrame, so seeing and
// pointing cannot drift into two different ideas of the same camera.
// How RenderThumbnail should draw. Defaults are vision v2's: each object in its palette colour on a
// light background, lit enough that unlit faces still read.
struct RenderOptions
{
    bool      palette_colors = true;                    // object_palette_color(object_index); false = filament colours
    ColorRGBA background{0.93f, 0.93f, 0.93f, 1.0f};
    float     emission = 0.3f;                          // the thumbnail shader's emission_factor
    // What the view is zoomed to. Unset: the plate's visible volumes, as before. A preset or a
    // `fit` sets it, otherwise "fit to one object" would move the camera but not narrow the view.
    std::optional<BoundingBoxf3> zoom_box;
};

// One volume RenderThumbnail actually drew, with what an agent needs to name and locate it.
struct RenderedVolume
{
    int           object_index = -1;                    // index into Model::objects; -1 for the wipe tower
    std::string   name;
    BoundingBoxf3 world_bbox;                           // bed mm
    ColorRGBA     color;
    bool          wipe_tower = false;
};

// What a render produced, beyond the pixels: enough to say "you drew nothing" with a reason.
struct RenderReport
{
    std::vector<RenderedVolume> drawn;
    bool                        uniform_image = false;  // every pixel identical, before overlays
    BoundingBoxf3               plate_box;              // the requested plate's build volume, bed mm
    OrcaMCP::RenderScene        scene;                  // the 3D view's scene it drew from
};

struct RenderCameraInfo
{
    OrcaMCP::CameraFrame frame;
    bool                 perspective = true;
};

// The prime tower on one plate, read from the live print, printer and project configs.
//
// It is here rather than inline in the reporting code because set_prime_tower_position needs the
// same numbers to validate against: the range it may write into, and the footprint that comes back
// out. Two copies of this arithmetic is how the reported position and the accepted position drift
// apart, which is exactly the class of bug this whole feature exists to close.
struct PrimeTowerState
{
    // Whether a tower will actually be printed here, and why not when it will not be.
    bool                      printed = false;
    OrcaMCP::PrimeTowerVerdict verdict = OrcaMCP::PrimeTowerVerdict::Disabled;

    // wipe_tower_x / wipe_tower_y as stored for this plate: PLATE-LOCAL millimetres, measured from
    // the plate's own front-left corner, which is the frame those two config keys use.
    Vec2d stored_position = Vec2d::Zero();
    // The same corner in PLATE MILLIMETRES -- the world frame get_scene_info reports object
    // bounding boxes in, where plate 2 sits several hundred millimetres along +X from plate 1.
    Vec2d corner = Vec2d::Zero();

    Vec3d  size       = Vec3d::Zero();  // the tower body: width, depth, height
    double brim_width = 0.0;            // prime_tower_brim_width, resolved (never negative here)

    BoundingBoxf body;       // the tower body in plate millimetres, brim excluded
    BoundingBoxf footprint;  // the same, brim INCLUDED -- the bed area the tower actually occupies

    Vec2d                    plate_size  = Vec2d::Zero();
    Vec3d                    plate_origin = Vec3d::Zero();
    OrcaMCP::PrimeTowerRange legal_range;  // plate-local, for stored_position
};

// A model object's printed footprint. The bounding box is the model; `rect` is the bed area the
// printed thing actually covers, which is larger by the brim whenever brim_type is not no_brim.
struct ObjectFootprint
{
    BoundingBoxf              body;  // the bounding box's shadow on the bed, plate millimetres
    BoundingBoxf              rect;  // the same grown by the brim -- what an agent must avoid
    std::string               brim_type;
    OrcaMCP::ObjectBrimExtent brim;
};

class OrcaMCPPlateUtils {
public:
    static nlohmann::json RenderPlateView(const nlohmann::json& params);
    static nlohmann::json GetCurrentProject(const bool with_model_object_features);

    // Reads the prime tower's state on `plate_index`. `full_config` is passed in because building
    // it is the expensive part and the per-plate loop only needs one.
    static PrimeTowerState GetPrimeTowerState(int plate_index, const DynamicPrintConfig& full_config);
    static PrimeTowerState GetPrimeTowerState(int plate_index);

    // The `prime_tower` object of a plate's report. Always carries `printed`; carries the geometry
    // only when a tower is actually printed, so "no tower here" cannot be misread as "tower at 0,0".
    static nlohmann::json PrimeTowerJson(const PrimeTowerState& state);

    // `object`'s printed footprint over `body_box` -- on a plate, the box of the instances that plate
    // holds (OrcaMCP::instances_on_plate) -- honouring its own per-object brim overrides before the
    // global print settings. One implementation, used by the scene report, the first-layer plan and
    // the collision check set_prime_tower_position runs, so they cannot disagree about where an
    // object ends.
    static ObjectFootprint GetObjectFootprint(const ModelObject& object, const BoundingBoxf3& body_box,
                                              const DynamicPrintConfig& print_cfg);

    // Turntable preview - captures multiple views around the plate
    static nlohmann::json CaptureTurntablePreview(int plate_index, int view_count = 4,
                                                   int resolution = 128);

    // Removes the render and preview images earlier sessions left in the temp directory (call on
    // startup): orcamcp_render_* and orcamcp_preview_* images older than an hour, so the images of
    // another OrcaMCP running at the same time survive.
    static void CleanupTempImages();

private:
    // `out_camera`, when given, receives the camera the thumbnail was actually drawn with --
    // which the caller cannot predict, because zoom_to_box frames the plate's contents.
    static void RenderThumbnail(ThumbnailData& thumbnail_data,
        const Vec3d& camera_position, const Vec3d& target, int plate_index,
        RenderCameraInfo* out_camera = nullptr);
    // `options` chooses colours, background and lighting; `report`, when given, receives what was
    // drawn and whether the result is a single flat colour.
    static void RenderThumbnail(ThumbnailData& thumbnail_data,
        const Vec3d& camera_position, const Vec3d& target, int plate_index,
        RenderCameraInfo* out_camera, const RenderOptions& options, RenderReport* report);

    static nlohmann::json GetPlates(bool with_model_object_features);
    static nlohmann::json GetModelObjectFeaturesJson(const ModelObject* obj);
};

}} // namespace Slic3r::GUI

#endif
