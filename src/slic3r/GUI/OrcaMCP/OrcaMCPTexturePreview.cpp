// src/slic3r/GUI/OrcaMCP/OrcaMCPTexturePreview.cpp -- see the header.
#include "OrcaMCPTexturePreview.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

TextureDisplacementBakeInput preview_input(TextureDisplacementBakeInput bake_input)
{
    for (TextureDisplacementBakeVolume& volume : bake_input.volumes)
        if (volume.options.pipeline_v2)
            volume.options.v2_max_triangles_k = 0;
    return bake_input;
}

namespace {
bool same_palette(const std::vector<PrintableColor>& a, const std::vector<PrintableColor>& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const PrintableColor& x, const PrintableColor& y) {
               return x.rgb == y.rgb && x.a == y.a && x.b == y.b && x.num == y.num && x.den == y.den;
           });
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

std::size_t preview_triangles(const TextureBakePlan& plan)
{
    std::size_t total = 0;
    for (const TextureBakeVolumeResult& result : plan.results)
        total += result.mesh.its.indices.size();
    return total;
}

std::shared_ptr<const PreviewCache::Entry> PreviewCache::find(const ObjectID& object, const TextureDisplacementBakeInput& input)
{
    for (const std::shared_ptr<Entry>& entry : m_entries)
        if (entry->object == object && same_preview_input(entry->input, input)) {
            entry->used = ++m_clock;
            return entry;
        }
    return nullptr;
}

bool PreviewCache::put(const ObjectID& object, TextureDisplacementBakeInput input, std::shared_ptr<const TextureBakePlan> plan, double seconds)
{
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&](const std::shared_ptr<Entry>& e) { return e->object == object; }),
                    m_entries.end());
    const std::size_t triangles = plan ? preview_triangles(*plan) : 0;
    if (!plan || plan->canceled || triangles > max_triangles)
        return false;
    auto entry       = std::make_shared<Entry>();
    entry->object    = object;
    entry->input     = std::move(input);
    entry->plan      = std::move(plan);
    entry->triangles = triangles;
    entry->seconds   = seconds;
    entry->used      = ++m_clock;
    m_entries.push_back(std::move(entry));
    evict_to_caps();
    return true;
}

void PreviewCache::prune(const std::function<bool(const ObjectID&)>& exists)
{
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&](const std::shared_ptr<Entry>& e) { return !exists(e->object); }),
                    m_entries.end());
}

std::size_t PreviewCache::triangles() const
{
    std::size_t total = 0;
    for (const std::shared_ptr<Entry>& entry : m_entries)
        total += entry->triangles;
    return total;
}

void PreviewCache::evict_to_caps()
{
    while (m_entries.size() > max_objects || (m_entries.size() > 1 && triangles() > max_triangles)) {
        const auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
                                             [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { return a->used < b->used; });
        m_entries.erase(oldest);
    }
}

std::string preview_timed_out_message(int object_id, double wait_s)
{
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.0f", std::max(0., wait_s));
    return "the texture preview of object " + std::to_string(object_id) + " needs more than the call's wait (" + seconds +
           " s), so it is drawn unbaked: raise ORCAMCP_TIMEOUT, or preview one object (fit)";
}

nlohmann::json preview_report_json(const PreviewReportInput& input, std::size_t cache_objects, std::size_t cache_triangles)
{
    using json = nlohmann::json;
    json objects = json::array();
    int  previewed = 0;
    bool complete  = true;
    for (const PreviewObject& object : input.objects) {
        json entry = {{"object_id", object.object_id}, {"parts", json::array()}, {"cached", object.cached}};
        if (!object.plan) {
            complete = false;
            entry["timed_out"] = true;
            entry["note"]      = preview_timed_out_message(object.object_id, input.wait_s);
            objects.push_back(std::move(entry));
            continue;
        }
        ++previewed;
        entry["seconds"] = std::round(object.seconds * 100.) / 100.;
        for (std::size_t i = 0; i < object.captured.volumes.size() && i < object.plan->results.size(); ++i)
            entry["parts"].push_back({{"volume_id", i < object.volume_ids.size() ? object.volume_ids[i] : -1},
                                      {"triangles_base", object.captured.volumes[i].base_mesh.indices.size()},
                                      {"triangles_preview", object.plan->results[i].mesh.its.indices.size()}});
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
    json report = {{"requested", true},
                   {"previewed", previewed},
                   {"complete", complete},
                   {"objects", std::move(objects)},
                   {"skipped", std::move(skipped)},
                   {"no_texture_layers", input.without_layers},
                   {"cache", {{"objects", cache_objects}, {"triangles", cache_triangles},
                              {"max_objects", PreviewCache::max_objects}, {"max_triangles", PreviewCache::max_triangles}}}};
    if (previewed == 0 && complete)
        report["note"] = "nothing was previewed: no object on plate " + std::to_string(input.plate_index) +
                         " has texture layers with paint to show, so the picture is the plain one (skipped says why for each object that has layers)";
    return report;
}

}}} // namespace Slic3r::GUI::OrcaMCP
