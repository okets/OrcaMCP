#ifndef slic3r_OrcaMCPRenderOverlay_hpp_
#define slic3r_OrcaMCPRenderOverlay_hpp_

// 2D overlays drawn on a finished render so the picture locates itself: the plate outline, a 10 mm
// grid, the origin corner with X/Y letters, numbered object labels, hatched excluded areas. Every
// point is projected through the render camera's own matrices (OrcaMCPRenderMath), so the overlay
// sits exactly on the geometry from any viewpoint. No GL: the drawing happens with wx on the
// wxImage, which is why a plan view drawn entirely on the CPU can use the same code.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/colour.h>
#include <wx/image.h>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

struct OverlayOptions
{
    bool outline  = true;
    bool grid     = true;
    bool origin   = true;
    bool labels   = true;
    bool excluded = true;
    bool any() const { return outline || grid || origin || labels || excluded; }
};

// `overlays` as the tool accepts it: missing/true = all, false = none, or an object with per-overlay
// booleans (unknown keys are ignored, missing keys keep their default of true).
OverlayOptions overlay_options_from_json(const nlohmann::json& value);
nlohmann::json overlay_options_to_json(const OverlayOptions& options);

struct OverlayLabel
{
    std::string text;
    Vec3d       world_anchor;  // bed mm; the label is centred on its projection
    ColorRGBA   color;
};

// The render buffer as a top-down wxImage with alpha: GL rows are bottom-up, images top-down.
wxImage thumbnail_to_wximage(const ThumbnailData& thumbnail_data);

// Draws the requested overlays onto `image` in place. `plate_box` and `excluded_areas` are bed mm.
void draw_overlays(wxImage&                          image,
                   const CameraFrame&                camera,
                   const BoundingBoxf3&              plate_box,
                   const std::vector<BoundingBoxf3>& excluded_areas,
                   const std::vector<OverlayLabel>&  labels,
                   const OverlayOptions&             options,
                   const wxColour&                   background = wxColour(237, 237, 237));

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
