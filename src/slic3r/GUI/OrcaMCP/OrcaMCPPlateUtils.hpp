#ifndef slic3r_GUI_OrcaMCPPlateUtils_hpp_
#define slic3r_GUI_OrcaMCPPlateUtils_hpp_

#include <nlohmann/json.hpp>
#include <GL/glew.h>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp"

namespace Slic3r { namespace GUI {

// The camera a thumbnail was rendered with, so a pixel of that image can be turned back into a
// ray later. The matrices and viewport are kept in pick_facet's own CameraFrame, so seeing and
// pointing cannot drift into two different ideas of the same camera.
struct RenderCameraInfo
{
    OrcaMCP::CameraFrame frame;
    bool                 perspective = true;
};

class OrcaMCPPlateUtils {
public:
    static nlohmann::json RenderPlateView(const nlohmann::json& params);
    static nlohmann::json GetCurrentProject(const bool with_model_object_features);

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
