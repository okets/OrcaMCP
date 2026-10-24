#include "OrcaMCPRenderPaint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "libslic3r/TriangleSelector.hpp"
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

double world_area_mm2(const indexed_triangle_set& its, const Transform3d& trafo)
{
    double area = 0.;
    for (const stl_triangle_vertex_indices& f : its.indices) {
        const Vec3d a = trafo * its.vertices[f[0]].cast<double>();
        const Vec3d b = trafo * its.vertices[f[1]].cast<double>();
        const Vec3d c = trafo * its.vertices[f[2]].cast<double>();
        area += 0.5 * (b - a).cross(c - a).norm();
    }
    return area;
}

std::vector<PaintFacets> compute_paint_facets(const std::vector<PaintVolumeInput>& inputs, const std::vector<PaintTint>& tints,
                                              const std::function<bool()>& stop)
{
    std::vector<PaintFacets> out;
    for (const PaintVolumeInput& in : inputs)
        for (const auto& [tint, data] : in.per_tint) {
            if (stop && stop())
                return out;
            if (in.mesh == nullptr || tint < 0 || size_t(tint) >= tints.size())
                continue;
            PaintFacets found;
            found.object_id = in.object_id;
            found.volume_id = in.volume_id;
            found.tint      = tint;
            found.its       = painted_facets(*in.mesh, data, tints[size_t(tint)].state);
            found.facets    = int(found.its.indices.size());
            if (found.facets == 0)
                continue;
            found.area_mm2 = world_area_mm2(found.its, in.to_plate);
            out.push_back(std::move(found));
        }
    return out;
}

std::string hex_color(const ColorRGBA& color)
{
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", int(color.r() * 255.f + 0.5f), int(color.g() * 255.f + 0.5f), int(color.b() * 255.f + 0.5f));
    return text;
}

nlohmann::json tint_legend_json(const std::vector<PaintTint>& tints, const std::vector<PaintFacets>& facets)
{
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
        legend.push_back(std::move(row));
    }
    return legend;
}

}}} // namespace Slic3r::GUI::OrcaMCP
