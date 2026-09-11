// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp
#include "OrcaMCPPaintSelect.hpp"

#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

std::vector<int> facet_component_ids(const indexed_triangle_set& its, int& component_count)
{
    component_count = 0;
    std::vector<int> ids(its.indices.size(), -1);
    if (its.indices.empty())
        return ids;

    const std::vector<Vec3i32> neighbors = its_face_neighbors_par(its);
    std::vector<int>           stack;
    for (std::size_t seed = 0; seed < ids.size(); ++seed) {
        if (ids[seed] != -1)
            continue;
        const int id = component_count++;
        ids[seed]    = id;
        stack.push_back(int(seed));
        // Iterative flood fill: a 4-million-facet shell would overflow a recursive one.
        while (!stack.empty()) {
            const int facet = stack.back();
            stack.pop_back();
            for (int neighbor : neighbors[facet])
                if (neighbor >= 0 && ids[neighbor] == -1) {
                    ids[neighbor] = id;
                    stack.push_back(neighbor);
                }
        }
    }
    return ids;
}

std::vector<ComponentInfo> summarize_components(const indexed_triangle_set& its,
                                                const std::vector<int>&     ids,
                                                int                         component_count,
                                                const Transform3d&          to_plate)
{
    std::vector<ComponentInfo> out(std::size_t(std::max(component_count, 0)));
    for (int c = 0; c < component_count; ++c)
        out[c].component = c;

    for (std::size_t f = 0; f < its.indices.size() && f < ids.size(); ++f) {
        const int id = ids[f];
        if (id < 0 || id >= component_count)
            continue;
        const Vec3i32& face = its.indices[f];
        const Vec3d a = to_plate * its.vertices[face[0]].cast<double>();
        const Vec3d b = to_plate * its.vertices[face[1]].cast<double>();
        const Vec3d c = to_plate * its.vertices[face[2]].cast<double>();
        ComponentInfo& info = out[id];
        info.facet_count += 1;
        info.area += 0.5 * (b - a).cross(c - a).norm();
        info.bbox.merge(a);
        info.bbox.merge(b);
        info.bbox.merge(c);
    }
    return out;
}

FacetAssignment assign_component(const std::vector<int>& ids, int component, int state)
{
    FacetAssignment out;
    out.states.assign(ids.size(), -1);
    out.unassigned = int(ids.size());
    for (std::size_t f = 0; f < ids.size(); ++f)
        if (ids[f] == component) {
            out.states[f] = state;
            --out.unassigned;
        }
    return out;
}

}}} // namespace Slic3r::GUI::OrcaMCP
