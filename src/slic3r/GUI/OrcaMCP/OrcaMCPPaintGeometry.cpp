// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.cpp
#include "OrcaMCPPaintGeometry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include <tbb/parallel_for.h>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

bool parse_paint_axis(const std::string& name, PaintAxis& out)
{
    if (name.size() != 1)
        return false;
    switch (std::tolower(static_cast<unsigned char>(name[0]))) {
    case 'x': out = PaintAxis::X; return true;
    case 'y': out = PaintAxis::Y; return true;
    case 'z': out = PaintAxis::Z; return true;
    default:  return false;
    }
}

const char* paint_axis_name(PaintAxis axis)
{
    switch (axis) {
    case PaintAxis::X: return "x";
    case PaintAxis::Y: return "y";
    case PaintAxis::Z: return "z";
    }
    return "z";
}

std::vector<PaintBand> make_even_bands(const std::vector<int>& states, double axis_min, double axis_max)
{
    std::vector<PaintBand> bands;
    if (states.empty() || !(axis_max > axis_min))
        return bands;

    const double span = axis_max - axis_min;
    const double n    = double(states.size());
    bands.reserve(states.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        PaintBand band;
        band.state = states[i];
        // Interpolate each boundary from the ends rather than accumulating a width, so
        // band[i].to and band[i+1].from are bit-identical and the last `to` is exactly axis_max.
        band.from = i == 0 ? axis_min : axis_min + span * (double(i) / n);
        band.to   = i + 1 == states.size() ? axis_max : axis_min + span * (double(i + 1) / n);
        bands.push_back(band);
    }
    return bands;
}

int band_index_for_value(const std::vector<PaintBand>& bands, double value)
{
    for (std::size_t i = 0; i < bands.size(); ++i)
        if (value >= bands[i].from && value < bands[i].to)
            return int(i);

    // Nothing matched. The far end of the painted range is the one closed boundary in the set,
    // so a facet centroid sitting exactly on it belongs to the band that ends there.
    int    best_idx = -1;
    double best_to  = 0.0;
    for (std::size_t i = 0; i < bands.size(); ++i)
        if (best_idx < 0 || bands[i].to > best_to) {
            best_idx = int(i);
            best_to  = bands[i].to;
        }
    if (best_idx >= 0 && std::abs(value - best_to) <= 1e-9)
        return best_idx;
    return -1;
}

bool paint_box_is_valid(const PaintBox& box)
{
    return box.min.x() <= box.max.x() && box.min.y() <= box.max.y() && box.min.z() <= box.max.z();
}

bool point_in_box(const PaintBox& box, const Vec3d& p)
{
    return p.x() >= box.min.x() && p.x() <= box.max.x() &&
           p.y() >= box.min.y() && p.y() <= box.max.y() &&
           p.z() >= box.min.z() && p.z() <= box.max.z();
}

bool point_in_sphere(const PaintSphere& sphere, const Vec3d& p)
{
    // Compare squared lengths so a zero radius needs no special case and no sqrt is taken
    // once per facet of a mesh that can carry hundreds of thousands of them.
    return (p - sphere.center).squaredNorm() <= sphere.radius * sphere.radius;
}

std::vector<Vec3d> facet_centroids(const indexed_triangle_set& its, const Transform3d& to_plate)
{
    std::vector<Vec3d> centroids(its.indices.size());
    // One matrix multiply per facet, embarrassingly parallel. On the 4.3-million-facet meshes a
    // generated figurine arrives with (T10), the serial loop was a measurable part of a
    // three-minute paint; the idiom is CutSurface.cpp:811-818.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, its.indices.size()),
        [&its, &to_plate, &centroids](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                const Vec3i32& face = its.indices[i];
                const Vec3d a = its.vertices[face[0]].cast<double>();
                const Vec3d b = its.vertices[face[1]].cast<double>();
                const Vec3d c = its.vertices[face[2]].cast<double>();
                // Transform the centroid rather than the three vertices: the transform is affine,
                // so the two agree, and this is one matrix multiply per facet instead of three.
                centroids[i] = to_plate * ((a + b + c) / 3.0);
            }
        });
    return centroids;
}

double its_surface_area(const indexed_triangle_set& its)
{
    double area = 0.0;
    for (const Vec3i32& face : its.indices) {
        const Vec3d a = its.vertices[face[0]].cast<double>();
        const Vec3d b = its.vertices[face[1]].cast<double>();
        const Vec3d c = its.vertices[face[2]].cast<double>();
        area += 0.5 * (b - a).cross(c - a).norm();
    }
    return area;
}

namespace {

// Shared shape for the three predicate-driven selections: every facet the predicate accepts
// gets `state`, every other facet is left to whatever it already was.
template<typename Predicate>
FacetAssignment assign_by_predicate(const std::vector<Vec3d>& centroids, int state, Predicate&& inside)
{
    FacetAssignment assignment;
    assignment.states.assign(centroids.size(), -1);
    for (std::size_t i = 0; i < centroids.size(); ++i) {
        if (inside(centroids[i]))
            assignment.states[i] = state;
        else
            ++assignment.unassigned;
    }
    return assignment;
}

} // namespace

FacetAssignment assign_bands(const std::vector<Vec3d>&     centroids,
                             PaintAxis                     axis,
                             const std::vector<PaintBand>& bands)
{
    FacetAssignment assignment;
    assignment.states.assign(centroids.size(), -1);
    assignment.band_counts.assign(bands.size(), 0);

    const int row = int(axis);
    for (std::size_t i = 0; i < centroids.size(); ++i) {
        const int band = band_index_for_value(bands, centroids[i][row]);
        if (band < 0) {
            ++assignment.unassigned;
            continue;
        }
        assignment.states[i] = bands[std::size_t(band)].state;
        ++assignment.band_counts[std::size_t(band)];
    }
    return assignment;
}

FacetAssignment assign_box(const std::vector<Vec3d>& centroids, const PaintBox& box, int state)
{
    return assign_by_predicate(centroids, state, [&box](const Vec3d& p) { return point_in_box(box, p); });
}

FacetAssignment assign_sphere(const std::vector<Vec3d>& centroids, const PaintSphere& sphere, int state)
{
    return assign_by_predicate(centroids, state,
                               [&sphere](const Vec3d& p) { return point_in_sphere(sphere, p); });
}

FacetAssignment assign_all(std::size_t facet_count, int state)
{
    FacetAssignment assignment;
    assignment.states.assign(facet_count, state);
    return assignment;
}

}}} // namespace Slic3r::GUI::OrcaMCP
