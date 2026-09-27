#ifndef slic3r_OrcaMCPFirstLayerPlan_hpp_
#define slic3r_OrcaMCPFirstLayerPlan_hpp_

// A top-down plan of a plate's first layer: object bodies, brims, support and the wipe tower, drawn
// on the CPU from the sliced Print (or from model footprints when the plate is not sliced). This is
// the view that answers "is the brim wide enough" and "where do the support feet land" -- the
// questions the 3D render cannot, because it draws model bodies only.
//
// Geometry is kept in Slic3r's scaled integer coordinates, in the BED frame (instance shifts
// applied), and converted to pixels by PlanMapping at draw time.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/colour.h>
#include <wx/image.h>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Polygon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderOverlay.hpp"

class wxGraphicsContext;

namespace Slic3r {
class DynamicPrintConfig;
class Model;
class Print;
class SupportLayer;
namespace GUI {
class PartPlate;
namespace OrcaMCP {

struct PlanObject
{
    int         object_index = -1;
    std::string name;
    ExPolygons  body;   // first-layer slices, scaled, bed frame; empty when the object's own first
                        // layer is not what the plate prints first (it stands on a raft)
    ExPolygons  raft;   // the raft's first layer under this object, when it stands on one
    Polygons    brim;   // brim loops that belong to this object, scaled, bed frame
    ColorRGBA   color;

    // What the object puts on the bed: its body, or its raft.
    const ExPolygons& on_bed() const { return body.empty() ? raft : body; }
};

struct FirstLayerPlan
{
    std::vector<PlanObject> objects;
    ExPolygons              support;       // support printed on the first layer, all objects (rafts are the objects')
    Polygons                loose_brim;    // brim loops no object claimed (grouped/merged brims)
    std::optional<Polygon>  wipe_tower;    // footprint incl. its brim, when a tower prints
    std::string             source;        // "sliced" or "footprints"
};

// Bed mm -> image pixels for a plan of `plate` whose longer side is `resolution` px, with a small
// margin. Pixel rows count down from the top, so +Y on the bed is up in the image.
struct PlanMapping
{
    double scale  = 1.;                 // px per mm
    Vec2d  origin = Vec2d::Zero();      // bed mm at pixel (0, height)
    int    width  = 0, height = 0;
    Vec2d to_px(const Vec2d& mm) const { return Vec2d((mm.x() - origin.x()) * scale, height - (mm.y() - origin.y()) * scale); }
};
PlanMapping plan_mapping(const BoundingBoxf3& plate, int resolution, double margin_mm = 5.0);

// An orthographic CameraFrame equivalent to `mapping`, so draw_overlays projects bed mm onto the
// plan exactly where to_px puts it. Tested against to_px directly.
CameraFrame plan_camera(const PlanMapping& mapping);

// The unsliced fallback: one rectangle per object, with its brim as an outer ring.
struct FootprintInput
{
    int           object_index = -1;
    std::string   name;
    BoundingBoxf3 body;              // bed mm
    double        brim_extent_mm = 0.;
};
FirstLayerPlan plan_from_footprints(const std::vector<FootprintInput>& footprints);

// The height a sliced `print` prints first: the lowest first layer among its objects' object and
// support layers. With a raft that is the raft's base; the object's own first layer sits on top of it.
double first_print_height(const Print& print);

// The band a support layer's lines cover (their width, a hair wider so neighbours merge), moved by
// `shift` onto an instance's place on the bed; only the lines of `role` when one is given.
ExPolygons support_covered(const SupportLayer& layer, const Point& shift = Point(0, 0),
                           std::optional<ExtrusionRole> role = std::nullopt);

// The first layer of a sliced `print`: what prints at its lowest height. An object's own first layer
// is drawn only when it is printed there, so a raft is drawn in its place, and support that starts
// higher up (standing on the part) is left out. `model` numbers the objects (object_index).
FirstLayerPlan plan_from_print(const Print& print, const Model& model);

// From the plate's Print when its slice result is valid, otherwise plan_from_footprints over the
// plate's objects. `full_config` resolves brim settings for the fallback.
FirstLayerPlan collect_first_layer(PartPlate& plate, const DynamicPrintConfig& full_config);

// The plan's light grey background; overlays draw only where it still shows (draw_overlays).
inline wxColour plan_background() { return wxColour(237, 237, 237); }

// A blank plan image of `mapping`'s size, with `paint` drawing on it through one graphics context
// (anti-aliased, bed mm to pixels by mapping.to_px). Shared by every top-down plan.
wxImage paint_plan(const PlanMapping& mapping, const std::function<void(wxGraphicsContext&)>& paint);

wxImage draw_first_layer_plan(const FirstLayerPlan&              plan,
                              const PlanMapping&                 mapping,
                              const BoundingBoxf3&               plate,
                              const std::vector<BoundingBoxf3>&  excluded_areas,
                              const OverlayOptions&              overlays);

// Labels for draw_overlays: the object index at each body's centroid (bed mm, z = 0).
std::vector<OverlayLabel> plan_labels(const FirstLayerPlan& plan);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
