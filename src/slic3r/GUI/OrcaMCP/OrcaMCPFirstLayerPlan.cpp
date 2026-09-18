#include "OrcaMCPFirstLayerPlan.hpp"

#include <algorithm>
#include <cmath>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/graphics.h>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

PlanMapping plan_mapping(const BoundingBoxf3& plate, int resolution, double margin_mm)
{
    PlanMapping m;
    const double w_mm = plate.size().x() + 2. * margin_mm;
    const double h_mm = plate.size().y() + 2. * margin_mm;
    const double long_side = std::max(w_mm, h_mm);
    if (!(long_side > 0.) || resolution <= 0)
        return m;
    m.scale  = resolution / long_side;
    m.width  = int(std::lround(w_mm * m.scale));
    m.height = int(std::lround(h_mm * m.scale));
    m.origin = Vec2d(plate.min.x() - margin_mm, plate.min.y() - margin_mm);
    return m;
}

CameraFrame plan_camera(const PlanMapping& m)
{
    // Orthographic: x in [origin.x, origin.x + width/scale] -> [-1, 1], y likewise; z ignored.
    CameraFrame cam;
    const double w_mm = m.width / m.scale, h_mm = m.height / m.scale;
    cam.view       = Eigen::Matrix4d::Identity();
    cam.projection = Eigen::Matrix4d::Identity();
    cam.projection(0, 0) = 2. / w_mm;
    cam.projection(0, 3) = -1. - 2. * m.origin.x() / w_mm;
    cam.projection(1, 1) = 2. / h_mm;
    cam.projection(1, 3) = -1. - 2. * m.origin.y() / h_mm;
    cam.projection(2, 2) = -1e-3;  // keep z finite and in front of the camera for every plate height
    cam.viewport = {0, 0, m.width, m.height};
    return cam;
}

static Polygon rectangle_scaled(const BoundingBoxf& b)
{
    Polygon p;
    p.points = {Point::new_scale(b.min.x(), b.min.y()), Point::new_scale(b.max.x(), b.min.y()),
                Point::new_scale(b.max.x(), b.max.y()), Point::new_scale(b.min.x(), b.max.y())};
    return p;
}

FirstLayerPlan plan_from_footprints(const std::vector<FootprintInput>& footprints)
{
    FirstLayerPlan plan;
    plan.source = "footprints";
    for (const FootprintInput& f : footprints) {
        PlanObject o;
        o.object_index = f.object_index;
        o.name         = f.name;
        o.color        = object_palette_color(f.object_index);
        const BoundingBoxf body(Vec2d(f.body.min.x(), f.body.min.y()), Vec2d(f.body.max.x(), f.body.max.y()));
        o.body.emplace_back(rectangle_scaled(body));
        if (f.brim_extent_mm > 0.) {
            BoundingBoxf ring = body;
            ring.offset(f.brim_extent_mm);
            o.brim.push_back(rectangle_scaled(ring));
        }
        plan.objects.push_back(std::move(o));
    }
    return plan;
}

std::vector<OverlayLabel> plan_labels(const FirstLayerPlan& plan)
{
    std::vector<OverlayLabel> labels;
    for (const PlanObject& o : plan.objects) {
        if (o.body.empty())
            continue;
        // Centroid of the largest slice: a frame's hollow centre would otherwise put the label in air,
        // and this is still inside the object's footprint for anything convex-ish.
        const ExPolygon* largest = &o.body.front();
        for (const ExPolygon& e : o.body)
            if (e.contour.area() > largest->contour.area())
                largest = &e;
        const Point c = largest->contour.centroid();
        labels.push_back({std::to_string(o.object_index), Vec3d(unscale<double>(c.x()), unscale<double>(c.y()), 0.), o.color});
    }
    return labels;
}

// --- from the Print --------------------------------------------------------------------------

// The Print works on its own copy of the model, so pointers never match the plater's objects;
// ObjectIDs survive the copy and do.
static int model_object_index(const ModelObject* mo)
{
    if (mo == nullptr)
        return -1;
    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    for (size_t i = 0; i < objects.size(); ++i)
        if (objects[i] == mo || objects[i]->id() == mo->id())
            return int(i);
    return -1;
}

static FirstLayerPlan plan_from_print(const Print& print)
{
    FirstLayerPlan plan;
    plan.source = "sliced";
    // ModelObject id -> index into plan.objects, so brim groups can be handed to their object.
    std::vector<std::pair<ObjectID, size_t>> by_model_id;

    for (const PrintObject* po : print.objects()) {
        if (po->layers().empty())
            continue;
        const Layer* first = po->layers().front();
        PlanObject   o;
        o.object_index = model_object_index(po->model_object());
        o.name         = po->model_object() != nullptr ? po->model_object()->name : std::string();
        o.color        = object_palette_color(o.object_index);
        for (const PrintInstance& inst : po->instances()) {
            ExPolygons slices = first->lslices;
            translate(slices, inst.shift);
            append(o.body, std::move(slices));
            if (!po->support_layers().empty()) {
                Polygons fills = po->support_layers().front()->support_fills.polygons_covered_by_width(float(scale_(0.05)));
                for (Polygon& p : fills)
                    p.translate(inst.shift);
                append(plan.support, union_ex(fills));
            }
        }
        // Brim groups may name the PrintObject or its ModelObject; accept either.
        by_model_id.emplace_back(po->id(), plan.objects.size());
        by_model_id.emplace_back(po->model_object()->id(), plan.objects.size());
        plan.objects.push_back(std::move(o));
    }

    // Brims already sit in bed coordinates. A group's instances say which object(s) it belongs to;
    // one that spans several objects (merged brims) is drawn loose, in neutral grey.
    for (const Print::SkirtBrimGroup& group : print.skirt_brim_groups()) {
        for (const Print::SkirtBrimGroup::Brim& brim : group.brims) {
            Polygons loops = brim.brim.polygons_covered_by_width(float(scale_(0.05)));
            if (loops.empty())
                continue;
            size_t owner = size_t(-1);
            bool   shared = false;
            for (const ObjectInstanceID& id : brim.instances)
                for (const auto& [model_id, index] : by_model_id)
                    if (model_id == id.object_id) {
                        if (owner != size_t(-1) && owner != index) shared = true;
                        owner = index;
                    }
            if (owner == size_t(-1) || shared)
                append(plan.loose_brim, std::move(loops));
            else
                append(plan.objects[owner].brim, std::move(loops));
        }
    }
    return plan;
}

FirstLayerPlan collect_first_layer(PartPlate& plate, const DynamicPrintConfig& full_config)
{
    Print* print = plate.fff_print();
    if (plate.is_slice_result_valid() && print != nullptr && !print->objects().empty()) {
        FirstLayerPlan plan = plan_from_print(*print);
        const PrimeTowerState tower = OrcaMCPPlateUtils::GetPrimeTowerState(plate.get_index(), full_config);
        if (tower.printed) {
            const double b = tower.brim_width;
            plan.wipe_tower = rectangle_scaled(BoundingBoxf(Vec2d(tower.corner.x() - b, tower.corner.y() - b),
                                                            Vec2d(tower.corner.x() + tower.size.x() + b, tower.corner.y() + tower.size.y() + b)));
        }
        return plan;
    }

    std::vector<FootprintInput> footprints;
    for (const ModelObject* mo : plate.get_objects_on_this_plate()) {
        const ObjectFootprint fp = OrcaMCPPlateUtils::GetObjectFootprint(*mo, full_config);
        FootprintInput in;
        in.object_index   = model_object_index(mo);
        in.name           = mo->name;
        in.body           = BoundingBoxf3(Vec3d(fp.body.min.x(), fp.body.min.y(), 0.), Vec3d(fp.body.max.x(), fp.body.max.y(), 0.));
        in.brim_extent_mm = fp.brim.extent_mm;
        footprints.push_back(std::move(in));
    }
    return plan_from_footprints(footprints);
}

// --- drawing ---------------------------------------------------------------------------------

namespace {

wxColour to_wx(const ColorRGBA& c, unsigned char alpha)
{
    return wxColour(static_cast<unsigned char>(c.r() * 255.f), static_cast<unsigned char>(c.g() * 255.f),
                    static_cast<unsigned char>(c.b() * 255.f), alpha);
}

wxColour darker(const ColorRGBA& c, unsigned char alpha)
{
    return wxColour(static_cast<unsigned char>(c.r() * 160.f), static_cast<unsigned char>(c.g() * 160.f),
                    static_cast<unsigned char>(c.b() * 160.f), alpha);
}

void add_polygon(wxGraphicsPath& path, const Polygon& poly, const PlanMapping& m)
{
    if (poly.points.size() < 3)
        return;
    for (size_t i = 0; i < poly.points.size(); ++i) {
        const Vec2d px = m.to_px(unscale(poly.points[i]));
        if (i == 0) path.MoveToPoint(px.x(), px.y()); else path.AddLineToPoint(px.x(), px.y());
    }
    path.CloseSubpath();
}

void fill_expolygons(wxGraphicsContext& gc, const ExPolygons& expolys, const PlanMapping& m, const wxBrush& brush, const wxPen& pen)
{
    for (const ExPolygon& e : expolys) {
        wxGraphicsPath path = gc.CreatePath();
        add_polygon(path, e.contour, m);
        for (const Polygon& hole : e.holes)
            add_polygon(path, hole, m);
        gc.SetBrush(brush);
        gc.SetPen(pen);
        gc.DrawPath(path, wxODDEVEN_RULE);  // holes stay holes
    }
}

void stroke_polygons(wxGraphicsContext& gc, const Polygons& polys, const PlanMapping& m, const wxPen& pen)
{
    gc.SetPen(pen);
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    for (const Polygon& p : polys) {
        wxGraphicsPath path = gc.CreatePath();
        add_polygon(path, p, m);
        gc.StrokePath(path);
    }
}

} // namespace

wxImage draw_first_layer_plan(const FirstLayerPlan&             plan,
                              const PlanMapping&                m,
                              const BoundingBoxf3&              plate,
                              const std::vector<BoundingBoxf3>& excluded_areas,
                              const OverlayOptions&             overlays)
{
    wxImage image(std::max(1, m.width), std::max(1, m.height));
    image.InitAlpha();
    image.SetRGB(wxRect(0, 0, image.GetWidth(), image.GetHeight()), 237, 237, 237);
    {
        unsigned char* a = image.GetAlpha();
        if (a != nullptr) std::fill(a, a + size_t(image.GetWidth()) * size_t(image.GetHeight()), 255);
    }

    {
        wxBitmap   bitmap(image, 32);
        wxMemoryDC dc(bitmap);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            // Support first: hatched grey under everything else.
            fill_expolygons(*gc, plan.support, m, wxBrush(wxColour(120, 120, 120, 150), wxBRUSHSTYLE_CROSSDIAG_HATCH), wxPen(wxColour(90, 90, 90, 200), 1));
            if (plan.wipe_tower.has_value()) {
                const ColorRGBA grey = wipe_tower_color();
                fill_expolygons(*gc, {ExPolygon(*plan.wipe_tower)}, m, wxBrush(to_wx(grey, 200)), wxPen(darker(grey, 255), 1));
            }
            // Brim as the band it covers, in a translucent darker shade of the owner's colour; the
            // individual loops are too dense to read as lines at plan scale.
            for (const PlanObject& o : plan.objects) {
                if (!o.brim.empty())
                    fill_expolygons(*gc, union_ex(o.brim), m, wxBrush(darker(o.color, 90)), wxPen(darker(o.color, 200), 1));
                fill_expolygons(*gc, o.body, m, wxBrush(to_wx(o.color, 165)), wxPen(darker(o.color, 255), 1));
            }
            if (!plan.loose_brim.empty())
                fill_expolygons(*gc, union_ex(plan.loose_brim), m, wxBrush(wxColour(80, 80, 80, 90)), wxPen(wxColour(80, 80, 80, 200), 1));
            gc.reset();
            dc.SelectObject(wxNullBitmap);
            image = bitmap.ConvertToImage();
        }
    }

    draw_overlays(image, plan_camera(m), plate, excluded_areas, plan_labels(plan), overlays);
    return image;
}

}}} // namespace Slic3r::GUI::OrcaMCP
