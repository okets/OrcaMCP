#ifndef slic3r_GUI_OrcaMCPPlateUtils_hpp_
#define slic3r_GUI_OrcaMCPPlateUtils_hpp_

#include <nlohmann/json.hpp>
#include <GL/glew.h>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateOccupancy.hpp"

namespace Slic3r { namespace GUI {

// The camera a thumbnail was rendered with, so a pixel of that image can be turned back into a
// ray later. The matrices and viewport are kept in pick_facet's own CameraFrame, so seeing and
// pointing cannot drift into two different ideas of the same camera.
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

    // `object`'s printed footprint, honouring its own per-object brim overrides before the global
    // print settings. One implementation, used both by the scene report and by the collision check
    // set_prime_tower_position runs, so the two cannot disagree about where an object ends.
    static ObjectFootprint GetObjectFootprint(const ModelObject& object, const DynamicPrintConfig& print_cfg);

    // Turntable preview - captures multiple views around the plate
    static nlohmann::json CaptureTurntablePreview(int plate_index, int view_count = 4,
                                                   int resolution = 128);

    // Cleanup old preview files (call on startup)
    static void CleanupPreviews();

private:
    // `out_camera`, when given, receives the camera the thumbnail was actually drawn with --
    // which the caller cannot predict, because zoom_to_box frames the plate's contents.
    static void RenderThumbnail(ThumbnailData& thumbnail_data,
        const Vec3d& camera_position, const Vec3d& target, int plate_index,
        RenderCameraInfo* out_camera = nullptr);

    static nlohmann::json GetPlates(bool with_model_object_features);
    static nlohmann::json GetModelObjectFeaturesJson(const ModelObject* obj);
};

}} // namespace Slic3r::GUI

#endif
