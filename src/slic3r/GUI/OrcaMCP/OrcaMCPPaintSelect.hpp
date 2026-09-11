// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp
#pragma once
#include <array>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "OrcaMCPPaintGeometry.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// ---- Connected components -------------------------------------------------------------------
//
// A facet's component id, one per facet in facet order. Two facets share a component when a path
// of shared edges joins them (its_face_neighbors_par, TriangleMesh.hpp:201; a negative neighbour
// is an open edge). Ids are assigned in discovery order over ascending facet index, so the same
// mesh always yields the same ids: an id read from get_object_components addresses the same shell
// in a later paint_object call. `component_count` receives the number of ids handed out.
std::vector<int> facet_component_ids(const indexed_triangle_set& its, int& component_count);

struct ComponentInfo
{
    int           component   = 0;
    int           facet_count = 0;
    // Surface area in the frame `to_plate` maps into -- plate mm^2 for the callers here.
    double        area        = 0.0;
    BoundingBoxf3 bbox;
};

// One entry per component, ascending by component id, each with its facet count, area and
// bounding box computed from the facets' plate-frame vertices.
std::vector<ComponentInfo> summarize_components(const indexed_triangle_set& its,
                                                const std::vector<int>&     ids,
                                                int                         component_count,
                                                const Transform3d&          to_plate);

// `state` for every facet whose id is `component`, -1 (leave alone) for every other facet. An id
// no facet carries selects nothing -- reported through `unassigned`, never widened to "everything".
FacetAssignment assign_component(const std::vector<int>& ids, int component, int state);

// ---- Picking --------------------------------------------------------------------------------

// Where a point or a ray met the surface. `facet` is the original facet index -- the index
// TriangleSelector::set_facet and seed_fill_select_triangles take -- or -1 when nothing was hit.
struct SurfacePick
{
    int    facet        = -1;
    Vec3d  point_local  = Vec3d::Zero();   // mesh coordinates, the frame TriangleSelector wants
    Vec3d  point_plate  = Vec3d::Zero();   // the same point in plate millimetres
    Vec3d  normal_plate = Vec3d::Zero();   // unit facet normal in the plate frame
    double distance     = 0.0;             // plate mm from the query point / ray origin
};

// True when `to_plate`'s linear part can be inverted, which every pick needs twice: once to carry
// the caller's plate coordinates into the mesh frame, and once (as its inverse transpose) to carry
// a facet normal back out. scale_object accepts a scale of 0, so an object really can be sitting
// on the plate under a rank-deficient transform; without this test the inverse comes back full of
// infinities and the pick reports a NaN point and normal -- which nlohmann serialises as null --
// under status "success".
bool plate_transform_is_invertible(const Transform3d& to_plate);

// Unit normal of `facet`, mapped into the plate frame by the inverse-transpose of `to_plate`'s
// linear part, so a non-uniformly scaled instance still reports a normal perpendicular to the
// surface it actually has on the plate. Zero for a degenerate facet, and for a `to_plate`
// plate_transform_is_invertible rejects.
Vec3d facet_normal_plate(const indexed_triangle_set& its, int facet, const Transform3d& to_plate);

// Closest point on the mesh to `plate_point`. Builds an AABBMesh over the mesh for this call;
// on a multi-million-facet mesh that is the dominant cost, and it is pure CPU work the caller
// may run off the GUI thread. False for an empty mesh, or a `to_plate`
// plate_transform_is_invertible rejects.
bool pick_nearest_point(const TriangleMesh& mesh,
                        const Transform3d&  to_plate,
                        const Vec3d&        plate_point,
                        SurfacePick&        out);

// First hit of the ray `origin_plate + t * dir_plate`, t > 0. False for an empty mesh, a zero
// direction, a `to_plate` plate_transform_is_invertible rejects, or a miss.
bool pick_ray(const TriangleMesh& mesh,
              const Transform3d&  to_plate,
              const Vec3d&        origin_plate,
              const Vec3d&        dir_plate,
              SurfacePick&        out);

// ---- Camera ---------------------------------------------------------------------------------

// What render_plate_view reports per view and pick_facet takes back: the two 4x4 matrices the
// thumbnail was drawn with and the viewport {x, y, width, height} in pixels. Pixel (0, 0) is the
// TOP-left of the saved image: OrcaMCPPlateUtils.cpp:63,105 flip the GL buffer row-wise when
// writing it, so the convention here is the image's, not OpenGL's.
struct CameraFrame
{
    Eigen::Matrix4d    view       = Eigen::Matrix4d::Identity();
    Eigen::Matrix4d    projection = Eigen::Matrix4d::Identity();
    std::array<int, 4> viewport   = {0, 0, 1, 1};
};

// The world-space ray through pixel (px, py). `origin` lies on the near plane and `dir` is a unit
// vector. False when the viewport has no area or projection * view cannot be inverted.
bool unproject_pixel_to_ray(const CameraFrame& camera, double px, double py, Vec3d& origin, Vec3d& dir);

// ---- The camera JSON, both directions --------------------------------------------------------
//
// render_plate_view emits this object per view; pick_facet reads the same object back. The two
// directions live together deliberately: if one of them renamed a key or transposed a matrix,
// nothing would fail -- the caller would get a confident answer about the wrong triangle. The
// round-trip test in tests/slic3rutils/test_paint_select.cpp is what holds them to each other.

// `m`'s sixteen elements, row-major: out[r * 4 + c] is m(r, c). Exactly what parse_matrix4 reads.
nlohmann::json matrix4_to_json(const Eigen::Matrix4d& m);

// The three keys parse_camera_frame requires: view_matrix, projection_matrix, viewport.
// render_plate_view adds informational keys of its own (type, pixel_origin, camera_position,
// target); the parser ignores everything it does not require, so those are free to change.
nlohmann::json camera_frame_to_json(const CameraFrame& camera);

// Sixteen row-major numbers into `out`. `what` names the field in `error`, since a caller that
// mistyped one element needs to be told which field, not just that a matrix was wrong.
bool parse_matrix4(const nlohmann::json& value, const char* what, Eigen::Matrix4d& out, std::string& error);

// A `camera` object as camera_frame_to_json wrote it. Extra keys are tolerated; a missing
// required key is refused with a message naming all three.
bool parse_camera_frame(const nlohmann::json& value, CameraFrame& out, std::string& error);

// ---- Seed fill ------------------------------------------------------------------------------

// The gizmo's own default (GLGizmoPainterBase.hpp:279, m_smart_fill_angle = 30.f).
constexpr double kDefaultSeedFillAngleDeg = 30.0;

// The connected region reachable from `seed_facet` without crossing an edge whose dihedral angle
// exceeds `angle_deg` -- TriangleSelector::seed_fill_select_triangles (TriangleSelector.hpp:332),
// the same traversal the GUI's smart-fill mode runs, read back per original facet. `state` is
// written to every facet the fill reached, -1 to every other. `seed_point_local` is the mesh-local
// point the fill starts from (SurfacePick::point_local); it need only lie on `seed_facet`.
// Out-of-range `seed_facet`, or an empty mesh, selects nothing.
FacetAssignment assign_connected(const TriangleMesh& mesh,
                                 const Transform3d&  to_plate,
                                 int                 seed_facet,
                                 const Vec3f&        seed_point_local,
                                 double              angle_deg,
                                 int                 state);

}}} // namespace Slic3r::GUI::OrcaMCP
