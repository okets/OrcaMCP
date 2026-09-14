#ifndef slic3r_GUI_OrcaMCPPlateOccupancy_hpp_
#define slic3r_GUI_OrcaMCPPlateOccupancy_hpp_

#include <string>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

// Everything on this page is pure arithmetic over rectangles, deliberately free of wxWidgets and of
// the Plater, so tests/slic3rutils/test_plate_occupancy.cpp can reach all of it headlessly. The
// wx-dependent half -- reading the configs, walking the plates, writing the answer into JSON --
// lives in OrcaMCPPlateUtils.cpp and OrcaMCPServer.cpp and is not reachable from a test.
//
// The problem this exists to solve: an agent asked get_scene_info for the occupants of a plate, got
// back only the model objects, computed the free bands correctly, moved a part into one of them and
// was told "Prime Tower is too close to others". The prime tower is printed plastic standing on the
// bed; so is an object's brim; a bed exclusion area is unusable bed either way. None of the three
// were reported, so the occupancy list an agent reasoned over was incomplete and the conclusions it
// drew from it were wrong in a way it could not see.
//
// Every rectangle here is in the SAME frame as the one it is compared against. The callers work in
// plate millimetres -- the world frame get_object_info and get_scene_info report object bounding
// boxes in, where plate 2 of a four-plate project genuinely sits several hundred millimetres along
// +X from plate 1. The one exception is prime_tower_position_range(), which is plate-LOCAL because
// the config key it describes (wipe_tower_x) is; its doc comment says so again.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// ---- Footprints -------------------------------------------------------------------------------

// The shadow `bbox` casts on the bed.
BoundingBoxf footprint_of(const BoundingBoxf3& bbox);

// `box` grown by `margin` mm on all four sides -- the printed skirt or brim around a body. A
// negative margin is treated as zero rather than shrinking the box: no brim setting means "the
// printed thing is smaller than the model", and quietly returning a too-small occupied area is the
// exact failure this file exists to prevent.
BoundingBoxf expand_footprint(const BoundingBoxf& box, double margin);

// The prime tower's occupied rectangle, brim included. `corner` is the tower body's front-left
// corner (min x, min y) in whatever frame the answer is wanted; `size` is the body's width and
// depth; `brim_width` is the resolved prime_tower_brim_width, which prints outside the body on all
// four sides.
BoundingBoxf prime_tower_footprint(const Vec2d& corner, const Vec2d& size, double brim_width);

// True when `footprint` lies entirely inside `plate`. Touching an edge counts as inside: a tower
// clamped exactly onto the limit prime_tower_position_range() reports must not then be called out
// of bounds by the check that follows.
bool footprint_within(const BoundingBoxf& footprint, const BoundingBoxf& plate);

// True when the two rectangles share area. Touching edges do not overlap -- two parts placed flush
// against each other are a slicing question, not a containment error, and reporting every shared
// edge as a collision would bury the real ones.
bool footprints_overlap(const BoundingBoxf& a, const BoundingBoxf& b);

// ---- Where the prime tower may stand ----------------------------------------------------------

// The range PartPlate::estimate_wipe_tower_polygon clamps the stored wipe_tower_x / wipe_tower_y
// into, in PLATE-LOCAL millimetres (0,0 is the plate's front-left corner, which is what those two
// config keys are measured from). Reproduced here rather than re-derived, so a position this says
// is legal is one the estimator will keep rather than silently move.
//
// The two margins are separate because upstream's are: the low edge and the far-edge margin use the
// RAW prime_tower_brim_width config value, which is negative when the brim is automatic, while the
// far edge subtracts the RESOLVED brim on top of that (PartPlate.cpp, estimate_wipe_tower_polygon).
// `margin` is WIPE_TOWER_MARGIN + the raw value; `brim_width` is the resolved one.
//
// `fits` is false when the tower plus its margins is wider or deeper than the plate, in which case
// min_* and max_* are still returned (max < min) so a caller can say by how much it misses.
struct PrimeTowerRange
{
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;
    bool   fits  = false;
};

PrimeTowerRange prime_tower_position_range(double plate_width, double plate_depth,
                                           double tower_width, double tower_depth,
                                           double margin, double brim_width);

// ---- Will a prime tower be printed on this plate at all? --------------------------------------

// The inputs GLCanvas3D::reload_scene decides by when it chooses whether to build a wipe-tower
// volume for a plate. Mirrored -- not guessed -- so "no tower here" from get_scene_info means the
// same thing as "no tower drawn in the 3D view".
struct PrimeTowerConditions
{
    bool is_fff                      = true;
    bool enable_prime_tower          = false;  // print config, enable_prime_tower
    bool timelapse_smooth            = false;  // print config, timelapse_type == tlSmooth
    bool wrapping_detection          = false;  // print config, enable_wrapping_detection
    int  project_filament_count      = 1;      // project config, filament_colour size
    bool gcode_only_mode             = false;  // a G-code-only view has no tower to place
    // Per plate:
    bool plate_has_objects           = false;
    int  plate_filament_count        = 0;      // PartPlate::get_extruders(true).size()
    bool plate_sequential            = false;  // this plate prints by object
    int  plate_printable_instances   = 0;
};

enum class PrimeTowerVerdict
{
    Printed,
    NotFff,
    Disabled,
    SingleFilamentProject,
    GcodeOnlyMode,
    SequentialMultiObject,
    EmptyPlate,
    SingleFilamentPlate,
};

PrimeTowerVerdict prime_tower_verdict(const PrimeTowerConditions& conditions);

// A stable machine-readable token for the verdict ("printed", "single_filament_plate", ...), for
// the `reason` field of the JSON.
const char* prime_tower_verdict_token(PrimeTowerVerdict verdict);

// One sentence saying why, for a human reading the response.
const char* prime_tower_verdict_explanation(PrimeTowerVerdict verdict);

// ---- Object brim ------------------------------------------------------------------------------

// How far past a model object's outline its brim is printed. `brim_type` is the serialized
// brim_type enum ("auto_brim", "outer_only", "no_brim", ...).
//
// `exact` is false whenever the slicer decides the real width itself at slice time: auto_brim
// recomputes it per volume group (Brim.cpp, configBrimWidthByVolumeGroups), and brim ears and
// painted brims only touch part of the outline. `upper_bound_mm` is then the widest the brim can
// get -- the auto-brim formula is capped at 18 mm -- so a caller that would rather over-reserve bed
// than collide has a number to use. When `exact` is true the two are equal.
//
// btInnerOnly is 0 outside the object on purpose: an inner brim is printed in the object's holes
// and does not reach past its outline at all.
struct ObjectBrimExtent
{
    double extent_mm      = 0.0;
    double upper_bound_mm = 0.0;
    bool   exact          = true;
};

// The cap the auto-brim width formula is clamped to in Brim.cpp.
constexpr double kAutoBrimWidthCapMm = 18.0;

ObjectBrimExtent object_brim_extent(const std::string& brim_type, double brim_width, double brim_object_gap);

// ---- Camera ------------------------------------------------------------------------------------

// The up vector to hand Camera::look_at for this view direction.
//
// look_at builds its basis as up.cross(view_direction).normalized(). Eigen's normalized() returns
// the vector unchanged when its norm is zero, so a camera looking straight down with up = +Z does
// not fail -- it produces an all-zero 3x3 basis and a view matrix that still renders a perfectly
// normal-looking image. The damage shows up later: pick_facet cannot invert that matrix, so the
// see-then-point loop silently breaks for the plan view an agent asks for first.
//
// Returns `preferred_up` unless the view direction is within a hair of parallel to it, and a
// perpendicular axis otherwise (+Y for a vertical view, the usual convention; +Z for the rest). A
// camera sitting exactly on its target has no view direction at all and gets `preferred_up` back.
Vec3d stable_camera_up(const Vec3d& camera_position, const Vec3d& target, const Vec3d& preferred_up = Vec3d::UnitZ());

}}} // namespace Slic3r::GUI::OrcaMCP

#endif // slic3r_GUI_OrcaMCPPlateOccupancy_hpp_
