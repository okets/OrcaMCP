// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintGeometry.hpp
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Which plate axis a band selection runs along. The value is the row of a Vec3d it indexes.
// Every coordinate in this header is in PLATE millimetres -- the frame get_object_info reports
// its bounding_box in -- never volume-local mesh coordinates.
enum class PaintAxis { X = 0, Y = 1, Z = 2 };

// "x" / "y" / "z", any case. Returns false and leaves `out` untouched for anything else.
bool parse_paint_axis(const std::string& name, PaintAxis& out);

// The lowercase name parse_paint_axis accepts, for echoing an axis back in a response.
const char* paint_axis_name(PaintAxis axis);

// One band along an axis, in plate millimetres. `state` is the raw EnforcerBlockerType value
// written to every facet whose centroid falls in the band: for mmu_segmentation that is the
// 1-based filament slot (TriangleSelector.hpp:23-32 numbers Extruder1..Extruder32 as 1..32),
// for the other three annotations it is 0 (none), 1 (enforcer) or 2 (blocker).
struct PaintBand
{
    int    state = 0;
    double from  = 0.0;
    double to    = 0.0;
};

// `states.size()` equal-width bands covering [axis_min, axis_max], in the order given.
// Boundaries are computed as axis_min + span * i / n rather than by repeated addition, so
// adjacent bands share a boundary exactly and the last band's `to` is exactly axis_max. Repeated
// addition would leave sub-micron gaps that facets fall through, and the failure would look like
// a handful of randomly unpainted triangles.
// Returns an empty vector when `states` is empty or axis_max <= axis_min. It does not decide what
// the caller should be told about that -- the tool layer turns an empty result into an error.
// Every band but the last is half-open [from, to); the last is closed [from, to] so axis_max
// itself belongs to a band instead of falling off the end of the range.
std::vector<PaintBand> make_even_bands(const std::vector<int>& states, double axis_min, double axis_max);

// Index of the band `value` belongs to, or -1 when it belongs to none.
// Bands are half-open [from, to), so a value sitting exactly on the boundary between two adjacent
// bands lands in the upper one and never in both. The one exception is the largest `to` in the
// set: a value there (within 1e-9) lands in that band, because otherwise every facet on the far
// face of the object would go unpainted. Bands need not tile and need not be sorted; overlapping
// bands are a caller's choice rather than an error, and the first match wins.
int band_index_for_value(const std::vector<PaintBand>& bands, double value);

// An axis-aligned box in plate millimetres.
struct PaintBox
{
    Vec3d min = Vec3d::Zero();
    Vec3d max = Vec3d::Zero();
};

// A sphere in plate millimetres.
struct PaintSphere
{
    Vec3d  center = Vec3d::Zero();
    double radius = 0.0;
};

// min <= max on every axis. A box that is flat on one axis is valid (it selects the facets whose
// centroid lies in that plane); a box whose min exceeds its max is a caller mistake, and the tool
// layer reports it rather than treating it as an empty selection.
bool paint_box_is_valid(const PaintBox& box);

// Inclusive on every face: a facet centroid exactly on a boundary is inside. Excluding it would
// silently drop the facets a caller most obviously meant to include when the box is the object's
// own bounding box.
bool point_in_box(const PaintBox& box, const Vec3d& p);

// Inclusive of the surface.
bool point_in_sphere(const PaintSphere& sphere, const Vec3d& p);

// Centroid of every facet of `its`, in the frame `to_plate` maps the mesh into -- for this
// feature always instance.get_matrix() * volume.get_matrix(), i.e. plate millimetres.
// Index i of the result is facet i of `its`, which is the same index
// TriangleSelector::set_facet takes (TriangleSelector.hpp:361), so the two line up directly.
// A facet belongs to the band or region containing its centroid: one facet, one answer, no
// partially painted triangles and no subdivision.
std::vector<Vec3d> facet_centroids(const indexed_triangle_set& its, const Transform3d& to_plate);

// Total area of `its` in the mesh's own units. Used only as the denominator of a coverage
// ratio, so the units cancel and a scaled instance reports the same coverage as an unscaled one.
double its_surface_area(const indexed_triangle_set& its);

// What a selection resolved to, per facet of the volume's mesh.
struct FacetAssignment
{
    // One entry per facet, in facet order. -1 means "this selection does not cover this facet",
    // which the writer leaves at whatever state it already had.
    std::vector<int> states;
    // Facets that landed in each band, parallel to the `bands` argument. Empty for the region
    // and whole-volume selections, which have no bands to count.
    std::vector<int> band_counts;
    // Facets that landed in no band, or outside the region. Reported so a caller can tell an
    // empty selection from a selection whose coordinates missed the object.
    int              unassigned = 0;
};

// Assigns bands[k].state to every facet whose centroid's `axis` component falls in bands[k],
// per band_index_for_value's rules (half-open except the last, first match wins on overlap).
// `centroids` and the returned `states` line up index for index with the facets they came from.
FacetAssignment assign_bands(const std::vector<Vec3d>&     centroids,
                             PaintAxis                     axis,
                             const std::vector<PaintBand>& bands);
// Assigns `state` to every facet whose centroid lies in `box`, per point_in_box's inclusive rule.
FacetAssignment assign_box(const std::vector<Vec3d>& centroids, const PaintBox& box, int state);
// Assigns `state` to every facet whose centroid lies in `sphere`, per point_in_sphere's inclusive rule.
FacetAssignment assign_sphere(const std::vector<Vec3d>& centroids, const PaintSphere& sphere, int state);
// Assigns `state` to every facet of a `facet_count`-facet mesh: the whole-volume selection.
FacetAssignment assign_all(std::size_t facet_count, int state);

}}} // namespace Slic3r::GUI::OrcaMCP
