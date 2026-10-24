#ifndef slic3r_OrcaMCPRenderSection_hpp_
#define slic3r_OrcaMCPRenderSection_hpp_

// render_plate_view's `section`: a plane that removes the side its normal points to, with the cut faces drawn and
// their areas measured. Pure: the request, the plane, and the cut of a body (slice, union, triangulate) on copies of
// shared meshes -- no GL, no wx, no scene -- so it is unit-tested in tests/slic3rutils/test_render_section.cpp. The
// resolution of "app" and ratio against the 3D view, and the drawing, are OrcaMCPPlateUtils.cpp's.

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A section as asked. Form::App is the cut the 3D view's section view shows now; PointNormal a plane through `point`;
// Ratio upstream's sweep along `normal` (0 = nothing cut, 1 = everything); Axis a plane square to x, y or z. `point` is
// in bed mm, or plate-local with plate_local (Axis x and y, and PointNormal; z is the same in both).
struct SectionRequest
{
    enum class Form { App, PointNormal, Ratio, Axis } form = Form::PointNormal;
    Vec3d  normal      = Vec3d::UnitZ();   // unit; the side it points to is removed
    Vec3d  point       = Vec3d::Zero();    // PointNormal: a point on the plane; Axis: axis * at_mm
    double ratio       = 0.;               // Ratio: in (0, 1]
    bool   plate_local = false;
};

// "app", {normal, point}, {normal, ratio} or {plane: "x"|"y"|"z", at_mm, remove?: "above"|"below"}, each with an
// optional frame ("bed_mm" default, "plate_local"; not with ratio). A section an answer returned is accepted back:
// its normal and point are the form, and source, removes, ratio_used, cap_color, applied, empty, objects, notes and
// warning are ignored. Throws std::runtime_error with what to send for anything else.
SectionRequest read_section(const nlohmann::json& section);

// The plane in the convention of the 3D view's ClippingPlane: a point p is removed where normal.dot(p) > offset.
struct SectionPlane
{
    Vec3d  normal = Vec3d::UnitZ();   // unit
    double offset = 0.;
    double signed_distance(const Vec3d& p) const { return normal.dot(p) - offset; }   // > 0: removed
};

// The plane through `point` (bed mm) with `normal` (any nonzero length).
SectionPlane plane_through_point(const Vec3d& normal, const Vec3d& point);

// The plane as the mcp_render shader takes it: a fragment with dot(world_pos, result) < 0 is discarded.
std::array<double, 4> shader_clipping_plane(const SectionPlane& plane);

// One piece of a body's solid in world mm: a model part (positive) or a negative volume (subtracted).
struct SectionPart
{
    std::shared_ptr<const TriangleMesh> mesh;
    Transform3d                         world = Transform3d::Identity();
    ObjectID                            identity;   // the volume's, for the scene-unchanged check
    int                                 volume_id = -1;
    bool                                negative  = false;
    bool                                open_edges = false;   // the mesh has open edges: slicing drops an open chain, so the cut may be short
};

// One instance of an object: what a section cuts together.
struct SectionBody
{
    int                      object_id   = -1;
    int                      instance_id = -1;
    ObjectID                 object_identity;
    std::vector<SectionPart> parts;
};

// How a plane meets a body.
enum class SectionEffect { Cut, KeepsAll, RemovesAll };
const char* section_effect_name(SectionEffect effect);

struct SectionCap
{
    int                  object_id   = -1;
    int                  instance_id = -1;
    SectionEffect        effect      = SectionEffect::KeepsAll;
    double               area_mm2    = 0.;
    int                  islands     = 0;
    indexed_triangle_set triangles;      // world mm, on the plane, facing the removed side
    bool                 open_edges  = false;   // a part of the body has open edges
    bool                 complete    = true;   // false: `stop` said so first
};

// The cut face of `body` at `plane`: every positive part sliced, unioned, minus the negative parts (what prints; the 3D
// view's own caps cut each model part on its own and subtract nothing), and only where the cut is at or above the bed
// (z >= SINKING_Z_THRESHOLD: what is below it is neither drawn nor printed), triangulated. Area in mm2 and the number of islands (outlines, each with its holes). Stops
// between parts.
SectionCap section_cap(const SectionBody& body, const SectionPlane& plane, const std::function<bool()>& stop);

// The answer's `section` for the plane: normal, point (the plane's closest point to the origin), source ("app",
// "ratio" or "plane"), removes, ratio_used for the app and ratio forms, cap_color, empty, objects
// [{object_id, section_area_mm2, islands, effect, instances?}] and notes (an empty cut, an object with open edges).
// `drawn` are the (object_id, instance_id) the picture shows; a body not in it is left out.
nlohmann::json section_json(const SectionRequest& asked, const SectionPlane& plane, const std::vector<SectionCap>& caps,
                            const std::vector<std::pair<int, int>>& drawn);

// True when `section` (a section_json) cut nothing because the plane removed every object it met.
bool section_removed_everything(const nlohmann::json& section);

// The box "app" and a ratio sweep along the normal, as the 3D view takes it: `current_plate` is its volumes_bounding_box(true)
// and `all_plates` its volumes_bounding_box() (either may be undefined), `instances` the boxes of the rendered plate's own
// instances. "app" is the view's: the current plate's volumes, else all of them (upstream's fallback when that plate is
// empty), else the rendered plate's instances. A ratio is the same for the current plate; on another plate it is that plate's
// instances only (not its whole footprint). `plate_box` when nothing else is defined.
BoundingBoxf3 section_sweep_box(bool app_form, bool plate_is_current, const BoundingBoxf3& current_plate, const BoundingBoxf3& all_plates,
                                const std::vector<BoundingBoxf3>& instances, const BoundingBoxf3& plate_box);

// The cut faces' colour, as the 3D view draws them.
constexpr float k_section_cap_grey = 0.25f;

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
