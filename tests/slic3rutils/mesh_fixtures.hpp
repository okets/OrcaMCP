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
