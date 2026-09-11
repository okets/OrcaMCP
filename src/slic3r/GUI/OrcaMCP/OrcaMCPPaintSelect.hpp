// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp
#pragma once
#include <array>
#include <vector>

#include <Eigen/Dense>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

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

// Unit normal of `facet`, mapped into the plate frame by the inverse-transpose of `to_plate`'s
// linear part, so a non-uniformly scaled instance still reports a normal perpendicular to the
// surface it actually has on the plate.
Vec3d facet_normal_plate(const indexed_triangle_set& its, int facet, const Transform3d& to_plate);

// Closest point on the mesh to `plate_point`. Builds an AABBMesh over the mesh for this call;
// on a multi-million-facet mesh that is the dominant cost, and it is pure CPU work the caller
// may run off the GUI thread. False for an empty mesh.
bool pick_nearest_point(const TriangleMesh& mesh,
                        const Transform3d&  to_plate,
                        const Vec3d&        plate_point,
                        SurfacePick&        out);

// First hit of the ray `origin_plate + t * dir_plate`, t > 0. False for an empty mesh, a zero
// direction, or a miss.
bool pick_ray(const TriangleMesh& mesh,
              const Transform3d&  to_plate,
              const Vec3d&        origin_plate,
              const Vec3d&        dir_plate,
              SurfacePick&        out);

}}} // namespace Slic3r::GUI::OrcaMCP
