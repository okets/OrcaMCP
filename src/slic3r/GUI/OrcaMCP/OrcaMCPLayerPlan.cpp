#include "OrcaMCPLayerPlan.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/font.h>
#include <wx/graphics.h>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

bool is_feature_extrusion(const GcodeMove& m)
{
    return m.type == EMoveType::Extrude && extrusion_feature_of(m.extrusion_role) != ExtrusionFeature::other;
}

int   filament_of(const GcodeMove& m) { return int(m.extruder_id) + 1; }
Vec2d xy_of(const GcodeMove& m) { return Vec2d(m.position.x(), m.position.y()); }

bool is_object_or_support(ExtrusionFeature f)
{
    return f == ExtrusionFeature::perimeters || f == ExtrusionFeature::infill || f == ExtrusionFeature::support ||
           f == ExtrusionFeature::support_interface;
}

// `value` to `decimals` places, as the nearest double (dividing by the power of ten, not multiplying by
// its inverse, keeps 6.05 from printing as 6.050000000000001), and never -0.
double round_to(double value, int decimals)
{
    const double scale = std::pow(10., decimals);
    return std::round(value * scale) / scale + 0.0;
}
double area_mm2(const ExPolygons& shape) { return area(shape) * SCALING_FACTOR * SCALING_FACTOR; }

// The height layer [begin, end) prints at: where its last extrusion is, else its last move, else the
// height of the layer before it (a layer change with nothing in it).
double layer_height_of(const std::vector<GcodeMove>& moves, size_t begin, size_t end, double previous_z)
{
    for (size_t i = end; i > begin; --i)
        if (is_feature_extrusion(moves[i - 1]))
            return moves[i - 1].position.z();
    return end > begin ? double(moves[end - 1].position.z()) : previous_z;
}

std::string drawable_feature_list()
{
    std::string list;
    for (size_t f = 0; f < k_plan_feature_count; ++f)
        list += std::string(f ? ", " : "") + extrusion_feature_key(ExtrusionFeature(f));
    return list;
}

std::optional<ExtrusionFeature> drawable_feature_named(const std::string& key)
{
    for (size_t f = 0; f < k_plan_feature_count; ++f)
        if (key == extrusion_feature_key(ExtrusionFeature(f)))
            return ExtrusionFeature(f);
    return std::nullopt;
}

const char* feature_label(ExtrusionFeature f)
{
    switch (f) {
    case ExtrusionFeature::perimeters: return "Perimeters";
    case ExtrusionFeature::infill: return "Infill";
    case ExtrusionFeature::support: return "Support";
    case ExtrusionFeature::support_interface: return "Support interface";
    case ExtrusionFeature::brim: return "Brim";
    case ExtrusionFeature::skirt: return "Skirt";
    case ExtrusionFeature::prime_tower: return "Prime tower";
    case ExtrusionFeature::other: break;
    }
    return "Other";
}

std::array<bool, k_plan_feature_count> parse_features(const nlohmann::json& value)
{
    if (!value.is_array() || value.empty())
        throw std::runtime_error("features must be a list of at least one of: " + drawable_feature_list());
    std::array<bool, k_plan_feature_count> on{};
    for (const nlohmann::json& item : value) {
        const std::optional<ExtrusionFeature> f = item.is_string() ? drawable_feature_named(item.get<std::string>()) : std::nullopt;
        if (!f)
            throw std::runtime_error("unknown feature " + item.dump() + "; features are " + drawable_feature_list());
        on[size_t(*f)] = true;
    }
    return on;
}

std::vector<int> parse_filaments(const nlohmann::json& value)
{
    if (!value.is_array() || value.empty())
        throw std::runtime_error("filaments must be a list of filament slot numbers, counted from 1");
    std::vector<int> filaments;
    for (const nlohmann::json& item : value) {
        int n = 0;
        if (!parse_integer_param(item, n) || n < 1)
            throw std::runtime_error("filament " + item.dump() + " is not a filament slot number: they count from 1");
        filaments.push_back(n);
    }
    std::sort(filaments.begin(), filaments.end());
    filaments.erase(std::unique(filaments.begin(), filaments.end()), filaments.end());
    return filaments;
}

std::optional<int> parse_fit(const nlohmann::json& value)
{
    if (value.is_string() && value.get<std::string>() == "plate")
        return std::nullopt;
    int index = 0;
    if (value.is_object() && value.contains("object_index") && parse_integer_param(value["object_index"], index))
        return index;
    throw std::runtime_error("fit must be \"plate\" or {\"object_index\": n}");
}

nlohmann::json layer_ref_json(const std::optional<PrintedLayerRef>& ref)
{
    if (!ref)
        return nullptr;
    return {{"number", ref->number}, {"print_z", round_to(ref->print_z, 4)}, {"height", round_to(ref->height, 4)}};
}

nlohmann::json optional_z_json(const std::optional<double>& z) { return z ? nlohmann::json(round_to(*z, 4)) : nlohmann::json(nullptr); }

nlohmann::json overhang_json(const std::optional<Overhang>& o)
{
    if (!o)
        return nullptr;
    return {{"area_mm2", round_to(o->area_mm2, 2)},
            {"under_support_mm2", round_to(o->under_support_mm2, 2)},
            {"under_interface_mm2", round_to(o->under_interface_mm2, 2)},
            {"support_z", optional_z_json(o->support_z)},
            {"nearest_support_z", optional_z_json(o->nearest_support_z)},
            {"contact_z", round_to(o->contact_z, 4)},
            {"tolerance_mm", round_to(o->tolerance_mm, 4)}};
}

nlohmann::json feature_areas_json(const FeatureAreas& areas)
{
    nlohmann::json out = nlohmann::json::object();
    for (size_t f = 0; f < k_plan_feature_count; ++f)
        out[extrusion_feature_key(ExtrusionFeature(f))] = round_to(areas[f], 2);
    return out;
}

// The part of an object layer's print heights, or its support layers', that matches `z`.
template<class LayerPtrs>
std::optional<size_t> index_at(const LayerPtrs& layers, double z)
{
    std::vector<double> zs;
    zs.reserve(layers.size());
    for (const auto* layer : layers)
        zs.push_back(layer->print_z);
    return layer_index_at_height(zs, z, k_gcode_height_tolerance);
}

PrintedLayerRef ref_of(const Layer& layer, size_t index) { return {int(index) + 1, layer.print_z, layer.height}; }

// Whether one of `object`'s instances puts `shape` (object coordinates) under `extent` (bed mm).
bool lies_under(const PrintObject& object, const BoundingBox& shape, const BoundingBoxf& extent)
{
    if (!extent.defined || !shape.defined)
        return true;  // nothing to tell objects apart by
    BoundingBoxf grown = extent;
    grown.offset(1.0);
    for (const PrintInstance& inst : object.instances()) {
        const Point        lo = shape.min + inst.shift, hi = shape.max + inst.shift;
        const BoundingBoxf box(Vec2d(unscale<double>(lo.x()), unscale<double>(lo.y())), Vec2d(unscale<double>(hi.x()), unscale<double>(hi.y())));
        if (box.overlap(grown))
            return true;
    }
    return false;
}

// Whether any of `layer`'s support lines run over `hang` (object coordinates): the cheap test, on the
// lines themselves, before any band is built.
bool support_lines_over(const SupportLayer& layer, const ExPolygons& hang, const BoundingBox& hang_box)
{
    Points points;
    layer.support_fills.collect_points(points);
    if (points.empty() || !BoundingBox(points).overlap(hang_box))
        return false;
    Polylines lines;
    layer.support_fills.collect_polylines(lines);
    return !intersection_pl(lines, hang).empty();
}

// Adds `shape` (object coordinates) to `frame` (bed mm) at each of `object`'s instances.
void merge_on_bed(BoundingBoxf& frame, const PrintObject& object, const BoundingBox& shape)
{
    if (!shape.defined)
        return;
    for (const PrintInstance& inst : object.instances())
        for (const Point& corner : {Point(shape.min + inst.shift), Point(shape.max + inst.shift)})
            frame.merge(Vec2d(unscale<double>(corner.x()), unscale<double>(corner.y())));
}

void merge_mm(BoundingBoxf& frame, const BoundingBox& bed_shape)
{
    if (bed_shape.defined) {
        frame.merge(Vec2d(unscale<double>(bed_shape.min.x()), unscale<double>(bed_shape.min.y())));
        frame.merge(Vec2d(unscale<double>(bed_shape.max.x()), unscale<double>(bed_shape.max.y())));
    }
}

const PrintObject* print_object_of(const Print& print, const Model& model, int object_index)
{
    for (const PrintObject* po : print.objects())
        if (model_object_index(model, po->model_object()) == object_index)
            return po;
    return nullptr;
}

// The legend box in the image's top-right corner: a swatch and a label per entry.
void draw_legend(wxImage& image, const std::vector<LegendEntry>& legend)
{
    wxBitmap   bitmap(image, 32);
    wxMemoryDC dc(bitmap);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    gc->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_SWISS)), wxColour(30, 30, 30));

    double text_w = 0., text_h = 0.;
    for (const LegendEntry& entry : legend) {
        double w = 0., h = 0.;
        gc->GetTextExtent(wxString::FromUTF8(entry.label), &w, &h);
        text_w = std::max(text_w, w);
        text_h = std::max(text_h, h);
    }
    const double pad = 6., swatch = 12., row = std::max(text_h, swatch) + 4.;
    const double box_w = pad + swatch + pad + text_w + pad, box_h = pad + row * legend.size() + pad - 4.;
    const double x0 = image.GetWidth() - box_w - 6., y0 = 6.;

    gc->SetPen(wxPen(wxColour(120, 120, 120), 1));
    gc->SetBrush(wxBrush(wxColour(255, 255, 255, 225)));
    gc->DrawRoundedRectangle(x0, y0, box_w, box_h, 4.);
    for (size_t i = 0; i < legend.size(); ++i) {
        const double y = y0 + pad + row * i;
        gc->SetPen(wxPen(wxColour(60, 60, 60), 1));
        gc->SetBrush(wxBrush(to_wx_colour(legend[i].color)));
        gc->DrawRectangle(x0 + pad, y + (row - 4. - swatch) / 2., swatch, swatch);
        gc->DrawText(wxString::FromUTF8(legend[i].label), x0 + pad + swatch + pad, y + (row - 4. - text_h) / 2.);
    }
    gc.reset();
    dc.SelectObject(wxNullBitmap);
    image = bitmap.ConvertToImage();
}

} // namespace

// --- which layer -----------------------------------------------------------------------------

std::vector<GcodeLayer> gcode_layers(const std::vector<GcodeMove>& moves)
{
    std::vector<GcodeLayer> layers;
    if (moves.empty())
        return layers;
    const unsigned count = moves.back().layer_id + 1;
    layers.reserve(count);
    size_t begin = 0;
    double z     = 0.;
    for (unsigned id = 0; id < count; ++id) {
        const auto end = std::partition_point(moves.begin() + begin, moves.end(), [id](const GcodeMove& m) { return m.layer_id <= id; });
        GcodeLayer layer;
        layer.begin = begin;
        layer.end   = size_t(end - moves.begin());
        layer.z = z = layer_height_of(moves, layer.begin, layer.end, z);
        layers.push_back(layer);
        begin = layer.end;
    }
    return layers;
}

size_t layer_by_number(const std::vector<GcodeLayer>& layers, int number)
{
    if (layers.empty())
        throw std::runtime_error("the plate's G-code has no layers");
    if (number < 1 || size_t(number) > layers.size())
        throw std::runtime_error("layer " + std::to_string(number) + " does not exist: this plate's G-code has layers 1.." +
                                 std::to_string(layers.size()) + ", numbered as the Preview's layer slider numbers them");
    return size_t(number - 1);
}

LayerAtHeight layer_nearest_z(const std::vector<GcodeLayer>& layers, double z)
{
    if (layers.empty())
        throw std::runtime_error("the plate's G-code has no layers");
    // G-code heights come from floats written to a thousandth, so two distances this close are the same
    // distance, and a height between two layers goes to the lower one.
    const double  same = k_gcode_height_tolerance / 2.;
    LayerAtHeight hit;
    double        best = std::numeric_limits<double>::max();
    for (size_t i = 0; i < layers.size(); ++i) {
        const double d = std::abs(layers[i].z - z);
        const bool   closer = d < best - same;
        const bool   tie_lower = std::abs(d - best) <= same && layers[i].z < layers[hit.index].z;
        if (closer || tie_lower) {
            best      = d;
            hit.index = i;
        }
    }
    for (size_t i = 0; i < layers.size(); ++i)
        if (i != hit.index && std::abs(layers[i].z - layers[hit.index].z) < same)
            hit.also_at.push_back(int(i) + 1);
    return hit;
}

// --- the request -----------------------------------------------------------------------------

bool LayerPlanRequest::draws(ExtrusionFeature feature) const
{
    return size_t(feature) < k_plan_feature_count && features[size_t(feature)];
}

bool LayerPlanRequest::draws(int filament) const
{
    return filaments.empty() || std::binary_search(filaments.begin(), filaments.end(), filament);
}

namespace {

const char* const k_layer_view_forms = R"(layer_view must be "first_layer", {"layer": n} or {"z": mm})";

// layer_view's object form, {layer | z, features?, filaments?, color_by?, fit?}.
LayerPlanRequest parse_sliced_layer_view(const nlohmann::json& view)
{
    static const char* const k_keys[] = {"layer", "z", "features", "filaments", "color_by", "fit"};
    for (const auto& [key, value] : view.items())
        if (std::find(std::begin(k_keys), std::end(k_keys), key) == std::end(k_keys))
            throw std::runtime_error("layer_view has no key \"" + key + "\"; it takes layer or z, and optionally features, "
                                     "filaments, color_by and fit");

    LayerPlanRequest request;
    const bool has_layer = view.contains("layer") && !view["layer"].is_null();
    const bool has_z     = view.contains("z") && !view["z"].is_null();
    if (has_layer == has_z)
        throw std::runtime_error(has_layer ? "layer_view takes layer or z, not both"
                                           : "layer_view needs layer (the Preview slider's layer number, from 1) or z (a height in mm)");
    if (has_layer) {
        int number = 0;
        if (!parse_integer_param(view["layer"], number))
            throw std::runtime_error("layer must be a whole number: the Preview slider's layer number, from 1");
        request.layer = number;
    } else {
        double z = 0.;
        if (!parse_double_param(view["z"], z))
            throw std::runtime_error("z must be a height in mm");
        request.z = z;
    }
    if (view.contains("features"))
        request.features = parse_features(view["features"]);
    if (view.contains("filaments"))
        request.filaments = parse_filaments(view["filaments"]);
    if (view.contains("color_by")) {
        const nlohmann::json& c = view["color_by"];
        if (c == "feature")
            request.color_by = LayerColorBy::feature;
        else if (c == "filament")
            request.color_by = LayerColorBy::filament;
        else
            throw std::runtime_error("color_by must be \"feature\" or \"filament\"");
    }
    if (view.contains("fit"))
        request.fit_object = parse_fit(view["fit"]);
    return request;
}

} // namespace

LayerView parse_layer_view(const nlohmann::json& value)
{
    LayerView view;
    if (value == "first_layer") {
        view.first_layer = true;
        return view;
    }
    nlohmann::json object;
    if (!parse_object_param(value, object))
        throw std::runtime_error(k_layer_view_forms);
    view.sliced = parse_sliced_layer_view(object);
    return view;
}

void check_filaments(const LayerPlanRequest& request, size_t filament_count)
{
    for (int filament : request.filaments)
        if (size_t(filament) > filament_count)
            throw std::runtime_error("filament " + std::to_string(filament) + " does not exist: this printer's filaments are 1.." +
                                     std::to_string(filament_count));
}

nlohmann::json drawn_json(const LayerPlanRequest& request)
{
    nlohmann::json features = nlohmann::json::array();
    for (size_t f = 0; f < k_plan_feature_count; ++f)
        if (request.features[f])
            features.push_back(extrusion_feature_key(ExtrusionFeature(f)));
    return {{"features", features},
            {"filaments", request.filaments.empty() ? nlohmann::json("all") : nlohmann::json(request.filaments)},
            {"color_by", request.color_by == LayerColorBy::filament ? "filament" : "feature"}};
}

// --- toolpaths -------------------------------------------------------------------------------

std::vector<ToolpathRun> layer_toolpaths(const std::vector<GcodeMove>& moves, const GcodeLayer& layer, const LayerPlanRequest& request)
{
    std::vector<ToolpathRun> runs;
    bool                     open = false;  // the last run takes the next extrusion if it matches
    for (size_t i = std::max<size_t>(layer.begin, 1); i < layer.end; ++i) {
        const GcodeMove& m = moves[i];
        if (!is_feature_extrusion(m)) {
            open = false;
            continue;
        }
        const ExtrusionFeature feature  = extrusion_feature_of(m.extrusion_role);
        const int              filament = filament_of(m);
        if (!request.draws(feature) || !request.draws(filament)) {
            open = false;
            continue;
        }
        const bool continues = open && runs.back().feature == feature && runs.back().filament == filament &&
                               std::abs(runs.back().width - m.width) < 1e-3f;
        if (!continues) {
            runs.push_back({feature, filament, m.width, {xy_of(moves[i - 1])}});
            open = true;
        }
        runs.back().points.push_back(xy_of(m));
    }
    return runs;
}

// --- numbers ---------------------------------------------------------------------------------

double LayerExtrusion::object_mm2() const
{
    return mm2[size_t(ExtrusionFeature::perimeters)] + mm2[size_t(ExtrusionFeature::infill)];
}

double LayerExtrusion::support_mm2() const
{
    return mm2[size_t(ExtrusionFeature::support)] + mm2[size_t(ExtrusionFeature::support_interface)];
}

LayerExtrusion layer_extrusion(const std::vector<GcodeMove>& moves, const GcodeLayer& layer)
{
    LayerExtrusion e;
    for (size_t i = std::max<size_t>(layer.begin, 1); i < layer.end; ++i) {
        const GcodeMove& m = moves[i];
        if (!is_feature_extrusion(m))
            continue;
        const ExtrusionFeature feature  = extrusion_feature_of(m.extrusion_role);
        const int              filament = filament_of(m);
        const Vec2d            from = xy_of(moves[i - 1]), to = xy_of(m);
        const double           mm2 = (to - from).norm() * double(m.width);
        e.mm2[size_t(feature)] += mm2;
        e.mm2_by_filament[filament][size_t(feature)] += mm2;
        if (std::find(e.filament_order.begin(), e.filament_order.end(), filament) == e.filament_order.end())
            e.filament_order.push_back(filament);
        if (is_object_or_support(feature)) {
            e.extent.merge(from);
            e.extent.merge(to);
        }
    }
    return e;
}

nlohmann::json layer_extrusion_json(const LayerExtrusion& e)
{
    nlohmann::json by_filament = nlohmann::json::object();
    for (const auto& [filament, areas] : e.mm2_by_filament)
        by_filament[std::to_string(filament)] = feature_areas_json(areas);
    return {{"extruded_mm2", feature_areas_json(e.mm2)},
            {"extruded_mm2_by_filament", by_filament},
            {"object_mm2", round_to(e.object_mm2(), 2)},
            {"support_mm2", round_to(e.support_mm2(), 2)},
            {"filaments", e.filament_order}};
}

// --- the Print at that height ----------------------------------------------------------------

std::optional<size_t> layer_index_at_height(const std::vector<double>& print_zs, double z, double tolerance)
{
    std::optional<size_t> best;
    for (auto it = std::lower_bound(print_zs.begin(), print_zs.end(), z - tolerance); it != print_zs.end() && *it <= z + tolerance; ++it)
        if (!best || std::abs(*it - z) < std::abs(print_zs[*best] - z))
            best = size_t(it - print_zs.begin());
    return best;
}

double support_contact_z(const PrintObject& object, const Layer& layer)
{
    const SlicingParameters& sp = object.slicing_parameters();
    // No gap for a zero-gap interface (a soluble one, say); the configured gap otherwise, which every
    // generator keeps clear: the normal one trims what its layer sync leaves inside it
    // (trim_support_layers_by_object), and the trees end their tips there.
    const double z = layer.bottom_z() - (sp.zero_gap_interface_top ? 0. : sp.gap_support_object);
    // A contact too close to the bed or the raft is printed on it (SupportMaterial.cpp, new_contact_layer).
    const double lowest = sp.raft_layers() > 1 ? sp.raft_contact_top_z : sp.first_print_layer_height;
    return z < lowest + sp.min_layer_height ? lowest : z;
}

double overhang_tolerance(const Layer& layer)
{
    // PrintRegion::flow's own lookup: the nozzle of the filament slot the outer wall is printed with.
    const ConfigOptionFloats& nozzles = layer.object()->print()->config().nozzle_diameter;
    double                    nozzle  = 0.;
    for (const LayerRegion* region : layer.regions())
        if (!region->slices.empty()) {
            const double d = nozzles.get_at(region->region().extruder(frExternalPerimeter) - 1);
            nozzle         = nozzle > 0. ? std::min(nozzle, d) : d;
        }
    return (nozzle > 0. ? nozzle : nozzles.get_at(0)) / 2.;
}

std::optional<Overhang> overhang_of(const PrintObject& object, size_t layer_index)
{
    const auto& layers = object.layers();
    if (layer_index == 0 || layer_index >= layers.size())
        return std::nullopt;
    const Layer& layer = *layers[layer_index];
    const Layer& below = *layers[layer_index - 1];

    Overhang o;
    o.tolerance_mm        = overhang_tolerance(layer);
    const ExPolygons hang = diff_ex(layer.lslices, offset_ex(below.lslices, float(scale_(o.tolerance_mm))));
    o.area_mm2            = area_mm2(hang);
    o.contact_z           = support_contact_z(object, layer);
    if (hang.empty())
        return o;

    // Down from the highest support layer at or below the contact height, to the first with lines under
    // the overhang: the contact when it is within one of its own layers of that height (variable layers
    // and merged contacts end it up to a layer lower), else only the nearest support under it.
    const BoundingBox hang_box = get_extents(hang);
    const auto&       supports = object.support_layers();
    for (size_t i = supports.size(); i > 0; --i) {
        const SupportLayer& s = *supports[i - 1];
        if (s.print_z > o.contact_z + k_gcode_height_tolerance || !support_lines_over(s, hang, hang_box))
            continue;
        if (s.print_z < o.contact_z - s.height - k_gcode_height_tolerance) {
            o.nearest_support_z = s.print_z;
            break;
        }
        o.support_z           = s.print_z;
        o.under_support_mm2   = area_mm2(intersection_ex(hang, support_covered(s)));
        o.under_interface_mm2 = area_mm2(intersection_ex(hang, support_covered(s, Point(0, 0), erSupportMaterialInterface)));
        break;
    }
    return o;
}

std::vector<ObjectAtHeight> objects_at_height(const Print& print, const Model& model, double z, const BoundingBoxf& extent)
{
    const bool                  by_object = print.config().print_sequence == PrintSequence::ByObject;
    std::vector<ObjectAtHeight> out;
    for (const PrintObject* po : print.objects()) {
        const std::optional<size_t> oi = index_at(po->layers(), z);
        const std::optional<size_t> si = index_at(po->support_layers(), z);
        if (!oi && !si)
            continue;
        if (by_object) {
            // Every object prints this height in its own pass; this G-code layer is one of them.
            const BoundingBox shape = oi ? get_extents(po->layers()[*oi]->lslices)
                                         : get_extents(po->support_layers()[*si]->support_fills.polygons_covered_by_width());
            if (!lies_under(*po, shape, extent))
                continue;
        }
        ObjectAtHeight o;
        o.object_index = model_object_index(model, po->model_object());
        o.name         = po->model_object() != nullptr ? po->model_object()->name : std::string();
        if (oi) {
            o.object_layer = ref_of(*po->layers()[*oi], *oi);
            o.overhang     = overhang_of(*po, *oi);
        }
        if (si)
            o.support_layer = ref_of(*po->support_layers()[*si], *si);
        out.push_back(std::move(o));
    }
    return out;
}

nlohmann::json objects_at_height_json(const std::vector<ObjectAtHeight>& objects)
{
    nlohmann::json out = nlohmann::json::array();
    for (const ObjectAtHeight& o : objects)
        out.push_back({{"object_index", o.object_index},
                       {"name", o.name},
                       {"object_layer", layer_ref_json(o.object_layer)},
                       {"support_layer", layer_ref_json(o.support_layer)},
                       {"overhang", overhang_json(o.overhang)}});
    return out;
}

std::vector<PrintFootprint> print_footprints(const Print& print, const Model& model)
{
    std::vector<PrintFootprint> out;
    for (const PrintObject* po : print.objects()) {
        PrintFootprint f;
        f.object_index = model_object_index(model, po->model_object());
        f.name         = po->model_object() != nullptr ? po->model_object()->name : std::string();
        merge_on_bed(f.box, *po, po->bounding_box());  // the sliced object's size, centred on each instance
        out.push_back(std::move(f));
    }
    return out;
}

std::optional<BoundingBoxf> object_frame(const Print& print, const Model& model, int object_index, double z)
{
    const PrintObject* po = print_object_of(print, model, object_index);
    if (po == nullptr)
        return std::nullopt;
    BoundingBoxf frame;
    merge_on_bed(frame, *po, po->bounding_box());
    if (const std::optional<size_t> oi = index_at(po->layers(), z))
        merge_on_bed(frame, *po, get_extents(po->layers()[*oi]->lslices));
    if (const std::optional<size_t> si = index_at(po->support_layers(), z)) {
        Points points;  // tree support's feet and branches reach well past the part
        po->support_layers()[*si]->support_fills.collect_points(points);
        if (!points.empty())
            merge_on_bed(frame, *po, BoundingBox(points));
    }
    if (std::abs(z - first_print_height(print)) < k_gcode_height_tolerance)  // brim and raft: the first-layer plan's
        for (const PlanObject& o : plan_from_print(print, model).objects)
            if (o.object_index == object_index) {
                merge_mm(frame, get_extents(o.on_bed()));
                merge_mm(frame, get_extents(o.brim));
            }
    return frame;
}

nlohmann::json layer_json(const std::vector<GcodeLayer>& layers, const LayerAtHeight& chosen, const LayerPlanRequest& request)
{
    nlohmann::json out = {{"number", int(chosen.index) + 1}, {"of", layers.size()}, {"z", round_to(layers[chosen.index].z, 4)}};
    if (request.z) {
        out["requested_z"] = *request.z;
        out["also_at"]     = chosen.also_at;
    }
    return out;
}

// --- colour and legend -----------------------------------------------------------------------

ColorRGBA run_color(const ToolpathRun& run, LayerColorBy color_by, const std::vector<ColorRGBA>& slot_colors)
{
    if (color_by == LayerColorBy::feature)
        return extrusion_feature_color(run.feature);
    if (run.filament >= 1 && size_t(run.filament) <= slot_colors.size())
        return slot_colors[size_t(run.filament - 1)];
    return ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
}

std::vector<LegendEntry> layer_legend(const LayerPlanRequest& request, const LayerExtrusion& e, const std::vector<ColorRGBA>& slot_colors)
{
    // The area each drawn (filament, feature) pair laid down.
    auto drawn_mm2 = [&](int filament, size_t feature) {
        const auto it = e.mm2_by_filament.find(filament);
        return it != e.mm2_by_filament.end() && request.draws(filament) && request.features[feature] ? it->second[feature] : 0.;
    };
    std::vector<LegendEntry> legend;
    if (request.color_by == LayerColorBy::feature) {
        for (size_t f = 0; f < k_plan_feature_count; ++f) {
            double mm2 = 0.;
            for (const auto& [filament, areas] : e.mm2_by_filament)
                mm2 += drawn_mm2(filament, f);
            if (mm2 > 0.)
                legend.push_back({extrusion_feature_key(ExtrusionFeature(f)), feature_label(ExtrusionFeature(f)),
                                  extrusion_feature_color(ExtrusionFeature(f)), mm2});
        }
        return legend;
    }
    for (const auto& [filament, areas] : e.mm2_by_filament) {
        double mm2 = 0.;
        for (size_t f = 0; f < k_plan_feature_count; ++f)
            mm2 += drawn_mm2(filament, f);
        if (mm2 <= 0.)
            continue;
        ToolpathRun probe;
        probe.filament = filament;
        legend.push_back({std::to_string(filament), "Filament " + std::to_string(filament),
                          run_color(probe, LayerColorBy::filament, slot_colors), mm2});
    }
    return legend;
}

nlohmann::json legend_json(const std::vector<LegendEntry>& legend)
{
    nlohmann::json out = nlohmann::json::array();
    for (const LegendEntry& entry : legend)
        out.push_back({{"key", entry.key}, {"label", entry.label}, {"color", encode_color(entry.color)},
                       {"extruded_mm2", round_to(entry.mm2, 2)}});
    return out;
}

// --- drawing ---------------------------------------------------------------------------------

wxImage draw_layer_plan(const std::vector<ToolpathRun>&   runs,
                        LayerColorBy                      color_by,
                        const std::vector<ColorRGBA>&     slot_colors,
                        const std::vector<LegendEntry>&   legend,
                        const PlanMapping&                mapping,
                        const BoundingBoxf3&              plate,
                        const std::vector<BoundingBoxf3>& excluded_areas,
                        const std::vector<OverlayLabel>&  labels,
                        const OverlayOptions&             overlays)
{
    wxImage image = paint_plan(mapping, [&](wxGraphicsContext& gc) {
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        for (const ToolpathRun& run : runs) {
            if (run.points.size() < 2)
                continue;
            // At its real width, so dense infill reads as a filled area and a gap as a gap.
            const double width_px = std::max(1.0, double(run.width) * mapping.scale);
            gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(to_wx_colour(run_color(run, color_by, slot_colors))).Width(width_px).Cap(wxCAP_BUTT).Join(wxJOIN_ROUND)));
            wxGraphicsPath path = gc.CreatePath();
            for (size_t i = 0; i < run.points.size(); ++i) {
                const Vec2d px = mapping.to_px(run.points[i]);
                if (i == 0) path.MoveToPoint(px.x(), px.y()); else path.AddLineToPoint(px.x(), px.y());
            }
            gc.StrokePath(path);
        }
    });
    draw_overlays(image, plan_camera(mapping), plate, excluded_areas, labels, overlays, plan_background());
    if (overlays.labels && !legend.empty())
        draw_legend(image, legend);
    return image;
}

}}} // namespace Slic3r::GUI::OrcaMCP
