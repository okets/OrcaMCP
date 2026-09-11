// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "OrcaMCPPaintGeometry.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Which of the four FacetsAnnotation members on a ModelVolume a call addresses. All four are the
// same type and the same machinery (Model.hpp:875-884: supported_facets, seam_facets,
// mmu_segmentation_facets, fuzzy_skin_facets); only the meaning of the per-facet state differs,
// which is why one tool with a mode parameter closes four gizmo-shaped gaps.
enum class PaintMode { Color, Support, Seam, FuzzySkin };

// "color" | "support" | "seam" | "fuzzy_skin", any case. Returns false and leaves `out` alone
// otherwise. Case-insensitive to match parse_paint_axis: both parsers are reached from the same
// MCP tool's parameters, and having them disagree about case costs a caller a wasted round trip.
// Brim ears are deliberately not a mode: they are BrimPoints on the ModelObject (Model.hpp:390),
// not facets, and have their own tool.
bool        parse_paint_mode(const std::string& name, PaintMode& out);
const char* paint_mode_name(PaintMode mode);

FacetsAnnotation&       annotation_for_mode(ModelVolume& mv, PaintMode mode);
const FacetsAnnotation& annotation_for_mode(const ModelVolume& mv, PaintMode mode);

// The largest state a facet can hold: int(EnforcerBlockerType::ExtruderMax), 32
// (TriangleSelector.hpp:31-32). A project with more filament slots than that cannot paint
// the ones above it, and the tool layer reports that rather than writing a state the
// slicer would clamp away in ModelVolume::update_extruder_count (Model.cpp:2641-2648).
int max_paint_state();

// The largest state `mode` itself can hold, which is the bound a write is validated against.
// Color addresses filament slots 1..max_paint_state(); Support and Seam only distinguish
// NONE/ENFORCER/BLOCKER; FuzzySkin only NONE/FUZZY_SKIN (TriangleSelector.hpp:19 aliases
// FUZZY_SKIN to ENFORCER, and GLGizmoFuzzySkin.hpp:29-30 paints nothing else). Centralised here
// because every caller that writes a state needs the same bound, and a copy of it per tool is
// how the four modes drift apart.
int max_paint_state_for(PaintMode mode);

// Raw EnforcerBlockerType value for a caller-supplied state name, for every mode except Color:
// "none" -> 0, "enforcer" -> 1, "blocker" -> 2. FuzzySkin also accepts "fuzzy_skin" as a synonym
// for "enforcer" (both -> 1) but rejects "blocker" (TriangleSelector.hpp:19 aliases FUZZY_SKIN to
// ENFORCER, and GLGizmoFuzzySkin.hpp:29-30 paints only FUZZY_SKIN and NONE); the synonym exists
// because paint_state_label(FuzzySkin, 1) reads back as "fuzzy_skin", and that token has to parse
// back to the same state or a caller who echoes a read straight into a write gets rejected.
// Color rejects every name: its states are filament slot numbers, which the tool takes as an
// integer, so a name there is a caller mistake worth reporting.
bool parse_paint_state(PaintMode mode, const std::string& name, int& out_state);

// How a raw state reads back for a given mode. Color: "unpainted" / "filament <n>", or
// "state <n>" for n above max_paint_state() -- a value the slicer could not have produced, so it
// is reported as out of domain rather than as a plausible-looking filament slot.
// Support and Seam: "none" / "enforcer" / "blocker". FuzzySkin: "none" / "fuzzy_skin".
// Every label parse_paint_state can consume round-trips back to the same state for its mode.
std::string paint_state_label(PaintMode mode, int state);

// instance.get_matrix() * volume.get_matrix() (Model.hpp:1354 and Model.hpp:1013): volume-local
// mesh coordinates -> plate coordinates. This is the same composition
// ModelObject::instance_bounding_box uses (Model.cpp:1685-1697), which is why the result lines up
// with the bounding_box get_object_info reports. An out-of-range or absent instance falls back to
// instance 0; an object with no instance at all yields the volume matrix alone.
Transform3d volume_to_plate(const ModelObject& obj, const ModelVolume& mv, std::size_t instance_idx);

// Where a stored BrimPoint reads back in the plate frame the rest of this API speaks.
// ModelObject::brim_points is stored object-local (Model.hpp ~390); Brim.cpp:373-374 transforms
// each by the instance matrix to get a world position and discards any whose world z ends up
// above 0. An out-of-range or absent instance falls back to instance 0, the same way
// volume_to_plate does, so two halves of one response are never read in different frames.
Vec3d brim_point_to_plate(const ModelObject& obj, const Vec3f& local_pos, std::size_t instance_idx);

// The inverse of the above, carrying the one rule GLGizmoBrimEars.cpp (~395-402) applies when a
// user places a point: an ear always sits on the object's underside, so world z is pinned to
// -0.0001 -- just below 0, the side Brim.cpp:373-374 keeps -- before converting to the
// object-local frame brim_points are stored in. Round-trips with brim_point_to_plate for the
// same instance.
Vec3f brim_point_to_object(const ModelObject& obj, double plate_x, double plate_y, std::size_t instance_idx);

// Writes `states` (one raw EnforcerBlockerType value per facet of mv.mesh(), -1 = leave alone)
// into the annotation `mode` selects, the same way GLGizmoMmuSegmentation::update_model_object
// does (GLGizmoMmuSegmentation.cpp, update_model_object): drive a TriangleSelector with set_facet,
// then hand it to FacetsAnnotation::set. Going through the selector rather than writing the
// bitstream directly is what makes MCP-painted data byte-identical to gizmo-painted data, so it
// renders in the gizmo and round-trips through 3MF unchanged.
// `replace` starts from a blank selector, discarding whatever the volume already carried for
// this mode; otherwise the existing paint is the base and only the listed facets move.
//
// Returns false for two unrelated reasons, which the bool alone cannot tell apart:
//   * the call was rejected and the annotation was not touched at all -- `states` is not exactly
//     one entry per facet, the mesh has no facets, or some entry is outside
//     [-1, max_paint_state_for(mode)];
//   * the call was applied but the annotation already held exactly this (FacetsAnnotation::set's
//     own return).
// A caller that must distinguish the two validates the size and the range itself first; the tool
// layer does, so that it can report which entry was wrong.
//
// Rejecting rather than clamping is deliberate. `states` comes from a FacetAssignment, which is
// sized to the facet count by construction, so a short vector is always a caller bug; painting
// its prefix would leave a half-painted model that looks deliberate. And EnforcerBlockerType is
// an int8_t whose values are bit-packed by the serializer, so an out-of-range state is stored raw
// here and silently clamped much later, far from the call that caused it.
bool apply_facet_states(ModelVolume& mv, PaintMode mode, const std::vector<int>& states, bool replace);

// One state present on one volume.
struct PaintedStateInfo
{
    int    state       = 0;
    // Leaf triangles at this state. A facet a gizmo split earlier counts more than once, which
    // is why area_ratio and not this number is the honest measure of how much is painted.
    int    facet_count = 0;
    // 0..1 of the volume's total mesh area. A ratio, so a scaled instance reports the same value.
    double area_ratio  = 0.0;
};

// Every state carrying at least one facet, ascending by state, state 0 (unpainted) included so a
// caller can see how much of the volume is still bare. An unpainted volume returns exactly one
// entry, {0, all facets, 1.0}.
std::vector<PaintedStateInfo> read_volume_paint(const ModelVolume& mv, PaintMode mode);

// True when the annotation `mode` selects carries data -- i.e. exactly when clear_volume_paint
// would change something. Lets a caller decide whether a clear is worth an undo snapshot before
// it mutates anything, which is the only order in which that question can be asked: a snapshot
// has to be taken before the write it protects.
bool has_volume_paint(const ModelVolume& mv, PaintMode mode);

// FacetsAnnotation::reset() on the member `mode` selects, leaving the other three alone.
// Returns false when the annotation was already empty, so a caller can tell "cleared" from
// "there was nothing to clear".
bool clear_volume_paint(ModelVolume& mv, PaintMode mode);

}}} // namespace Slic3r::GUI::OrcaMCP
