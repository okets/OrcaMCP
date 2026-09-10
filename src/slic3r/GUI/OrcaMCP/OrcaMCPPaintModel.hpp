// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp
#pragma once
#include <cstddef>
#include <string>

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

}}} // namespace Slic3r::GUI::OrcaMCP
