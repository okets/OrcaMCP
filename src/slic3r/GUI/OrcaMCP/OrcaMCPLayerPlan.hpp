#ifndef slic3r_OrcaMCPLayerPlan_hpp_
#define slic3r_OrcaMCPLayerPlan_hpp_

// A top-down plan of any one sliced layer: render_plate_view's layer_view {layer} / {z}.
//
// It is drawn from the plate's G-code processor result -- the moves the Preview draws -- not from the
// Print, because a move records the filament that really printed it. Working that out from the Print
// would repeat GCode::process_layer's rules (support set to filament 0 prints with whatever tool is
// active, infill and support flushing, mixed filaments). The layers are the G-code's: layer N is what
// the Preview's slider calls layer N, and there are get_print_estimate's printed_layers of them.
//
// The Print still answers what only it knows: which object layer and which support layer print at
// that height (support can have heights of its own), and how much of an overhang has support under
// it. Everything here but draw_layer_plan is plain arithmetic over those two, unit-tested in
// tests/slic3rutils/test_layer_plan.cpp without the app.

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/image.h>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPExtrusionFeatures.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPFirstLayerPlan.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderOverlay.hpp"

namespace Slic3r {
class Layer;
class Model;
class Print;
class PrintObject;
namespace GUI { namespace OrcaMCP {

using GcodeMove = GCodeProcessorResult::MoveVertex;

// --- which layer -----------------------------------------------------------------------------

// One printed layer of the G-code: its moves are [begin, end) of GCodeProcessorResult::moves, and z is
// the height it prints at -- where its last extrusion is, which for a spiral vase is the top of the
// climb. Layer i is the one the Preview's slider numbers i + 1.
struct GcodeLayer
{
    size_t begin = 0, end = 0;
    double z     = 0.;
};

// Every layer of `moves`, found by binary search on MoveVertex::layer_id, which the processor only
// ever counts up (one per layer change), so this costs a few probes per layer, not a pass over moves.
std::vector<GcodeLayer> gcode_layers(const std::vector<GcodeMove>& moves);

// The layer the slider calls `number` (1-based). Throws, naming the valid range, when there is none.
size_t layer_by_number(const std::vector<GcodeLayer>& layers, int number);

struct LayerAtHeight
{
    size_t           index = 0;  // into the layers
    std::vector<int> also_at;    // other layer numbers printed at the same height (a by-object print)
};

// The layer printed nearest to `z` mm. A height exactly between two goes to the lower; one outside
// the print goes to its first or last layer. Throws when there are no layers.
LayerAtHeight layer_nearest_z(const std::vector<GcodeLayer>& layers, double z);

// --- the request -----------------------------------------------------------------------------

// The features a plan can draw: every ExtrusionFeature but `other` (start G-code purge lines and the
// like), in the enum's order.
constexpr size_t k_plan_feature_count = size_t(ExtrusionFeature::other);

enum class LayerColorBy { feature, filament };

// layer_view's object form.
struct LayerPlanRequest
{
    std::optional<int>                     layer;      // 1-based slider number
    std::optional<double>                  z;          // mm, snapped to the nearest printed layer
    std::array<bool, k_plan_feature_count> features;   // which features to draw; all by default
    std::vector<int>                       filaments;  // 1-based slots to draw; empty draws them all
    LayerColorBy                           color_by = LayerColorBy::feature;
    std::optional<int>                     fit_object; // frame this object instead of the plate

    LayerPlanRequest() { features.fill(true); }
    bool draws(ExtrusionFeature feature) const;
    bool draws(int filament) const;
};

// What render_plate_view's layer_view asks for: the first-layer plan, or one sliced layer.
struct LayerView
{
    bool             first_layer = false;
    LayerPlanRequest sliced;  // when not first_layer
};

// The one reader of layer_view: "first_layer", or {layer | z, features?, filaments?, color_by?, fit?}
// -- an object, or the JSON text of one from a client whose cached schema types layer_view as a
// string. Throws, naming the valid forms and values, on anything else: another form, both or neither
// of layer and z, an unknown feature, a filament number below 1, an unknown color_by.
LayerView parse_layer_view(const nlohmann::json& value);

// Throws, naming the printer's filaments, when `request` names one past filament_count. Apart from
// parse_layer_view because the count is the plate's: parse first, then check against it.
void check_filaments(const LayerPlanRequest& request, size_t filament_count);

// What was drawn: {features, filaments, color_by}, as the response echoes it.
nlohmann::json drawn_json(const LayerPlanRequest& request);

// --- toolpaths -------------------------------------------------------------------------------

// Consecutive extrusions of one feature, filament and width, as one polyline in bed mm. A move stores
// where it ends, so the first point is where the move before it ended.
struct ToolpathRun
{
    ExtrusionFeature   feature  = ExtrusionFeature::other;
    int                filament = 0;   // 1-based
    float              width    = 0.f; // mm
    std::vector<Vec2d> points;
};

// The runs of `layer` the request draws.
std::vector<ToolpathRun> layer_toolpaths(const std::vector<GcodeMove>& moves, const GcodeLayer& layer,
                                         const LayerPlanRequest& request);

// --- numbers ---------------------------------------------------------------------------------

using FeatureAreas = std::array<double, k_plan_feature_count>;

// Everything one layer extrudes, whatever is drawn: the area each feature lays down (length x width,
// so where two lines overlap it counts twice), by filament too, and the filaments in the order they
// start printing. `extent` bounds its object and support lines, not the tower or skirt.
struct LayerExtrusion
{
    FeatureAreas                 mm2{};
    std::map<int, FeatureAreas>  mm2_by_filament;  // 1-based filament
    std::vector<int>             filament_order;
    BoundingBoxf                 extent;

    double object_mm2() const;   // perimeters + infill
    double support_mm2() const;  // support + support_interface
};

LayerExtrusion layer_extrusion(const std::vector<GcodeMove>& moves, const GcodeLayer& layer);

// {extruded_mm2: {feature: mm2}, extruded_mm2_by_filament: {"n": {...}}, object_mm2, support_mm2,
// filaments: [...]}, to 0.01 mm2.
nlohmann::json layer_extrusion_json(const LayerExtrusion& extrusion);

// --- the Print at that height ----------------------------------------------------------------

// One object or support layer, numbered from 1 within its own object.
struct PrintedLayerRef
{
    int    number  = 0;
    double print_z = 0.;
    double height  = 0.;
};

// How far below an overhang the search for its support goes: well past any top Z distance a profile
// uses, and a bounded number of layers to look at.
constexpr double k_support_search_mm = 2.0;

// The support under an overhang, as plain facts. Support layers belong to the whole object, so the one
// right at an overhang's bottom may have been built for another overhang elsewhere; this is the first
// support layer, going down from the overhang's bottom, whose lines lie under this overhang (more than
// 0.01 mm2 of it): its height z, gap_mm from it up to the overhang, and how much of the overhang its
// support lines -- and its interface lines alone -- lie under (the bands support_covered builds, line
// width included). A gap near the configured top Z distance means it is the contact layer.
//
// When no support layer within k_support_search_mm below has lines under the overhang, it is the
// nearest support layer below instead, with no area, and searched_to_mm says how deep the search went:
// nothing touches the overhang there.
struct SupportBelow
{
    double                z             = 0.;
    double                gap_mm        = 0.;
    double                support_mm2   = 0.;
    double                interface_mm2 = 0.;
    std::optional<double> searched_to_mm;       // set only when nothing was found under the overhang
    size_t                layers_visited = 0;   // support layers the search measured (not reported)
};

// How much of an object layer hangs over nothing -- the parts of it more than `tolerance_mm` beyond the
// layer below (half the nozzle printing its walls: a wall can lean that far on its own) -- and the
// support under it (SupportBelow), none when there is no support layer below at all.
struct Overhang
{
    double                      area_mm2     = 0.;
    double                      tolerance_mm = 0.;
    std::optional<SupportBelow> support_below;
};

struct ObjectAtHeight
{
    int                            object_index = -1;
    std::string                    name;
    std::optional<PrintedLayerRef> object_layer;   // none: nothing of the object prints at this height
    std::optional<PrintedLayerRef> support_layer;
    std::optional<Overhang>        overhang;       // on an object layer with one below it
};

// The index of the entry of ascending `print_zs` within `tolerance` of `z`, the closest if several.
std::optional<size_t> layer_index_at_height(const std::vector<double>& print_zs, double z, double tolerance);

// The G-code writes heights to a thousandth of a mm; this matches them to the Print's.
constexpr double k_gcode_height_tolerance = 0.002;

// Every object of `print` with an object or support layer printed at `z`. A by-object print prints
// each object in its own pass, so there only the objects whose layer at `z` lies under `extent` (the
// G-code layer's own lines) are listed.
std::vector<ObjectAtHeight> objects_at_height(const Print& print, const Model& model, double z,
                                              const BoundingBoxf& extent);

// The overhang of `object`'s layer `layer_index`, or none on its first layer.
std::optional<Overhang> overhang_of(const PrintObject& object, size_t layer_index);

// Half the smallest nozzle printing `layer`'s outer walls: how far a wall can lean past the layer
// below on its own. The nozzle is the one the slicer sizes those walls for (PrintRegion::flow).
double overhang_tolerance(const Layer& layer);

nlohmann::json objects_at_height_json(const std::vector<ObjectAtHeight>& objects);

// Where each of `print`'s objects stands on the bed: the box of its instances in bed mm (x, y), from
// the sliced object's own size and each instance's place. It costs nothing per vertex, unlike the
// model's exact instance box, which on a million-facet mesh took 1.7 s (-O0) and was needed twice.
struct PrintFootprint
{
    int          object_index = -1;
    std::string  name;
    BoundingBoxf box;
};
std::vector<PrintFootprint> print_footprints(const Print& print, const Model& model);

// What a layer plan's fit: {object_index} frames on the layer at `z`: the object's footprint, and
// whatever of its own prints at that height reaching past it -- its support lines, and on the first
// layer its brim and raft, and any combined brim touching it (plan_from_print). None when `print`
// does not hold that object.
std::optional<BoundingBoxf> object_frame(const Print& print, const Model& model, int object_index, double z);

// The response's layer: {number, of, z}, and with a height asked for, requested_z and also_at.
nlohmann::json layer_json(const std::vector<GcodeLayer>& layers, const LayerAtHeight& chosen, const LayerPlanRequest& request);

// --- colour and legend -----------------------------------------------------------------------

struct LegendEntry
{
    std::string key;    // a feature key, or the filament number
    std::string label;  // what the image's legend says
    ColorRGBA   color;
    double      mm2 = 0.;  // extruded area of what this entry drew
};

// The colour a run is drawn in: its feature's Preview colour, or its filament's slot colour.
ColorRGBA run_color(const ToolpathRun& run, LayerColorBy color_by, const std::vector<ColorRGBA>& slot_colors);

// One entry per colour in the picture, for the features and filaments the request draws and the
// layer extrudes, in feature order or filament order.
std::vector<LegendEntry> layer_legend(const LayerPlanRequest& request, const LayerExtrusion& extrusion,
                                      const std::vector<ColorRGBA>& slot_colors);

nlohmann::json legend_json(const std::vector<LegendEntry>& legend);

// --- drawing ---------------------------------------------------------------------------------

// The runs, stroked at their extrusion width (at least a pixel) over the plan background, then the
// overlays, then the legend in the top-right corner when overlays.labels is on.
wxImage draw_layer_plan(const std::vector<ToolpathRun>&   runs,
                        LayerColorBy                      color_by,
                        const std::vector<ColorRGBA>&     slot_colors,
                        const std::vector<LegendEntry>&   legend,
                        const PlanMapping&                mapping,
                        const BoundingBoxf3&              plate,
                        const std::vector<BoundingBoxf3>& excluded_areas,
                        const std::vector<OverlayLabel>&  labels,
                        const OverlayOptions&             overlays);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r

#endif
