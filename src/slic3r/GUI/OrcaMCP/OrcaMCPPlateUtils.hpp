#ifndef slic3r_GUI_OrcaMCPPlateUtils_hpp_
#define slic3r_GUI_OrcaMCPPlateUtils_hpp_

#include <nlohmann/json.hpp>
#include <functional>
#include <optional>
#include <vector>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPMeshHealth.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderPaint.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.hpp"
#include "libslic3r/Color.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPBedExclusion.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateOccupancy.hpp"

namespace Slic3r { class GLVolume; class GLShaderProgram; }

namespace Slic3r { namespace GUI {

// The camera a thumbnail was rendered with, so a pixel of that image can be turned back into a
// ray later. The matrices and viewport are kept in pick_facet's own CameraFrame, so seeing and
// pointing cannot drift into two different ideas of the same camera.
// What a render hook is handed: the camera and the shader in use while a volume (or a pass) is drawn.
struct RenderPass
{
    const Transform3d& view;
    const Transform3d& projection;
    GLShaderProgram&   shader;
    int                plate_index;
    const std::vector<GLVolume*>& drawn;   // the volumes this picture draws, in order
};

// How RenderThumbnail should draw. Defaults are vision v2's: each object in its palette colour on a
// light background, lit enough that unlit faces still read.
struct RenderOptions
{
    bool      palette_colors = true;                    // object_palette_color(object_index); false = filament colours
    bool      neutral_base   = false;                   // paint tints asked for: unpainted surfaces in GLVolume::NEUTRAL_COLOR so they read
    OrcaMCP::ColorBy color_by = OrcaMCP::ColorBy::Object; // what the report calls the colours' source
    ColorRGBA background{0.93f, 0.93f, 0.93f, 1.0f};
    float     emission = 0.3f;                          // the thumbnail shader's emission_factor
    // What the view is zoomed to. Unset: the plate's visible volumes, as before. A preset or a
    // `fit` sets it, otherwise "fit to one object" would move the camera but not narrow the view.
    std::optional<BoundingBoxf3> zoom_box;

    // Hooks later render options plug into; all empty by default, which draws what the render has always drawn.
    // draw_volume runs before each volume's own drawing and returns true when it drew the volume itself (the
    // texture preview); after_volumes run once the volumes are drawn, shader still in use (paint tints, section
    // caps); shading is what the shader is told (light, clipping plane); camera_frame draws with exactly these
    // matrices instead of framing a box (the exact camera).
    std::function<bool(const GLVolume&, RenderPass&)>   draw_volume;
    std::vector<std::function<void(RenderPass&)>>       after_volumes;
    OrcaMCP::ShaderSettings                             shading;
    std::optional<OrcaMCP::CameraFrame>                 camera_frame;
    // camera_mode exact: the eye is the one given, with this lens, built inside the render from the depth box it draws.
    std::optional<OrcaMCP::ExactCameraSpec>             exact_camera;
    bool                                                orthographic = false;   // fit mode, orthographic projection
    std::optional<OrcaMCP::LightRequest>                light;                  // a raking light: shading is worked out per camera
};

// One volume RenderThumbnail actually drew, with what an agent needs to name and locate it.
struct RenderedVolume
{
    int           object_index = -1;                    // index into Model::objects; -1 for the wipe tower
    int           volume_id    = -1;                    // the part within the object (get_object_info's volume_id)
    int           instance_id  = -1;
    std::string   name;
    BoundingBoxf3 world_bbox;                           // bed mm
    ColorRGBA     color;                                // the colour the label is drawn in (a colour-painted part: its base filament's)
    std::string   color_source = "palette";             // palette | filament | paint | neutral (OrcaMCP::color_source_of)
    bool          wipe_tower = false;
};

// What a render produced, beyond the pixels: enough to say "you drew nothing" with a reason.
struct RenderReport
{
    std::vector<RenderedVolume> drawn;
    bool                        uniform_image = false;  // every pixel identical, before overlays
    BoundingBoxf3               plate_box;              // the requested plate's build volume, bed mm
    OrcaMCP::RenderScene        scene;                  // the 3D view's scene it drew from
    bool                        completed = false;      // the picture was drawn (no early return)
    bool                        shader_fallback = false; // mcp_render did not build on this driver: drawn with thumbnail, shading ignored
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

// What get_scene_info says about one plate, measured by GetPlates.
struct PlateEntry
{
    std::string    name;
    int            index      = 0;
    bool           is_current = false;  // the plate per-plate tools act on, the one `bed` describes
    BoundingBoxf3  box;
    nlohmann::json model_objects  = nlohmann::json::array();
    nlohmann::json prime_tower    = nlohmann::json::object();
    nlohmann::json excluded_areas = nlohmann::json::array();
    nlohmann::json exclusion_regions = nlohmann::json::array(); // every region the 3D view draws (exclusion_region_json)
    nlohmann::json occupancy      = nlohmann::json::array();
    nlohmann::json settings       = nlohmann::json::object(); // the plate's own settings (set_plate_settings' shape)
    nlohmann::json effective      = nlohmann::json::object(); // what applies on it
    nlohmann::json layer_gcodes   = nlohmann::json::array();  // its G-code at a layer (plate_layer_gcodes_json)
};

namespace OrcaMCP {
// The bed exclusion regions plate `plate` draws, in plate mm, from the plater's config (none without one).
std::vector<ExclusionRegion> plate_exclusion_regions(PartPlate& plate);
} // namespace OrcaMCP

// One view of a 3D render as the first step resolved it: the request, the box it frames and its camera in bed mm.
struct ResolvedView
{
    OrcaMCP::ViewRequest request;
    BoundingBoxf3        fit_box;
    Vec3d                camera_position = Vec3d::Zero();
    Vec3d                target          = Vec3d::Zero();
    bool                 pose_known      = true;   // false: a replayed camera that carries no camera_position / target
};

// A render_plate_view call in three steps (as repair_mesh): PrepareRender reads the request and captures what it
// needs (main thread), ComputeRender does the work that needs no scene (HTTP thread; empty until a render option
// has mesh work to do), DrawRender draws on the main thread and checks the scene is still what was captured.
struct RenderJob
{
    using Work = std::function<void(const std::function<bool()>& stop)>;

    OrcaMCP::RenderRequest                    request;
    std::vector<ResolvedView>                 views;
    BoundingBoxf3                             plate_box;
    std::vector<BoundingBoxf3>                legacy_boxes;
    std::vector<OrcaMCP::ExclusionRegion>     exclusion_regions;
    std::vector<Work>                         work;                           // run by ComputeRender on the HTTP thread

    // paint: the tints asked for, what the first step captured of the parts that carry them, and what the work made of it.
    std::vector<OrcaMCP::PaintTint>           tints;
    std::vector<OrcaMCP::PaintVolumeInput>    paint_inputs;
    OrcaMCP::PaintResult                      paint_result;
    bool                                      paint_done = false;             // the work completed (else the call's wait ran out)

    // section: the plane worked out in the first step (from "app" or a ratio, the 3D view's), the instances it cuts as
    // the first step captured them, and what the work made of them.
    OrcaMCP::SectionRequest                   section;                        // as asked, with the app's normal and ratio filled in
    OrcaMCP::SectionPlane                     section_plane;
    std::vector<OrcaMCP::SectionBody>         section_bodies;
    std::vector<OrcaMCP::SectionCap>          section_caps;
    bool                                      section_done = false;           // the work completed (else the call's wait ran out)
};

class OrcaMCPPlateUtils {
public:
    // Main thread. Reads and checks `payload` (render_plate_view's arguments) and captures the scene into `job`.
    // Returns the answer when the call is answered here (a layer view, which is drawn on the CPU), else null.
    // Throws std::runtime_error with what to send instead for a request it refuses.
    static nlohmann::json PrepareRender(const nlohmann::json& payload, RenderJob& job);
    // HTTP thread. Runs job.work, each piece stopping at `stop`.
    static void ComputeRender(RenderJob& job, const std::function<bool()>& stop);
    // Main thread. Draws every view and answers; an error answer when the scene changed since PrepareRender.
    static nlohmann::json DrawRender(RenderJob& job);
    // `mesh_health` is model_mesh_health(model), read once for the call: every object entry's
    // mesh_warning, and its features when asked for, come from it.
    static nlohmann::json GetCurrentProject(const bool with_model_object_features,
                                            const std::vector<OrcaMCP::MeshHealth>& mesh_health);
    // get_scene_info's response before the handler adds the open dialogs, the warnings and the next
    // steps: the bed (the current plate's area), `plates` and `unplaced_objects`.
    static nlohmann::json SceneJson(const std::string& hash_code, bool sequential_print, const BoundingBoxf3& bed,
                                    nlohmann::json plates, nlohmann::json unplaced_objects);
    // One entry of get_scene_info's `plates`.
    static nlohmann::json PlateJson(const PlateEntry& plate);
    // One entry of a plate's `model_objects`: model_object_summary_json of the instances `here`, its
    // brim and printed footprint (`footprint`, GetObjectFootprint), its variable layer height, its
    // filaments, and the mesh-health numbers when `with_features`.
    static nlohmann::json ScenePlateObjectJson(const ModelObject& object, int object_index, const OrcaMCP::InstancesOnPlate& here,
                                               const OrcaMCP::MeshHealth& health, const ObjectFootprint& footprint, bool with_features);
    // get_scene_info's unplaced_objects: every object with an instance no plate holds, and which.
    // Checking instance 0 alone hid the copy of an object on two plates whose second plate had lost it.
    static nlohmann::json UnplacedObjectsJson(const Model& model, PartPlateList& plates,
                                              const std::vector<OrcaMCP::MeshHealth>& mesh_health, bool with_features);
    // One unplaced_objects entry: the instances of an object no plate holds (`unplaced`), where they
    // are, and the mesh fields a plate's object entry carries -- mesh_warning (and its reason), and
    // features when `with_features`.
    static nlohmann::json UnplacedObjectJson(const ModelObject& object, int object_index, const OrcaMCP::InstancesOnPlate& unplaced,
                                             const OrcaMCP::MeshHealth& health, bool with_features);

    // Reads the prime tower's state on `plate_index`. `full_config` is passed in because building
    // it is the expensive part and the per-plate loop only needs one. With `measure_unprinted` false
    // a tower that is not printed is not measured (size, footprint and legal_range stay empty): the
    // slicer's estimate walks every instance's convex hull, which on a 1M-facet sphere took about
    // 0.6 s per plate at -O0, on every get_scene_info, for a tower that was never there.
    static PrimeTowerState GetPrimeTowerState(int plate_index, const DynamicPrintConfig& full_config,
                                              bool measure_unprinted = true);
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

    // The 3D view's canvas, whatever tab is showing, with a scene reload it postponed while hidden done
    // first, so its volumes are the model's. `scene_current` is false when the reload could not be done
    // (GTK/EGL may refuse a hidden canvas). Renders draw from it; flatten_object selects in it.
    static GLCanvas3D* SceneCanvas(bool& scene_current);

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

    static nlohmann::json GetPlates(bool with_model_object_features, const std::vector<OrcaMCP::MeshHealth>& mesh_health);
};

}} // namespace Slic3r::GUI

#endif
