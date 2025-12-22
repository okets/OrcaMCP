#ifndef slic3r_GUI_OrcaMCPPlateUtils_hpp_
#define slic3r_GUI_OrcaMCPPlateUtils_hpp_

#include <nlohmann/json.hpp>
#include <GL/glew.h>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI {

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
    static void RenderThumbnail(ThumbnailData& thumbnail_data,
        const Vec3d& camera_position, const Vec3d& target, int plate_index);

    static nlohmann::json GetPlates(bool with_model_object_features);
    static nlohmann::json GetModelObjectFeaturesJson(const ModelObject* obj);
};

}} // namespace Slic3r::GUI

#endif
