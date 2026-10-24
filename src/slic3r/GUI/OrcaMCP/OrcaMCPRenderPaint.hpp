#ifndef slic3r_OrcaMCPRenderPaint_hpp_
#define slic3r_OrcaMCPRenderPaint_hpp_

// render_plate_view's paint: which paint a picture tints and in which colours, the painted facets of each part, and
// the legend that counts them. Everything here is a decision or mesh work on copies -- no GL, no wx, no scene -- so it
// is unit-tested in tests/slic3rutils/test_render_paint.cpp; the drawing is OrcaMCPPlateUtils.cpp's.

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// One colour a picture tints a state of a paint channel in. `channel` is the name a request uses ("support", "seam",
// "fuzzy_skin", "texture"); a texture tint also names its layer slot.
struct PaintTint
{
    std::string channel;
    int         texture_layer = -1;   // texture only
    int         state         = 1;    // the facet state tinted: enforcer / painted 1, blocker 2
    ColorRGBA   color;
    std::string label;                // "support enforcer", "texture layer 3"
};

// The channel names `paint` takes, in the order tints are made.
const std::vector<std::string>& paint_channel_names();

// The paint panels' own colours, referenced from the gizmos (TriangleSelectorGUI::enforcers_color / blockers_color).
ColorRGBA gizmo_enforcer_color();
ColorRGBA gizmo_blocker_color();

// The colour of texture layer `slot` (0..7): eight fixed colours that stay apart from the object palette, the two
// gizmo colours and the neutral grey the unpainted surface is drawn in (tested).
ColorRGBA texture_layer_color(int slot);

// The tints of `channels` (a subset of paint_channel_names(), any order). support and seam: enforcer then blocker in the
// gizmos' colours; fuzzy_skin: the enforcer colour only; texture: one tint per slot of `texture_slots`, or when that is
// empty per slot of `slots_with_layers`, in ascending slot order.
std::vector<PaintTint> paint_tints(const std::vector<std::string>& channels, const std::vector<int>& texture_slots,
                                   const std::vector<int>& slots_with_layers);

// The paint channel a tint reads from a volume.
PaintChannel paint_channel_of(const PaintTint& tint);

// Where a part's colour in the picture comes from: "paint" (colour paint is always drawn in its filaments' colours),
// "neutral" (a paint tint is asked for: the unpainted surface is grey so the tints read), "filament" (color_by
// filament) or "palette".
std::string color_source_of(bool colour_painted, bool neutral_base, ColorBy color_by);

// The filament colour of a part printing with filament `extruder_id` (1-based; 0 or past the list is the nearest slot,
// as GLVolume::simple_render clamps it). `colors` must not be empty.
ColorRGBA filament_color_for(int extruder_id, const std::vector<ColorRGBA>& colors);

// What the main thread captures of one part with paint: its mesh (shared, immutable) and, per tint, the volume's
// annotation data, so the facets can be worked out on any thread.
struct PaintVolumeInput
{
    int                                 object_id = -1;
    int                                 volume_id = -1;
    ObjectID                            object_identity;
    ObjectID                            volume_identity;
    std::shared_ptr<const TriangleMesh> mesh;
    Transform3d                         to_plate = Transform3d::Identity();   // the first instance on the plate, for the area
    std::vector<std::pair<int, PaintData>> per_tint;                          // (index into the tints, the annotation's data)
};

// The facets of one part in one tint (volume-local), and what they count.
struct PaintFacets
{
    int                  object_id = -1;
    int                  volume_id = -1;
    int                  tint      = 0;
    indexed_triangle_set its;
    int                  facets   = 0;      // leaf triangles, the count get_object_paint reports
    double               area_mm2 = 0.;     // on the plate, through the first instance's transform
};

// The leaf triangles of `data` at `state` on `mesh` (what FacetsAnnotation::get_facets returns, on any thread).
indexed_triangle_set painted_facets(const TriangleMesh& mesh, const PaintData& data, int state);

// Surface area of `its` after `trafo`, mm2.
double world_area_mm2(const indexed_triangle_set& its, const Transform3d& trafo);

// What the work made of the captured parts.
struct PaintResult
{
    std::vector<PaintFacets> facets;            // parts and tints with none are left out
    std::vector<std::pair<int, int>> stale;     // (tint, object_id) whose mask indexes triangles the mesh no longer has: skipped
    bool                     complete = true;   // false: `stop` said so before every part was read
};

// The painted facets of every part and tint. One TriangleSelector is built per part and annotation (support enforcer and
// blocker share it) and read per state. A mask that indexes triangles its mesh does not have (a stale texture mask)
// is skipped and listed in `stale`. Stops before each selector and every few thousand triangles of an area sum.
PaintResult compute_paint_facets(const std::vector<PaintVolumeInput>& inputs, const std::vector<PaintTint>& tints,
                                 const std::function<bool()>& stop);

// The answer's legend: per tint {channel, layer?, state, state_name, label, color (#rrggbb), objects: [{object_id, facets,
// area_mm2}], stale?: [object ids]} -- objects with none of it are left out, so a tint nothing carries has an empty list.
nlohmann::json tint_legend_json(const std::vector<PaintTint>& tints, const PaintResult& result);

// What to tell the reader beyond the legend: a texture request when no object on the plate has a texture layer, and a
// stale mask. Empty when there is nothing to say.
std::vector<std::string> tint_notes(const std::vector<std::string>& channels, const std::vector<PaintTint>& tints,
                                     const PaintResult& result);

// The colour a label is drawn in: `color` unless it is too dark to read on the label's dark pill, then lightened toward
// white to a luminance of 0.35 (the object palette is untouched).
ColorRGBA readable_label_color(const ColorRGBA& color);

// "#rrggbb"
std::string hex_color(const ColorRGBA& color);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
