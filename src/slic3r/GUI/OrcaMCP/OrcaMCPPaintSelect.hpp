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

}}} // namespace Slic3r::GUI::OrcaMCP
