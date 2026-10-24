#include "OrcaMCPRenderPaint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

const std::vector<std::string>& paint_channel_names()
{
    static const std::vector<std::string> names = {"support", "seam", "fuzzy_skin", "texture"};
    return names;
}

ColorRGBA gizmo_enforcer_color() { return TriangleSelectorGUI::enforcers_color; }
ColorRGBA gizmo_blocker_color() { return TriangleSelectorGUI::blockers_color; }

ColorRGBA texture_layer_color(int slot)
{
    // Chosen to stay apart from object_palette_color's twelve, the gizmos' two and the neutral grey (tested).
    static const ColorRGBA table[8] = {
        {1.00f, 0.00f, 1.00f, 1.f}, {0.00f, 1.00f, 0.00f, 1.f}, {0.30f, 1.00f, 1.00f, 1.f}, {0.70f, 1.00f, 0.00f, 1.f},
        {1.00f, 0.45f, 1.00f, 1.f}, {1.00f, 0.00f, 0.00f, 1.f}, {0.00f, 1.00f, 0.50f, 1.f}, {0.90f, 0.90f, 0.40f, 1.f},
    };
    return table[std::clamp(slot, 0, 7)];
}

std::vector<PaintTint> paint_tints(const std::vector<std::string>& channels, const std::vector<int>& texture_slots,
                                   const std::vector<int>& slots_with_layers)
{
    const auto asked = [&](const std::string& name) { return std::find(channels.begin(), channels.end(), name) != channels.end(); };
    std::vector<PaintTint> tints;
    for (const std::string& channel : {std::string("support"), std::string("seam")})
        if (asked(channel)) {
            tints.push_back({channel, -1, 1, gizmo_enforcer_color(), channel + " enforcer"});
            tints.push_back({channel, -1, 2, gizmo_blocker_color(), channel + " blocker"});
        }
    if (asked("fuzzy_skin"))
        tints.push_back({"fuzzy_skin", -1, 1, gizmo_enforcer_color(), "fuzzy skin"});
    if (asked("texture")) {
        std::vector<int> slots = texture_slots.empty() ? slots_with_layers : texture_slots;
        std::sort(slots.begin(), slots.end());
        slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
        for (int slot : slots)
            tints.push_back({"texture", slot, 1, texture_layer_color(slot), "texture layer " + std::to_string(slot)});
    }
    return tints;
}

PaintChannel paint_channel_of(const PaintTint& tint)
{
    if (tint.channel == "support")    return PaintChannel(PaintMode::Support);
    if (tint.channel == "seam")       return PaintChannel(PaintMode::Seam);
    if (tint.channel == "fuzzy_skin") return PaintChannel(PaintMode::FuzzySkin);
    return PaintChannel::texture(tint.texture_layer);
}

std::string color_source_of(bool colour_painted, bool neutral_base, ColorBy color_by)
{
    if (colour_painted) return "paint";
    if (neutral_base)   return "neutral";
    return color_by == ColorBy::Filament ? "filament" : "palette";
}

ColorRGBA filament_color_for(int extruder_id, const std::vector<ColorRGBA>& colors)
{
    return colors[size_t(std::clamp(extruder_id - 1, 0, int(colors.size()) - 1))];
}

indexed_triangle_set painted_facets(const TriangleMesh& mesh, const PaintData& data, int state)
{
    TriangleSelector selector(mesh);
    selector.deserialize(data, false);
    return selector.get_facets(EnforcerBlockerType(state));
}

// The area of triangles [begin, end) of `its` after `trafo`.
static double area_range(const indexed_triangle_set& its, const Transform3d& trafo, size_t begin, size_t end)
{
    double area = 0.;
    for (size_t i = begin; i < end; ++i) {
        const stl_triangle_vertex_indices& f = its.indices[i];
        const Vec3d a = trafo * its.vertices[f[0]].cast<double>();
        const Vec3d b = trafo * its.vertices[f[1]].cast<double>();
        const Vec3d c = trafo * its.vertices[f[2]].cast<double>();
        area += 0.5 * (b - a).cross(c - a).norm();
    }
    return area;
}

double world_area_mm2(const indexed_triangle_set& its, const Transform3d& trafo)
{
    return area_range(its, trafo, 0, its.indices.size());
}

PaintResult compute_paint_facets(const std::vector<PaintVolumeInput>& inputs, const std::vector<PaintTint>& tints,
                                 const std::function<bool()>& stop)
{
    PaintResult out;
    const auto stopped = [&] { return stop && stop(); };
    for (const PaintVolumeInput& in : inputs) {
        if (in.mesh == nullptr)
            continue;
        // One selector per annotation: the tints of one channel (and texture layer) read the same data.
        std::vector<std::pair<std::pair<std::string, int>, std::unique_ptr<TriangleSelector>>> selectors;
        for (const auto& [tint_index, data] : in.per_tint) {
            if (tint_index < 0 || size_t(tint_index) >= tints.size())
                continue;
            const PaintTint& tint = tints[size_t(tint_index)];
            if (stopped()) {
                out.complete = false;
                return out;
            }
            // A mask indexing a triangle the mesh no longer has is unreadable (and upstream's bound check lets a negative index by).
            if (mask_is_stale(data, in.mesh->its.indices.size())) {
                out.stale.emplace_back(tint_index, in.object_id);
                continue;
            }
            const std::pair<std::string, int> key{tint.channel, tint.texture_layer};
            auto found = std::find_if(selectors.begin(), selectors.end(), [&](const auto& s) { return s.first == key; });
            if (found == selectors.end()) {
                auto selector = std::make_unique<TriangleSelector>(*in.mesh);
                selector->deserialize(data, false);
                selectors.emplace_back(key, std::move(selector));
                found = std::prev(selectors.end());
            }
            PaintFacets facets;
            facets.object_id = in.object_id;
            facets.volume_id = in.volume_id;
            facets.tint      = tint_index;
            facets.its       = found->second->get_facets(EnforcerBlockerType(tint.state));
            facets.facets    = int(facets.its.indices.size());
            if (facets.facets == 0)
                continue;
            // The area sum is bounded too: a million-facet part is a few hundred thousand triangles per tint.
            double area = 0.;
            for (size_t begin = 0; begin < facets.its.indices.size(); begin += 4096) {
                if (stopped()) {
                    out.complete = false;
                    return out;
                }
                area += area_range(facets.its, in.to_plate, begin, std::min(facets.its.indices.size(), begin + 4096));
            }
            facets.area_mm2 = area;
            out.facets.push_back(std::move(facets));
        }
    }
    return out;
}

std::string hex_color(const ColorRGBA& color)
{
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", int(color.r() * 255.f + 0.5f), int(color.g() * 255.f + 0.5f), int(color.b() * 255.f + 0.5f));
    return text;
}

nlohmann::json tint_legend_json(const std::vector<PaintTint>& tints, const PaintResult& result)
{
    const std::vector<PaintFacets>& facets = result.facets;
    nlohmann::json legend = nlohmann::json::array();
    for (size_t t = 0; t < tints.size(); ++t) {
        const PaintTint& tint = tints[t];
        // One row per object: its parts' facets and area added.
        std::vector<std::pair<int, std::pair<int, double>>> by_object;
        for (const PaintFacets& f : facets) {
            if (f.tint != int(t))
                continue;
            auto row = std::find_if(by_object.begin(), by_object.end(), [&](const auto& r) { return r.first == f.object_id; });
            if (row == by_object.end())
                row = by_object.insert(by_object.end(), {f.object_id, {0, 0.}});
            row->second.first += f.facets;
            row->second.second += f.area_mm2;
        }
        std::sort(by_object.begin(), by_object.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        nlohmann::json objects = nlohmann::json::array();
        for (const auto& [object_id, count] : by_object)
            objects.push_back({{"object_id", object_id}, {"facets", count.first}, {"area_mm2", std::round(count.second * 100.) / 100.}});
        nlohmann::json row = {{"channel", tint.channel}, {"state", tint.state},
                              {"state_name", tint.channel == "texture" ? "painted" : tint.channel == "fuzzy_skin" ? "fuzzy_skin" : tint.state == 2 ? "blocker" : "enforcer"},
                              {"label", tint.label}, {"color", hex_color(tint.color)}, {"objects", std::move(objects)}};
        if (tint.texture_layer >= 0)
            row["layer"] = tint.texture_layer;
        nlohmann::json stale = nlohmann::json::array();
        for (const auto& [stale_tint, object_id] : result.stale)
            if (stale_tint == int(t))
                stale.push_back(object_id);
        if (!stale.empty()) {
            row["stale"]         = true;
            row["stale_objects"] = std::move(stale);
        }
        legend.push_back(std::move(row));
    }
    return legend;
}

std::vector<std::string> tint_notes(const std::vector<std::string>& channels, const std::vector<PaintTint>& tints,
                                     const PaintResult& result)
{
    std::vector<std::string> notes;
    const bool texture_asked = std::find(channels.begin(), channels.end(), "texture") != channels.end();
    const bool texture_tints = std::any_of(tints.begin(), tints.end(), [](const PaintTint& t) { return t.channel == "texture"; });
    if (texture_asked && !texture_tints)
        notes.push_back("No object on the plate has a texture layer, so there is no texture paint to draw. get_texture_displacement lists "
                        "an object's layers; set_texture_displacement adds one.");
    if (!result.stale.empty())
        notes.push_back("A paint mask indexes triangles its mesh no longer has (the mesh was replaced after it was painted), so it is not "
                        "drawn: the legend marks it stale. Paint it again, or clear it.");
    return notes;
}

ColorRGBA readable_label_color(const ColorRGBA& color)
{
    const float luminance = 0.3f * color.r() + 0.59f * color.g() + 0.11f * color.b();
    constexpr float minimum = 0.35f;
    if (luminance >= minimum)
        return color;
    const float t = (minimum - luminance) / (1.f - luminance);
    return ColorRGBA(color.r() + (1.f - color.r()) * t, color.g() + (1.f - color.g()) * t, color.b() + (1.f - color.b()) * t, color.a());
}

}}} // namespace Slic3r::GUI::OrcaMCP
