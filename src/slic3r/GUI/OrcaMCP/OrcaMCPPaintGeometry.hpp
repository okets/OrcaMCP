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

}}} // namespace Slic3r::GUI::OrcaMCP
