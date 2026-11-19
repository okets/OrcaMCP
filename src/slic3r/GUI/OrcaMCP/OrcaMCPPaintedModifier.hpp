// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintedModifier.hpp
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

// The decisions of the painted-modifier forms of the paint and part tools apart from the app, so they
// are tested without it (tests/slic3rutils/test_painted_modifier_paint.cpp). A painted modifier
// (ModelVolumeType::PAINTED_MODIFIER, upstream) is a volume that shares the mesh and placement of the
// model part it is painted on and applies its own settings to that painted region; its paint is the
// volume's own FacetsAnnotation (painted_modifier_facets), so the PaintMode::PaintedModifier writes go
// through the same machinery as the other modes. Which volume a call addresses, which host it paints
// over, what a depth value may be, and what a refusal says are all here; the tools (OrcaMCPPaintTools.cpp,
// OrcaMCPPartTools.cpp) run the object list's and the gizmo's own actions.

namespace Slic3r {
class ModelObject;
class ModelVolume;
enum class ModelVolumeType : int;
namespace GUI { namespace OrcaMCP {

// Every painted modifier's volume_id (its index into object.volumes), ascending.
std::vector<int> painted_modifier_ids(const ModelObject& object);

// The volume_id of the model part a painted modifier is painted on, -1 when it has none (the part was
// deleted, or set_volume_type turned it into something that is not a part).
int host_volume_id(const ModelObject& object, const ModelVolume& painted_modifier);

// Make the object's painted modifiers follow the mesh and placement of their hosts, as the app does
// before every slice and save (ModelObject::sync_painted_modifiers): a host whose mesh changed (a
// repair, a reload, a bake) leaves its modifiers on the old facets, and a paint or a read there would
// address facets the host no longer has. Nothing happens when the object has none. Returns whether
// anything moved. Main thread; takes no undo step, as the app's own sync does.
bool ensure_painted_modifiers_synced(ModelObject& object);

// Why a `mode: painted_modifier` call cannot address `volume_id`, or nothing: it must name one of the
// object's painted modifiers, left out only when there is nothing to name (then the refusal is the
// call that creates one; with modifiers present, it is the volume_id to add). A modifier whose host is
// no longer a part is refused with the gizmo's own words for it, and the delete that removes it.
std::optional<std::string> painted_modifier_target_refusal(int object_id, const ModelObject& object, std::optional<int> volume_id);

// The `depth_mm` of a call, when it gives one: a number from 0 to 1000 inclusive (the gizmo's Depth
// field, BBLDragFloat ... 0.f, 1000.f); 0 reaches across the part to its nearest unpainted surface.
// Anything else is refused, never clamped; `out` holds the depth when the refusal is nothing.
std::optional<std::string> read_depth_mm(const nlohmann::json& params, std::optional<float>& out);

// add_volume's `type: painted_modifier` arguments: which part to paint a modifier over (the first
// model part, what the object list's Add Painted Modifier takes when nothing is given), and the depth.
struct PaintedModifierRequest
{
    int                 host_volume_id;
    std::optional<float> depth_mm;
};

// Read them, or the refusal: the host must be a model part (upstream's rule), `depth_mm` its range, and
// the type takes no `shape`, `size` or `file_path` -- it shares the host's mesh.
std::variant<PaintedModifierRequest, std::string> read_painted_modifier_request(int object_id, const ModelObject& object,
                                                                                const nlohmann::json& params);

// Why `host_volume_id` or `depth_mm` cannot be given with another add_volume `type`: they belong to
// type painted_modifier alone. Nothing for that type, and for any other with neither argument.
std::optional<std::string> painted_modifier_argument_refusal(const std::string& type_name, const nlohmann::json& params);

// What a painted modifier adds to its get_object_info volume row: {host_volume_id (null when the host
// is gone), depth_mm, painted (its facets are not empty), settings (the keys of its own config)}.
// An empty object for any other volume. Cheap: no selector is built.
nlohmann::json painted_modifier_row_fields(const ModelObject& object, std::size_t volume_idx);

// How much of the volume's host surface its paint claims: {painted_facets, coverage_percent} of state
// painted. Builds a selector over the mesh, so it is asked for only where a response reports coverage.
nlohmann::json painted_modifier_coverage_json(const ModelVolume& painted_modifier);

// The painted modifiers on volume `volume_id` that a change of its type to `to` would leave without a
// part to paint on: them, when `to` is not MODEL_PART (call this before the change); none otherwise.
std::vector<int> painted_modifiers_orphaned_by_type_change(const ModelObject& object, int volume_id, ModelVolumeType to);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
