#ifndef slic3r_OrcaMCPFirstLayerPlan_hpp_
#define slic3r_OrcaMCPFirstLayerPlan_hpp_

// A top-down plan of a plate's first layer: object bodies, brims, support and the wipe tower, drawn
// on the CPU from the sliced Print (or from model footprints when the plate is not sliced). This is
// the view that answers "is the brim wide enough" and "where do the support feet land" -- the
// questions the 3D render cannot, because it draws model bodies only.
//
// Geometry is kept in Slic3r's scaled integer coordinates, in the BED frame (instance shifts
// applied), and converted to pixels by PlanMapping at draw time.

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/image.h>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderOverlay.hpp"

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI {
class PartPlate;
namespace OrcaMCP {

struct PlanObject
{
    int         object_index = -1;
    std::string name;
    ExPolygons  body;   // first-layer slices, scaled, bed frame
    Polygons    brim;   // brim loops that belong to this object, scaled, bed frame
    ColorRGBA   color;
};

struct FirstLayerPlan
{
    std::vector<PlanObject> objects;
    ExPolygons              support;       // support first layer, all objects
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

// From the plate's Print when its slice result is valid, otherwise plan_from_footprints over the
// plate's objects. `full_config` resolves brim settings for the fallback.
FirstLayerPlan collect_first_layer(PartPlate& plate, const DynamicPrintConfig& full_config);

wxImage draw_first_layer_plan(const FirstLayerPlan&              plan,
                              const PlanMapping&                 mapping,
                              const BoundingBoxf3&               plate,
                              const std::vector<BoundingBoxf3>&  excluded_areas,
                              const OverlayOptions&              overlays);

// Labels for draw_overlays: the object index at each body's centroid (bed mm, z = 0).
std::vector<OverlayLabel> plan_labels(const FirstLayerPlan& plan);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
