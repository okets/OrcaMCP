#pragma once

// Meshes with known defects, and a one-part object that holds one, as a load leaves it: what the
// mesh-health tests and the tests of what a response suggests next are built on.

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>
#include <utility>

namespace mesh_fixtures {

// A closed 10 mm cube with its first facet removed: a triangular hole whose three edges each
// leave one neighbour facet with an open edge.
inline indexed_triangle_set cube_missing_facet()
{
    indexed_triangle_set its = Slic3r::its_make_cube(10.0, 10.0, 10.0);
    its.indices.erase(its.indices.begin());
    return its;
}

inline indexed_triangle_set translated(indexed_triangle_set its, const Slic3r::Vec3f& offset)
{
    for (Slic3r::Vec3f& v : its.vertices)
        v += offset;
    return its;
}

// `count` separate cubes in one mesh, each 1 mm and 3 mm from the last: `count` shells.
inline indexed_triangle_set separate_cubes(int count)
{
    indexed_triangle_set its;
    for (int i = 0; i < count; ++i)
        Slic3r::its_merge(its, translated(Slic3r::its_make_cube(1.0, 1.0, 1.0), Slic3r::Vec3f(3.f * i, 0.f, 0.f)));
    return its;
}

// A flat 10 mm square in the XY plane, two facets and no thickness: a stray sheet with four open
// edges and no volume.
inline indexed_triangle_set flat_square()
{
    indexed_triangle_set its;
    its.vertices = {{0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, {10.f, 10.f, 0.f}, {0.f, 10.f, 0.f}};
    its.indices  = {{0, 1, 2}, {0, 2, 3}};
    return its;
}

// A closed 10 mm cube at the origin with `stray` beside it, 20 mm along X, in one mesh: two shells.
inline indexed_triangle_set cube_with(indexed_triangle_set stray)
{
    indexed_triangle_set its = Slic3r::its_make_cube(10.0, 10.0, 10.0);
    Slic3r::its_merge(its, translated(std::move(stray), Slic3r::Vec3f(20.f, 0.f, 0.f)));
    return its;
}

// One object holding `mesh` as its only part, with one instance, as a load leaves it.
struct OnePartObject
{
    Slic3r::Model        model;
    Slic3r::ModelObject* object = nullptr;

    explicit OnePartObject(Slic3r::TriangleMesh&& mesh, const std::string& name = "Test object")
    {
        object       = model.add_object();
        object->name = name;
        object->add_volume(std::move(mesh));
        object->add_instance();
    }
};

} // namespace mesh_fixtures
