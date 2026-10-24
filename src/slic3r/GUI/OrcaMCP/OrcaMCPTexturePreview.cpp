// src/slic3r/GUI/OrcaMCP/OrcaMCPTexturePreview.cpp -- see the header.
#include "OrcaMCPTexturePreview.hpp"
#include "OrcaMCPRelief.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <cereal/archives/binary.hpp>
#include <cereal/types/utility.hpp>

#include "libslic3r/TextureDisplacement.hpp"
#include "slic3r/GUI/TextureDisplacementObject.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

TextureDisplacementBakeInput preview_input(TextureDisplacementBakeInput bake_input, bool keep_budget)
{
    for (TextureDisplacementBakeVolume& volume : bake_input.volumes)
        if (volume.options.pipeline_v2) {
            int& budget = volume.options.v2_max_triangles_k;
            if (!keep_budget || budget == 0 || budget > max_part_triangles_k)
                budget = max_part_triangles_k;
        }
    return bake_input;
}

double suggested_resolution_mm(double current_mm, std::size_t refined, std::size_t cap)
{
    if (current_mm <= 0. || refined == 0 || cap == 0)
        return 0.;
    const double fit = std::ceil(current_mm * std::sqrt(double(refined) / double(cap)) * 1.05 * 100.) / 100.;   // 5 % over: the count is not exactly 1/edge^2
    return fit >= 0.02 && fit <= 2. ? fit : 0.;
}

namespace {
bool same_palette(const std::vector<PrintableColor>& a, const std::vector<PrintableColor>& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const PrintableColor& x, const PrintableColor& y) {
               return x.rgb == y.rgb && x.a == y.a && x.b == y.b && x.num == y.num && x.den == y.den;
           });
}

template<class T> std::string serialized(const T& value)
{
    std::ostringstream os(std::ios::binary);
    {
        cereal::BinaryOutputArchive archive(os);
        archive(value);
    }
    return os.str();
}

// Two 64-bit lanes over the bytes fed to it: a collision needs both to agree.
struct Hasher
{
    std::uint64_t a = 0xcbf29ce484222325ull, b = 0x9e3779b97f4a7c15ull;

    void word(std::uint64_t w)
    {
        a = (a ^ w) * 0x100000001b3ull;
        a ^= a >> 32;
        b = (b + w + 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull;
        b ^= b >> 29;
    }
    void bytes(const void* data, std::size_t size)
    {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (; size >= 8; size -= 8, p += 8) {
            std::uint64_t w;
            std::memcpy(&w, p, 8);
            word(w);
        }
        std::uint64_t tail = size;
        for (std::size_t i = 0; i < size; ++i)
            tail = (tail << 8) ^ p[i];
        word(tail + 0x1234567ull);
    }
    template<class T> void pod(const T& value) { bytes(&value, sizeof value); }
    template<class T> void vec(const std::vector<T>& v) { word(v.size()); if (!v.empty()) bytes(v.data(), v.size() * sizeof(T)); }
    void text(const std::string& s) { word(s.size()); bytes(s.data(), s.size()); }
    void bits(const std::vector<bool>& v)
    {
        word(v.size());
        std::uint64_t w = 0;
        int           n = 0;
        for (bool bit : v) {
            w = (w << 1) | (bit ? 1u : 0u);
            if (++n == 64) { word(w); w = 0; n = 0; }
        }
        word(w);
    }
    void splitting(const TriangleSelector::TriangleSplittingData& d)
    {
        word(d.triangles_to_split.size());
        for (const auto& t : d.triangles_to_split) {
            word(std::uint64_t(std::uint32_t(t.triangle_idx)));
            word(std::uint64_t(std::uint32_t(t.bitstream_start_idx)));
        }
        bits(d.bitstream);
        bits(d.used_states);
    }
};

void hash_options(Hasher& h, TextureDisplacementOptions options, bool keep_budget)
{
    options.active_slot = options.panel_mode = 0;
    if (options.pipeline_v2 && !keep_budget)
        options.v2_max_triangles_k = 0;
    h.text(serialized(options));
}

void hash_layers(Hasher& h, const std::vector<TextureDisplacementLayer>& layers)
{
    h.word(layers.size());
    for (const TextureDisplacementLayer& layer : layers) {
        h.pod(layer.preview_adjust.brightness);
        h.pod(layer.preview_adjust.contrast);
        h.pod(layer.preview_adjust.sharpness);
        h.pod(layer.preview_adjust.blur);
        h.pod(layer.preview_adjust.hue_deg);
        if (layer.image_data)
            h.vec(*layer.image_data);
        else
            h.word(0xdeadull);
        TextureDisplacementLayer without = layer;
        without.image_data.reset();
        h.text(serialized(without));
    }
}
} // namespace

bool same_preview_input(const TextureDisplacementBakeInput& a, const TextureDisplacementBakeInput& b)
{
    if (a.volumes.size() != b.volumes.size() || !same_palette(a.color.palette, b.color.palette) ||
        !same_palette(a.color.palette_pure, b.color.palette_pure) || a.color.despeckle_passes != b.color.despeckle_passes)
        return false;
    for (std::size_t i = 0; i < a.volumes.size(); ++i)
        if (!same_bake_volume(a.volumes[i], b.volumes[i], /*ignore_preview_budget=*/true))
            return false;
    return true;
}

PreviewKey preview_key(const TextureDisplacementBakeInput& input, const PreviewColours& colours, int classic_budget_k, bool keep_budget)
{
    Hasher h;
    h.word(std::uint64_t(std::uint32_t(classic_budget_k)));
    h.word(keep_budget ? 1 : 0);
    h.word(input.volumes.size());
    for (const PrintableColor& c : input.color.palette) { h.pod(c.rgb); h.pod(c.a); h.pod(c.b); h.pod(c.num); h.pod(c.den); }
    h.word(input.color.palette.size());
    for (const PrintableColor& c : input.color.palette_pure) { h.pod(c.rgb); h.pod(c.a); h.pod(c.b); h.pod(c.num); h.pod(c.den); }
    h.pod(input.color.despeckle_passes);
    for (const TextureDisplacementBakeVolume& v : input.volumes) {
        h.word(v.volume_id.id);
        h.pod(v.resting_drop);
        h.pod(v.texture_anchor);
        h.pod(v.volume_to_world.matrix());
        h.vec(v.model_uv);
        h.vec(v.base_mesh.vertices);
        h.vec(v.base_mesh.indices);
        for (const auto& mask : v.facets_data)
            h.splitting(mask);
        hash_layers(h, v.layers);
        hash_options(h, v.options, keep_budget);
    }
    h.word(colours.colour_paint.size());
    for (const auto& paint : colours.colour_paint)
        h.splitting(paint);
    for (const ColorRGBA& c : colours.extruders)
        h.pod(c);
    return {h.a, h.b};
}

std::size_t PreviewDraw::triangles() const
{
    std::size_t total = 0;
    for (const PreviewPart& part : parts)
        total += part.mesh.indices.size();
    return total;
}

std::size_t PreviewDraw::bytes() const
{
    std::size_t total = 0;
    for (const PreviewPart& part : parts)
        total += part.mesh.vertices.size() * sizeof(Vec3f) + part.mesh.indices.size() * sizeof(stl_triangle_vertex_indices) + part.runs.size() * sizeof(PreviewRun);
    return total;
}

PreviewDraw make_preview_draw(TextureBakePlan&& plan, const TextureDisplacementBakeInput& original, const std::vector<int>& volume_ids,
                              const PreviewColours& colours, bool keep_budget)
{
    PreviewDraw draw;
    // Colour slots: 0 the part's own colour, 1..P the palette (the texture's colours), then one per extruder state.
    std::vector<ColorRGBA> table(1, ColorRGBA());
    for (const PrintableColor& entry : original.color.palette)
        table.emplace_back(entry.rgb.x(), entry.rgb.y(), entry.rgb.z(), 1.f);
    const std::size_t palette = original.color.palette.size();
    for (const ColorRGBA& extruder : colours.extruders)
        table.push_back(extruder);

    for (std::size_t i = 0; i < plan.results.size() && i < original.volumes.size(); ++i) {
        PreviewPart part;
        part.volume_id      = i < volume_ids.size() ? volume_ids[i] : -1;
        part.triangles_base = original.volumes[i].base_mesh.indices.size();
        indexed_triangle_set mesh = std::move(plan.results[i].mesh.its);
        part.triangles = mesh.indices.size();
        const TextureBakeStats&               stats   = plan.results[i].stats;
        const TextureDisplacementBakeVolume&  source  = original.volumes[i];
        part.triangles_unsimplified = stats.triangles_refined;
        const int budget_k = !source.options.pipeline_v2 ? 0 : !keep_budget ? max_part_triangles_k
                           : (source.options.v2_max_triangles_k == 0 || source.options.v2_max_triangles_k > max_part_triangles_k) ? max_part_triangles_k
                           : source.options.v2_max_triangles_k;
        part.simplified_to_cap = budget_k == max_part_triangles_k && stats.triangles_refined > max_part_triangles;
        if (stats.triangles_refined > max_part_triangles) {
            const double edge = source.options.v2_refine_mm > 0.f
                ? double(source.options.v2_refine_mm)
                : std::max(0.05, double(recommend_v2_resolution(source.base_mesh, source.layers, source.volume_to_world).edge_mm));
            part.suggested_resolution_mm = suggested_resolution_mm(edge, stats.triangles_refined, max_part_triangles);
        }
        if (mesh.indices.empty()) {
            draw.parts.push_back(std::move(part));
            continue;   // this part's layers moved nothing: its own mesh is drawn
        }
        if (mesh.indices.size() > max_part_triangles) {
            part.skipped = "too_large";
            draw.parts.push_back(std::move(part));
            continue;
        }
        // The slot of each triangle: the texture's colour, else the part's colour paint carried onto it, else its own colour.
        std::vector<std::uint16_t> slot(mesh.indices.size(), 0);
        const std::vector<std::uint8_t>& texture = plan.results[i].triangle_color;
        std::vector<std::uint8_t>        painted;
        if (i < colours.colour_paint.size() && !colours.colour_paint[i].bitstream.empty() && !colours.extruders.empty())
            painted = transfer_facet_paint(original.volumes[i].base_mesh, colours.colour_paint[i], mesh);
        for (std::size_t t = 0; t < slot.size(); ++t) {
            if (t < texture.size() && texture[t] > 0 && std::size_t(texture[t]) <= palette)
                slot[t] = std::uint16_t(texture[t]);
            else if (t < painted.size() && painted[t] > 0 && std::size_t(painted[t]) <= colours.extruders.size())
                slot[t] = std::uint16_t(palette + painted[t]);
        }
        // One counting sort into runs; only the index buffer is permuted, never the vertices.
        std::vector<std::size_t> count(table.size() + 1, 0);
        for (const std::uint16_t s : slot)
            ++count[std::size_t(s) + 1];
        std::vector<std::size_t> start(table.size() + 1, 0);
        for (std::size_t s = 0; s < table.size(); ++s)
            start[s + 1] = start[s] + count[s + 1];
        part.mesh.vertices = std::move(mesh.vertices);
        part.mesh.indices.resize(mesh.indices.size());
        std::vector<std::size_t> next(start.begin(), start.end() - 1);
        for (std::size_t t = 0; t < slot.size(); ++t)
            part.mesh.indices[next[slot[t]]++] = mesh.indices[t];
        for (std::size_t s = 0; s < table.size(); ++s)
            if (start[s + 1] > start[s])
                part.runs.push_back({{start[s] * 3, start[s + 1] * 3}, table[s], s == 0});
        draw.parts.push_back(std::move(part));
    }
    return draw;
}

PreviewResult compute_preview(const TextureDisplacementBakeInput& captured, const TextureDisplacementPrepareInput* prepare,
                              const std::vector<int>& volume_ids, const PreviewColours& colours,
                              const std::function<bool(int)>& progress, bool keep_budget)
{
    PreviewResult result;
    TextureDisplacementBakeInput work = preview_input(captured, keep_budget);
    if (prepare != nullptr && !prepare->volumes.empty()) {
        // The classic bake prepares first (remesh, adaptive refinement) and displaces what that leaves; the preview does
        // the same on a copy, the paint carried exactly as the preparation carries it.
        const TextureDisplacementPreparePlan prepared = plan_texture_prepare(*prepare, progress);
        if (prepared.canceled) {
            result.canceled = true;
            return result;
        }
        bool lost = false, applied = false;
        for (const TextureDisplacementPrepareResult& r : prepared.results)
            lost = lost || r.paint_lost;
        if (!lost)
            for (std::size_t p = 0; p < prepared.results.size() && p < prepare->volumes.size(); ++p) {
                if (prepared.results[p].mesh.indices.empty())
                    continue;
                for (TextureDisplacementBakeVolume& volume : work.volumes)
                    if (volume.volume_id == prepare->volumes[p].volume_id) {
                        volume.base_mesh   = prepared.results[p].mesh;
                        volume.facets_data = prepared.results[p].masks;
                        volume.model_uv    = prepared.results[p].uv;
                        applied            = true;
                    }
            }
        result.draw.preparation = lost ? "paint_lost" : applied ? "applied" : "unchanged";
    }
    TextureBakePlan plan = plan_texture_bake(work, progress);
    if (plan.canceled) {
        result.canceled = true;
        return result;
    }
    std::string preparation = std::move(result.draw.preparation);
    result.draw             = make_preview_draw(std::move(plan), captured, volume_ids, colours, keep_budget);
    result.draw.preparation = std::move(preparation);
    return result;
}

PreviewStamp capture_preview_stamp(const ModelObject& object)
{
    PreviewStamp stamp;
    ModelObject& mutable_object = const_cast<ModelObject&>(object);
    const std::vector<ModelVolume*> parts = painted_texture_parts(mutable_object);
    const std::vector<float>        drops = resting_drops(object, parts);
    stamp.anchor = texture_anchor(object);
    if (const ModelVolume* owner = texture_stack_owner(object))
        stamp.options = owner->texture_displacement_options;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        PreviewStamp::Part part;
        part.id       = parts[i]->id();
        part.mesh     = parts[i]->mesh_ptr();
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
            part.masks[std::size_t(slot)] = parts[i]->texture_displacement_facet(slot).timestamp();
        part.to_world = texture_displacement_volume_to_world(*parts[i]);
        part.layers   = layers_for_part(object, *parts[i]);
        part.drop     = drops[i];
        part.uv       = parts[i]->texture_uv.size();
        stamp.parts.push_back(std::move(part));
    }
    return stamp;
}

bool preview_stamp_current(const ModelObject& object, const PreviewStamp& stamp)
{
    const PreviewStamp now = capture_preview_stamp(object);
    if (now.parts.size() != stamp.parts.size() || now.anchor != stamp.anchor || !same_bake_options(now.options, stamp.options))
        return false;
    for (std::size_t i = 0; i < now.parts.size(); ++i) {
        const PreviewStamp::Part &a = now.parts[i], &b = stamp.parts[i];
        if (a.id != b.id || a.mesh != b.mesh || a.masks != b.masks || a.to_world.matrix() != b.to_world.matrix() || a.drop != b.drop ||
            a.uv != b.uv || !same_texture_layers(a.layers, b.layers))
            return false;
    }
    return true;
}

std::shared_ptr<const PreviewCache::Entry> PreviewCache::find(const ObjectID& object, const PreviewKey& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const std::shared_ptr<Entry>& entry : m_entries)
        if (entry->object == object && entry->key == key) {
            entry->used = ++m_clock;
            return entry;
        }
    return nullptr;
}

bool PreviewCache::put(const ObjectID& object, const PreviewKey& key, std::shared_ptr<const PreviewDraw> draw, double seconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&](const std::shared_ptr<Entry>& e) { return e->object == object; }),
                    m_entries.end());
    const std::size_t triangles = draw ? draw->triangles() : 0;
    if (!draw || triangles > max_triangles)
        return false;
    auto entry       = std::make_shared<Entry>();
    entry->object    = object;
    entry->key       = key;
    entry->draw      = std::move(draw);
    entry->triangles = triangles;
    entry->seconds   = seconds;
    entry->used      = ++m_clock;
    m_entries.push_back(std::move(entry));
    evict_to_caps();
    return true;
}

void PreviewCache::prune(const std::function<bool(const ObjectID&)>& exists)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&](const std::shared_ptr<Entry>& e) { return !exists(e->object); }),
                    m_entries.end());
}

void PreviewCache::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

std::size_t PreviewCache::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

std::size_t PreviewCache::triangles() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t total = 0;
    for (const std::shared_ptr<Entry>& entry : m_entries)
        total += entry->triangles;
    return total;
}

std::size_t PreviewCache::bytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t total = 0;
    for (const std::shared_ptr<Entry>& entry : m_entries)
        total += entry->draw->bytes();
    return total;
}

void PreviewCache::evict_to_caps()
{
    const auto held = [this] {
        std::size_t total = 0;
        for (const std::shared_ptr<Entry>& entry : m_entries)
            total += entry->triangles;
        return total;
    };
    while (m_entries.size() > max_objects || (m_entries.size() > 1 && held() > max_triangles)) {
        const auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
                                             [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { return a->used < b->used; });
        m_entries.erase(oldest);
    }
}

PreviewCache& texture_preview_cache()
{
    static PreviewCache cache;
    return cache;
}

void drop_texture_previews()
{
    texture_preview_cache().clear();
    drop_bake_references();   // the surfaces kept for color_by relief belong to the project that is gone too
}

std::string preview_timed_out_message(int object_id, double wait_s, bool narrowed)
{
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.0f", std::max(0., wait_s));
    return "the texture preview of object " + std::to_string(object_id) + " needs more than the call's wait (" + seconds +
           " s), so it is drawn unbaked: raise ORCAMCP_TIMEOUT" +
           (narrowed ? std::string() : ", or fit a view to one object ({object_id}), which previews only that object");
}

std::string preview_not_reached_message(int object_id, bool narrowed)
{
    return "the call's wait ran out on a slower object before the texture preview of object " + std::to_string(object_id) +
           " was started, so it is drawn unbaked: raise ORCAMCP_TIMEOUT" +
           (narrowed ? std::string() : ", or fit a view to this object ({object_id}), which previews only that object");
}

std::string preview_part_note(const PreviewPart& part)
{
    const auto number = [](double v) {
        std::ostringstream os;
        os << v;
        return os.str();
    };
    const std::string fit = part.suggested_resolution_mm > 0.
        ? "texture_preview: {resolution_mm: " + number(part.suggested_resolution_mm) + "} previews it unsimplified"
        : "clear_object_paint on the layers you do not need, or paint a smaller region, brings it under the cap";
    if (part.skipped == "too_large")
        return "this part's preview has " + std::to_string(part.triangles) + " triangles, more than the " + std::to_string(max_part_triangles) +
               " a render holds even simplified to that, so it is drawn unbaked: " + (part.triangles_unsimplified > 0 ? fit : "bake_texture_displacement it, or " + fit);
    if (part.simplified_to_cap)
        return "the unsimplified preview has " + std::to_string(part.triangles_unsimplified) + " triangles, more than the " + std::to_string(max_part_triangles) +
               " a render holds, so it is simplified to that (the bake's triangle budget set to the cap): the finest detail is merged. " + fit +
               "; texture_preview: {resolution_mm, budget_k} previews a bake planned with those settings";
    return std::string();
}

nlohmann::json preview_report_json(const PreviewReportInput& input, const PreviewCache& cache)
{
    using json = nlohmann::json;
    json objects   = json::array();
    int  previewed = 0;
    bool complete  = true;
    for (const PreviewObject& object : input.objects) {
        json entry = {{"object_id", object.object_id}, {"parts", json::array()}, {"cached", object.cached}};
        if (!object.draw) {
            complete = false;
            if (object.attempted) {
                entry["timed_out"] = true;
                entry["note"]      = preview_timed_out_message(object.object_id, input.wait_s, input.narrowed);
            } else {
                entry["not_reached"] = true;
                entry["note"]        = preview_not_reached_message(object.object_id, input.narrowed);
            }
            objects.push_back(std::move(entry));
            continue;
        }
        if (!object.undrawn.empty()) {
            complete = false;
            entry["undrawn"] = object.undrawn;
            entry["note"]    = object.undrawn == "over_render_total"
                ? "this preview is kept but not drawn: one render draws at most " + std::to_string(PreviewCache::max_triangles) + " preview triangles; fit a view to this object to see it"
                : "the call's time ran out before this preview could be uploaded, so it is drawn unbaked; render again (the preview is kept)";
            objects.push_back(std::move(entry));
            continue;
        }
        ++previewed;
        entry["seconds"] = std::round(object.seconds * 100.) / 100.;
        if (!object.draw->preparation.empty())
            entry["preparation"] = object.draw->preparation;
        for (const PreviewPart& part : object.draw->parts) {
            json p = {{"volume_id", part.volume_id}, {"triangles_base", part.triangles_base}, {"triangles_preview", part.triangles}};
            if (part.triangles_unsimplified > 0)
                p["triangles_unsimplified"] = part.triangles_unsimplified;
            if (part.simplified_to_cap)
                p["simplified_to_cap"] = true;
            if (!part.skipped.empty())
                p["skipped"] = part.skipped;
            if (const std::string note = preview_part_note(part); !note.empty())
                p["note"] = note;
            entry["parts"].push_back(std::move(p));
        }
        if (object.keep_budget && object.classic_budget_k > 0 && input.requested.is_object() && !input.requested.empty())
            entry["note"] = "this object is on the classic pipeline, whose preparation budget is classic_budget_k: the default pipeline's resolution_mm and budget_k were not used "
                            "(bake_texture_displacement takes classic_budget_k)";
        if (object.draw->preparation == "paint_lost")
            entry["note"] = "the classic bake's preparation (remesh) would lose the paint, so the bake refuses it; this shows the surface without the preparation";
        if (object.panel_open) {
            entry["panel_open"] = true;
            entry["note"]       = "the texture panel is open on this object: its strokes since the panel last wrote them into the model are not in this preview";
        }
        objects.push_back(std::move(entry));
    }
    json skipped = json::array();
    for (const PreviewSkip& skip : input.skipped) {
        json entry = {{"object_id", skip.object_id}, {"reason", skip.reason}};
        if (!skip.next_steps.empty())
            entry["next_steps"] = skip.next_steps;
        skipped.push_back(std::move(entry));
    }
    json report = {{"requested", input.requested},
                   {"previewed", previewed},
                   {"complete", complete},
                   {"objects", std::move(objects)},
                   {"skipped", std::move(skipped)},
                   {"no_texture_layers", input.without_layers},
                   {"cache", {{"objects", cache.size()}, {"triangles", cache.triangles()}, {"bytes", cache.bytes()},
                              {"max_objects", PreviewCache::max_objects}, {"max_triangles", PreviewCache::max_triangles}}}};
    if (!input.outside_fit.empty())
        report["outside_fit"] = input.outside_fit;
    if (previewed == 0 && complete)
        report["note"] = "nothing was previewed: no object on plate " + std::to_string(input.plate_index) +
                         (input.outside_fit.empty() ? " has texture layers with paint to show" : " inside the views' fit has texture layers with paint to show") +
                         ", so the picture is the plain one (skipped says why for each object that has layers)";
    return report;
}

}}} // namespace Slic3r::GUI::OrcaMCP
