#include "OrcaMCPRenderOverlay.hpp"

#include <algorithm>
#include <cmath>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/font.h>
#include <wx/graphics.h>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

OverlayOptions overlay_options_from_json(const nlohmann::json& value)
{
    OverlayOptions o;
    if (value.is_boolean() && !value.get<bool>()) {
        o.outline = o.grid = o.origin = o.labels = o.excluded = false;
    } else if (value.is_object()) {
        o.outline  = value.value("outline", true);
        o.grid     = value.value("grid", true);
        o.origin   = value.value("origin", true);
        o.labels   = value.value("labels", true);
        o.excluded = value.value("excluded", true);
    }
    return o;
}

nlohmann::json overlay_options_to_json(const OverlayOptions& o)
{
    return {{"outline", o.outline}, {"grid", o.grid}, {"origin", o.origin}, {"labels", o.labels}, {"excluded", o.excluded}};
}

wxImage thumbnail_to_wximage(const ThumbnailData& thumbnail_data)
{
    wxImage image(int(thumbnail_data.width), int(thumbnail_data.height));
    image.InitAlpha();
    for (unsigned int r = 0; r < thumbnail_data.height; ++r) {
        const unsigned int rr = (thumbnail_data.height - 1 - r) * thumbnail_data.width;
        for (unsigned int c = 0; c < thumbnail_data.width; ++c) {
            const unsigned char* px = thumbnail_data.pixels.data() + 4 * (rr + c);
            image.SetRGB(int(c), int(r), px[0], px[1], px[2]);
            image.SetAlpha(int(c), int(r), px[3]);
        }
    }
    return image;
}

namespace {

wxColour to_wx(const ColorRGBA& c, unsigned char alpha = 255)
{
    return wxColour(static_cast<unsigned char>(c.r() * 255.f), static_cast<unsigned char>(c.g() * 255.f),
                    static_cast<unsigned char>(c.b() * 255.f), alpha);
}

// Everything below draws through one context; `project` turns bed mm into image pixels and says
// when a point has no pixel (behind the camera), so a line with a missing end is simply skipped.
struct OverlayPainter
{
    wxGraphicsContext& gc;
    const CameraFrame& camera;
    const int          width, height;
    wxFont             font;  // wxGraphicsContext has no getter, so the painter remembers what it set

    void set_font(const wxFont& f, const wxColour& colour)
    {
        font = f;
        gc.SetFont(f, colour);
    }

    bool project(const Vec3d& world, wxPoint2DDouble& out) const
    {
        Vec2d px;
        if (!project_point_to_pixel(camera, world, px))
            return false;
        out = wxPoint2DDouble(px.x(), px.y());
        return true;
    }

    void line(const Vec3d& a, const Vec3d& b, const wxPen& pen) const
    {
        wxPoint2DDouble pa, pb;
        if (!project(a, pa) || !project(b, pb))
            return;
        gc.SetPen(pen);
        gc.StrokeLine(pa.m_x, pa.m_y, pb.m_x, pb.m_y);
    }

    // A closed polygon through the projections of `corners`; false when a corner has no pixel.
    bool polygon(const std::vector<Vec3d>& corners, wxGraphicsPath& path) const
    {
        wxPoint2DDouble p;
        for (size_t i = 0; i < corners.size(); ++i) {
            if (!project(corners[i], p))
                return false;
            if (i == 0) path.MoveToPoint(p); else path.AddLineToPoint(p);
        }
        path.CloseSubpath();
        return true;
    }

    void text_centred(const wxString& text, const wxPoint2DDouble& at, const wxColour& colour) const
    {
        double w = 0., h = 0.;
        gc.SetFont(font, colour);
        gc.GetTextExtent(text, &w, &h);
        gc.DrawText(text, at.m_x - w / 2., at.m_y - h / 2.);
    }

    static std::vector<Vec3d> footprint_corners(const BoundingBoxf3& box, double z)
    {
        return {Vec3d(box.min.x(), box.min.y(), z), Vec3d(box.max.x(), box.min.y(), z),
                Vec3d(box.max.x(), box.max.y(), z), Vec3d(box.min.x(), box.max.y(), z)};
    }
};

} // namespace

// Grid and outline belong on the bed, under the parts, but a 2D pass has no depth. They are drawn
// on a transparent layer and composited only where the render still shows its background colour,
// so an object's pixels are never crossed by a grid line. Anti-aliased edges are the only pixels
// that misjudge, and they are one pixel wide.
static void composite_on_background(wxImage& base, const wxImage& layer, const wxColour& background)
{
    const int w = base.GetWidth(), h = base.GetHeight();
    if (!layer.IsOk() || layer.GetWidth() != w || layer.GetHeight() != h || !layer.HasAlpha())
        return;
    const unsigned char* lrgb = layer.GetData();
    const unsigned char* la   = layer.GetAlpha();
    unsigned char*       brgb = base.GetData();
    const int            tol  = 6;
    for (int i = 0; i < w * h; ++i) {
        const unsigned char a = la[i];
        if (a == 0)
            continue;
        unsigned char* b = brgb + 3 * i;
        if (std::abs(int(b[0]) - background.Red()) > tol || std::abs(int(b[1]) - background.Green()) > tol || std::abs(int(b[2]) - background.Blue()) > tol)
            continue;  // not background: an object is here, keep it on top
        const unsigned char* l = lrgb + 3 * i;
        for (int c = 0; c < 3; ++c)
            b[c] = static_cast<unsigned char>((l[c] * a + b[c] * (255 - a)) / 255);
    }
}

void draw_overlays(wxImage&                          image,
                   const CameraFrame&                camera,
                   const BoundingBoxf3&              plate_box,
                   const std::vector<BoundingBoxf3>& excluded_areas,
                   const std::vector<OverlayLabel>&  labels,
                   const OverlayOptions&             options,
                   const wxColour&                   background)
{
    if (!options.any() || !image.IsOk())
        return;

    const double z = plate_box.min.z();
    const wxFont bold(wxFontInfo(11).Family(wxFONTFAMILY_SWISS).Bold());

    // Pass 1, under the parts: grid and outline on a transparent layer.
    if (options.grid || options.outline) {
        wxImage layer(image.GetWidth(), image.GetHeight());
        layer.InitAlpha();
        std::fill(layer.GetAlpha(), layer.GetAlpha() + size_t(layer.GetWidth()) * size_t(layer.GetHeight()), 0);
        wxBitmap   lbitmap(layer, 32);
        wxMemoryDC ldc(lbitmap);
        std::unique_ptr<wxGraphicsContext> lgc(wxGraphicsContext::Create(ldc));
        if (lgc) {
            lgc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            OverlayPainter under{*lgc, camera, image.GetWidth(), image.GetHeight()};
            if (options.grid) {
                const wxPen minor(wxColour(0, 0, 0, 40), 1), major(wxColour(0, 0, 0, 90), 1);
                for (const GridSegment& seg : grid_segments(plate_box, 10.0, 5))
                    under.line(seg.a, seg.b, seg.major ? major : minor);
            }
            if (options.outline) {
                wxGraphicsPath path = lgc->CreatePath();
                if (under.polygon(OverlayPainter::footprint_corners(plate_box, z), path)) {
                    lgc->SetBrush(*wxTRANSPARENT_BRUSH);
                    lgc->SetPen(wxPen(wxColour(60, 60, 60), 1));
                    lgc->StrokePath(path);
                }
            }
            lgc.reset();
            ldc.SelectObject(wxNullBitmap);
            composite_on_background(image, lbitmap.ConvertToImage(), background);
        }
    }

    // Pass 2, on top: excluded areas, origin, labels.
    wxBitmap   bitmap(image, 32);
    wxMemoryDC dc(bitmap);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    gc->SetInterpolationQuality(wxINTERPOLATION_BEST);
    OverlayPainter paint{*gc, camera, image.GetWidth(), image.GetHeight()};

    if (options.excluded) {
        for (const BoundingBoxf3& area : excluded_areas) {
            wxGraphicsPath path = gc->CreatePath();
            if (!paint.polygon(OverlayPainter::footprint_corners(area, z), path))
                continue;
            gc->SetBrush(wxBrush(wxColour(200, 40, 40, 120), wxBRUSHSTYLE_CROSSDIAG_HATCH));
            gc->SetPen(wxPen(wxColour(200, 40, 40, 200), 1));
            gc->DrawPath(path);
        }
    }

    if (options.origin) {
        const Vec3d  o = Vec3d(plate_box.min.x(), plate_box.min.y(), z);
        const Vec3d  size = plate_box.size();
        wxPoint2DDouble po;
        if (paint.project(o, po)) {
            gc->SetBrush(wxBrush(wxColour(40, 40, 40)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawEllipse(po.m_x - 3., po.m_y - 3., 6., 6.);
        }
        const wxPen axis(wxColour(40, 40, 40), 2);
        const Vec3d ax = o + Vec3d(0.12 * size.x(), 0., 0.), ay = o + Vec3d(0., 0.12 * size.y(), 0.);
        paint.line(o, ax, axis);
        paint.line(o, ay, axis);
        paint.set_font(bold, wxColour(40, 40, 40));
        wxPoint2DDouble p;
        if (paint.project(ax + Vec3d(0.02 * size.x(), 0., 0.), p)) paint.text_centred("X", p, wxColour(40, 40, 40));
        if (paint.project(ay + Vec3d(0., 0.02 * size.y(), 0.), p)) paint.text_centred("Y", p, wxColour(40, 40, 40));
    }

    if (options.labels) {
        paint.set_font(bold, *wxWHITE);
        for (const OverlayLabel& label : labels) {
            wxPoint2DDouble p;
            if (!paint.project(label.world_anchor, p))
                continue;
            const wxString text = wxString::FromUTF8(label.text.c_str());
            double w = 0., h = 0.;
            gc->GetTextExtent(text, &w, &h);
            const double pad = 4.;
            // A dark pill keeps the label legible on any background; the text carries the object's
            // colour so number and shape can be matched at a glance.
            gc->SetBrush(wxBrush(wxColour(30, 30, 30, 210)));
            gc->SetPen(wxPen(to_wx(label.color), 1));
            gc->DrawRoundedRectangle(p.m_x - w / 2. - pad, p.m_y - h / 2. - pad / 2., w + 2. * pad, h + pad, 4.);
            paint.text_centred(text, p, to_wx(label.color));
        }
    }

    gc.reset();
    dc.SelectObject(wxNullBitmap);
    image = bitmap.ConvertToImage();
}

}}} // namespace Slic3r::GUI::OrcaMCP
