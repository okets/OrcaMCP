#include "OrcaMCPRelief.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <iomanip>
#include <map>
#include <sstream>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>

#include "libslic3r/AABBTreeIndirect.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderPaint.hpp"   // hex_color

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

constexpr float k_nan = std::numeric_limits<float>::quiet_NaN();
constexpr std::size_t k_chunk = 32768;   // triangles between two looks at `stop`

std::uint64_t fnv_words(std::uint64_t hash, const void* data, std::size_t bytes)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (; bytes >= 4; bytes -= 4, p += 4) {
        std::uint32_t word;
        std::memcpy(&word, p, 4);
        hash = (hash ^ word) * 1099511628211ull;
    }
    for (; bytes > 0; --bytes, ++p)
        hash = (hash ^ *p) * 1099511628211ull;
    return hash;
}

std::size_t mesh_bytes(const indexed_triangle_set& its)
{
    return its.vertices.size() * sizeof(Vec3f) + its.indices.size() * sizeof(stl_triangle_vertex_indices);
}

ColorRGBA lerp_color(const ColorRGBA& a, const ColorRGBA& b, double t)
{
    return ColorRGBA(float(a.r() + (b.r() - a.r()) * t), float(a.g() + (b.g() - a.g()) * t), float(a.b() + (b.b() - a.b()) * t), 1.f);
}

} // namespace

std::uint64_t mesh_fingerprint(const indexed_triangle_set& its)
{
    std::uint64_t hash = 14695981039346656037ull;
    const std::uint64_t counts[2] = {its.vertices.size(), its.indices.size()};
    hash = fnv_words(hash, counts, sizeof(counts));
    hash = fnv_words(hash, its.vertices.data(), its.vertices.size() * sizeof(Vec3f));
    hash = fnv_words(hash, its.indices.data(), its.indices.size() * sizeof(stl_triangle_vertex_indices));
    return hash;
}

// ---- The references kept between calls ----------------------------------------------------------------------------

bool BakeReferences::remember(const ObjectID& volume, std::uint64_t fingerprint, indexed_triangle_set mesh, const Transform3d& volume_matrix)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&volume](const std::shared_ptr<BakeReference>& e) { return e->volume == volume; }),
                    m_entries.end());
    const std::size_t bytes = mesh_bytes(mesh);
    if (bytes > m_max_bytes)
        return false;
    auto entry           = std::make_shared<BakeReference>();
    entry->volume        = volume;
    entry->fingerprint   = fingerprint;
    entry->mesh          = std::make_shared<const indexed_triangle_set>(std::move(mesh));
    entry->volume_matrix = volume_matrix;
    entry->bytes         = bytes;
    entry->used          = ++m_clock;
    m_entries.push_back(std::move(entry));
    evict_to_caps();
    return true;
}

std::shared_ptr<const BakeReference> BakeReferences::find(const ObjectID& volume, std::uint64_t fingerprint)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const std::shared_ptr<BakeReference>& entry : m_entries)
        if (entry->volume == volume && entry->fingerprint == fingerprint) {
            entry->used = ++m_clock;
            return entry;
        }
    return nullptr;
}

void BakeReferences::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

std::size_t BakeReferences::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

std::size_t BakeReferences::bytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t total = 0;
    for (const std::shared_ptr<BakeReference>& entry : m_entries)
        total += entry->bytes;
    return total;
}

void BakeReferences::evict_to_caps()
{
    const auto total = [this] {
        std::size_t sum = 0;
        for (const std::shared_ptr<BakeReference>& e : m_entries)
            sum += e->bytes;
        return sum;
    };
    while (!m_entries.empty() && (m_entries.size() > m_max_parts || total() > m_max_bytes)) {
        const auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
                                             [](const std::shared_ptr<BakeReference>& a, const std::shared_ptr<BakeReference>& b) { return a->used < b->used; });
        m_entries.erase(oldest);
    }
}

BakeReferences& bake_references()
{
    static BakeReferences references;
    return references;
}

void drop_bake_references() { bake_references().clear(); }

// ---- Distances and facet angles -----------------------------------------------------------------------------------

ReliefDistances relief_distances(const indexed_triangle_set& drawn, const Transform3d& drawn_to_frame, const indexed_triangle_set& reference,
                                 const Transform3d& reference_to_frame, const std::function<bool()>& stop)
{
    ReliefDistances out;
    out.mm.assign(drawn.indices.size(), k_nan);
    if (drawn.indices.empty())
        return out;
    if (reference.indices.empty()) {
        out.complete = false;
        return out;
    }

    std::vector<Vec3f> ref_vertices(reference.vertices.size());
    for (std::size_t i = 0; i < ref_vertices.size(); ++i)
        ref_vertices[i] = (reference_to_frame * reference.vertices[i].cast<double>()).cast<float>();
    // A mirroring matrix turns the facets inside out: the outward normal is then the cross product's opposite.
    const double handed = reference_to_frame.linear().determinant() < 0. ? -1. : 1.;
    std::vector<Vec3d> normals(reference.indices.size());
    for (std::size_t f = 0; f < normals.size(); ++f) {
        const Vec3d a = ref_vertices[reference.indices[f](0)].cast<double>(), b = ref_vertices[reference.indices[f](1)].cast<double>(),
                    c = ref_vertices[reference.indices[f](2)].cast<double>();
        Vec3d n = (b - a).cross(c - a) * handed;
        const double length = n.norm();
        normals[f] = length > 0. ? Vec3d(n / length) : Vec3d(Vec3d::Zero());
    }
    const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(ref_vertices, reference.indices);

    for (std::size_t begin = 0; begin < drawn.indices.size(); begin += k_chunk) {
        if (stop()) {
            out.complete = false;
            return out;
        }
        const std::size_t end = std::min(drawn.indices.size(), begin + k_chunk);
        tbb::parallel_for(tbb::blocked_range<std::size_t>(begin, end, 512), [&](const tbb::blocked_range<std::size_t>& range) {
            for (std::size_t f = range.begin(); f < range.end(); ++f) {
                const stl_triangle_vertex_indices& t = drawn.indices[f];
                const Vec3d centroid = (drawn_to_frame * drawn.vertices[t(0)].cast<double>() + drawn_to_frame * drawn.vertices[t(1)].cast<double>() +
                                        drawn_to_frame * drawn.vertices[t(2)].cast<double>()) / 3.;
                std::size_t hit_face = 0;
                Vec3d       hit_point = Vec3d::Zero();
                const double squared = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(ref_vertices, reference.indices, tree, centroid, hit_face, hit_point);
                if (squared < 0.)
                    continue;
                const double distance = std::sqrt(squared);
                out.mm[f] = float((centroid - hit_point).dot(normals[hit_face]) < 0. ? -distance : distance);
            }
        });
    }
    return out;
}

DetailMetrics detail_metrics(const indexed_triangle_set& source, const Transform3d& to_mm, const std::function<bool()>& stop)
{
    DetailMetrics out;
    out.angle_deg.assign(source.indices.size(), k_nan);
    if (source.indices.empty())
        return out;

    // Meshes whose triangles each carry their own vertices share no edge until equal vertices are merged.
    indexed_triangle_set its = source;
    {
        indexed_triangle_set merged = source;
        its_merge_vertices(merged, false);
        if (merged.indices.size() == source.indices.size())
            its = std::move(merged);
    }
    for (Vec3f& v : its.vertices)
        v = (to_mm * v.cast<double>()).cast<float>();

    const std::vector<Vec3i32> neighbors = its_face_neighbors_par(its);
    std::vector<Vec3f>         normals(its.indices.size());
    std::vector<float>         edges;
    edges.reserve(its.indices.size() * 3);
    for (std::size_t f = 0; f < its.indices.size(); ++f) {
        if (f % k_chunk == 0 && stop()) {
            out.complete = false;
            return out;
        }
        const Vec3f& a = its.vertices[its.indices[f](0)];
        const Vec3f& b = its.vertices[its.indices[f](1)];
        const Vec3f& c = its.vertices[its.indices[f](2)];
        Vec3f n = (b - a).cross(c - a);
        const float length = n.norm();
        normals[f] = length > 0.f ? Vec3f(n / length) : Vec3f(Vec3f::Zero());
        edges.push_back((b - a).norm());
        edges.push_back((c - b).norm());
        edges.push_back((a - c).norm());
    }
    for (std::size_t f = 0; f < its.indices.size(); ++f) {
        if (f % k_chunk == 0 && stop()) {
            out.complete = false;
            return out;
        }
        float widest = 0.f;
        for (int k = 0; k < 3; ++k) {
            const int other = neighbors[f](k);
            if (other < 0) {
                ++out.open_edges;
                continue;
            }
            const float cosine = std::clamp(normals[f].dot(normals[std::size_t(other)]), -1.f, 1.f);
            widest = std::max(widest, std::acos(cosine) * float(180. / PI));
        }
        out.angle_deg[f] = widest;
    }
    const auto middle = edges.begin() + std::ptrdiff_t(edges.size() / 2);
    std::nth_element(edges.begin(), middle, edges.end());
    out.edge_median_mm = *middle;
    const auto [lo, hi] = std::minmax_element(edges.begin(), edges.end());
    out.edge_min_mm = *lo;
    out.edge_max_mm = *hi;
    return out;
}

// ---- Ramps, bins, runs --------------------------------------------------------------------------------------------

Ramp relief_ramp(double min_mm, double max_mm, std::optional<double> half_range)
{
    Ramp ramp;
    ramp.diverging = true;
    const double half = half_range && *half_range > 0. ? *half_range : std::max(0.001, std::max(std::abs(min_mm), std::abs(max_mm)));
    ramp.from = -half;
    ramp.to   = half;
    const ColorRGBA white(1.f, 1.f, 1.f, 1.f), warm(0.84f, 0.12f, 0.06f, 1.f), cool(0.10f, 0.25f, 0.82f, 1.f);
    constexpr int bins = 15;
    for (int i = 0; i < bins; ++i) {
        const double t = double(i - bins / 2) / double(bins / 2);   // -1 .. 1, the middle bin 0
        ramp.colors.push_back(t >= 0. ? lerp_color(white, warm, t) : lerp_color(white, cool, -t));
    }
    return ramp;
}

Ramp detail_ramp(double measured_p98_deg, std::optional<double> max_deg)
{
    Ramp ramp;
    ramp.from = 0.;
    ramp.to   = max_deg && *max_deg > 0. ? *max_deg : std::max(1., std::ceil(measured_p98_deg));
    const ColorRGBA anchors[] = {ColorRGBA(0.96f, 0.96f, 0.90f, 1.f), ColorRGBA(0.99f, 0.85f, 0.40f, 1.f), ColorRGBA(0.93f, 0.45f, 0.15f, 1.f),
                                 ColorRGBA(0.55f, 0.10f, 0.20f, 1.f), ColorRGBA(0.20f, 0.05f, 0.30f, 1.f)};
    constexpr int bins = 12, segments = 4;
    for (int i = 0; i < bins; ++i) {
        const double position = double(i) / double(bins - 1) * segments;
        const int    segment  = std::min(segments - 1, int(position));
        ramp.colors.push_back(lerp_color(anchors[segment], anchors[segment + 1], position - segment));
    }
    return ramp;
}

std::vector<std::uint8_t> ramp_bins(const std::vector<float>& values, const Ramp& ramp)
{
    std::vector<std::uint8_t> bins(values.size(), 0);
    const double n = double(ramp.bins());
    if (ramp.bins() == 0 || !(ramp.to > ramp.from))
        return bins;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (std::isnan(values[i]))
            continue;
        const int index = std::clamp(int(std::floor((double(values[i]) - ramp.from) / (ramp.to - ramp.from) * n)), 0, int(ramp.bins()) - 1);
        bins[i] = std::uint8_t(index + 1);
    }
    return bins;
}

ColourRuns colour_runs(const indexed_triangle_set& its, const std::vector<std::uint8_t>& bins, const Ramp& ramp)
{
    ColourRuns out;
    out.mesh.vertices = its.vertices;
    const std::size_t slots = ramp.bins() + 1;
    std::vector<std::size_t> start(slots + 1, 0);
    for (std::size_t f = 0; f < its.indices.size(); ++f)
        ++start[std::min<std::size_t>(bins[f], slots - 1) + 1];
    for (std::size_t s = 1; s <= slots; ++s)
        start[s] += start[s - 1];
    out.mesh.indices.resize(its.indices.size());
    std::vector<std::size_t> next(start.begin(), start.end() - 1);
    for (std::size_t f = 0; f < its.indices.size(); ++f)
        out.mesh.indices[next[std::min<std::size_t>(bins[f], slots - 1)]++] = its.indices[f];
    for (std::size_t s = 0; s < slots; ++s)
        if (start[s + 1] > start[s])
            out.runs.push_back({{start[s] * 3, start[s + 1] * 3}, s == 0 ? ColorRGBA(0.f, 0.f, 0.f, 1.f) : ramp.colors[s - 1], s == 0});
    return out;
}

double quantile(std::vector<float> values, double q)
{
    values.erase(std::remove_if(values.begin(), values.end(), [](float v) { return std::isnan(v); }), values.end());
    if (values.empty())
        return 0.;
    const std::size_t k = std::min(values.size() - 1, std::size_t(std::floor(std::clamp(q, 0., 1.) * double(values.size() - 1) + 0.5)));
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(k), values.end());
    return values[k];
}

// ---- Colouring a scene's parts ------------------------------------------------------------------------------------

namespace {

// One part measured, before the scale is known.
struct Measured
{
    std::size_t         part = 0;       // index into the inputs
    std::string         reference;
    std::vector<float>  values;         // relief: mm per triangle; detail: degrees
    const indexed_triangle_set* drawn = nullptr;
    DetailMetrics       detail;
};

void fill_stats(ReliefPartReport& report, const std::vector<float>& values, const Ramp& ramp, bool detail)
{
    double sum = 0., sum_abs = 0., lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    std::size_t n = 0;
    for (const float v : values) {
        if (std::isnan(v))
            continue;
        ++n;
        sum += v;
        sum_abs += std::abs(double(v));
        lo = std::min(lo, double(v));
        hi = std::max(hi, double(v));
        if (v < ramp.from) ++report.clamped_low;
        if (v > ramp.to) ++report.clamped_high;
    }
    report.triangles = values.size();
    if (n == 0)
        return;
    report.min      = detail ? 0. : lo;
    report.max      = hi;
    report.mean_abs = sum_abs / double(n);
    report.mean     = sum / double(n);
    report.p50      = quantile(values, 0.5);
    report.p90      = quantile(values, 0.9);
}

} // namespace

ReliefOutput compute_relief(const std::vector<ReliefPartInput>& parts, bool detail, std::optional<double> range, BakeReferences& references,
                            const std::function<bool()>& stop)
{
    ReliefOutput out;
    out.report.detail      = detail;
    out.report.range_asked = range;
    out.report.parts.resize(parts.size());
    std::vector<Measured> measured;
    std::size_t           budget = relief_max_total_triangles;
    bool                  stopped = false;

    for (std::size_t i = 0; i < parts.size(); ++i) {
        const ReliefPartInput& in     = parts[i];
        ReliefPartReport&      report = out.report.parts[i];
        report.object_id = in.object_id;
        report.volume_id = in.volume_id;
        const auto leave = [&report](const std::string& reason) { report.reason = reason; };
        if (!in.unusable.empty()) { leave(in.unusable); continue; }
        if (stopped || stop()) {
            stopped = true;
            out.report.complete = false;
            leave("timed_out: the call's wait ran out before this part was reached");
            continue;
        }
        if (!in.mesh) { leave("the part has no mesh"); continue; }
        const indexed_triangle_set& drawn = in.previewed != nullptr ? *in.previewed : in.mesh->its;
        if (drawn.indices.empty()) { leave("the part has no triangles"); continue; }
        if (drawn.indices.size() > relief_max_part_triangles) {
            leave("too_large: " + std::to_string(drawn.indices.size()) + " triangles, over the " + std::to_string(relief_max_part_triangles) + " a colouring holds");
            continue;
        }
        if (drawn.indices.size() > budget) {
            leave("over_render_total: one picture colours at most " + std::to_string(relief_max_total_triangles) + " triangles; fit one object");
            continue;
        }

        Measured m;
        m.part  = i;
        m.drawn = &drawn;
        if (detail) {
            m.detail = detail_metrics(drawn, in.to_world, stop);
            if (!m.detail.complete) {
                stopped = true;
                out.report.complete = false;
                leave("timed_out: the call's wait ran out while this part was being measured");
                continue;
            }
            m.values = m.detail.angle_deg;
        } else {
            std::shared_ptr<const BakeReference> kept;
            const indexed_triangle_set*          reference = nullptr;
            Transform3d drawn_to_frame = Transform3d::Identity(), reference_to_frame = Transform3d::Identity();
            if (in.previewed != nullptr) {
                reference   = &in.mesh->its;
                m.reference = "unbaked_surface";
            } else if ((kept = references.find(in.volume_identity, mesh_fingerprint(in.mesh->its)))) {
                reference          = kept->mesh.get();
                m.reference        = "pre_bake_mesh";
                drawn_to_frame     = in.volume_matrix;
                reference_to_frame = kept->volume_matrix;
            }
            if (reference == nullptr) {
                leave(in.has_unbaked_layers ? "unbaked: add texture_preview, which draws the texture the layers would make and measures it from the surface before it"
                                            : "no reference: the surface before the bake is not known (the bake was not made through bake_texture_displacement in this session, "
                                              "or the mesh changed since); color_by detail works without one");
                continue;
            }
            if (reference->indices.size() > relief_max_part_triangles) {
                leave("too_large: its reference has " + std::to_string(reference->indices.size()) + " triangles, over the " + std::to_string(relief_max_part_triangles) + " a colouring holds");
                continue;
            }
            ReliefDistances distances = relief_distances(drawn, drawn_to_frame, *reference, reference_to_frame, stop);
            if (!distances.complete) {
                stopped = true;
                out.report.complete = false;
                leave("timed_out: the call's wait ran out while this part was being measured");
                continue;
            }
            m.values = std::move(distances.mm);
        }
        budget -= drawn.indices.size();
        measured.push_back(std::move(m));
    }

    // One scale for every part, so colours mean the same everywhere in the picture.
    double lo = 0., hi = 0.;
    std::vector<float> pooled;
    for (const Measured& m : measured) {
        for (const float v : m.values)
            if (!std::isnan(v)) {
                lo = std::min(lo, double(v));
                hi = std::max(hi, double(v));
            }
        if (detail)
            pooled.insert(pooled.end(), m.values.begin(), m.values.end());
    }
    out.report.ramp = detail ? detail_ramp(quantile(std::move(pooled), 0.98), range) : relief_ramp(lo, hi, range);

    for (const Measured& m : measured) {
        ReliefPartReport& report = out.report.parts[m.part];
        report.reference = m.reference;
        report.coloured  = true;
        fill_stats(report, m.values, out.report.ramp, detail);
        if (detail) {
            report.open_edges     = m.detail.open_edges;
            report.edge_min_mm    = m.detail.edge_min_mm;
            report.edge_median_mm = m.detail.edge_median_mm;
            report.edge_max_mm    = m.detail.edge_max_mm;
        }
        ReliefPartDraw draw;
        draw.object_id = report.object_id;
        draw.volume_id = report.volume_id;
        draw.runs      = colour_runs(*m.drawn, ramp_bins(m.values, out.report.ramp), out.report.ramp);
        out.draws.push_back(std::move(draw));
    }
    return out;
}

// ---- The answer ---------------------------------------------------------------------------------------------------

namespace {

double rounded(double value) { return std::round(value * 1e4) / 1e4; }

std::string number_text(double value)
{
    std::ostringstream out;
    out << std::setprecision(4) << value;
    return out.str();
}

nlohmann::json part_json(const ReliefPartReport& p, bool detail)
{
    nlohmann::json json = {{"volume_id", p.volume_id}, {"triangles", p.triangles}};
    if (detail) {
        json["max_deg"]  = rounded(p.max);
        json["mean_deg"] = rounded(p.mean);
        json["p50_deg"]  = rounded(p.p50);
        json["p90_deg"]  = rounded(p.p90);
        json["beyond_scale"] = p.clamped_high;
        json["open_edges"]   = p.open_edges;
        json["edge_mm"]      = {{"min", rounded(p.edge_min_mm)}, {"median", rounded(p.edge_median_mm)}, {"max", rounded(p.edge_max_mm)}};
    } else {
        json["reference"]     = p.reference;
        json["min_mm"]        = rounded(p.min);
        json["max_mm"]        = rounded(p.max);
        json["mean_abs_mm"]   = rounded(p.mean_abs);
        json["beyond_scale"]  = {{"sunk", p.clamped_low}, {"raised", p.clamped_high}};
    }
    return json;
}

} // namespace

nlohmann::json ramp_legend_json(const Ramp& ramp)
{
    nlohmann::json legend = nlohmann::json::array();
    for (std::size_t i = 0; i < ramp.bins(); ++i)
        legend.push_back({{"from", rounded(ramp.from + ramp.step() * double(i))}, {"to", rounded(ramp.from + ramp.step() * double(i + 1))},
                          {"color", hex_color(ramp.colors[i])}});
    return legend;
}

std::vector<std::string> relief_notes(const ReliefReport& report)
{
    std::vector<std::string> notes;
    const bool any = std::any_of(report.parts.begin(), report.parts.end(), [](const ReliefPartReport& p) { return p.coloured; });
    if (!any) {
        notes.push_back(report.detail ? "no part was coloured: see not_coloured for why"
                                      : "no part has a surface to measure the relief from, so nothing is coloured by relief: texture_preview gives an unbaked texture its reference "
                                        "(the surface before it), and a bake made through bake_texture_displacement keeps its pre-bake mesh for this session; a mesh baked in the "
                                        "texture panel, an earlier session's or a loaded one has none -- color_by detail shows its pattern and mesh density without a reference");
        return notes;
    }
    std::size_t clamped = 0;
    for (const ReliefPartReport& p : report.parts)
        clamped += p.clamped_low + p.clamped_high;
    if (clamped > 0 && report.range_asked)
        notes.push_back(std::to_string(clamped) + " facets lie beyond the requested scale and are drawn in its end colours; leave the range out to scale to the measured extremes");
    if (!report.detail) {
        const bool flat = std::all_of(report.parts.begin(), report.parts.end(), [](const ReliefPartReport& p) { return !p.coloured || (std::abs(p.min) < 1e-4 && std::abs(p.max) < 1e-4); });
        if (flat)
            notes.push_back("no relief was measured (every facet is within 0.0001 mm of the reference): the texture moved nothing here, or its depth rounds to zero");
        notes.push_back("the distance is to the nearest point of the reference surface, not along the displacement, so steep relief reads shallower than its depth");
    } else {
        notes.push_back("each facet is coloured by its largest angle to the facets it shares an edge with: a flat or smoothly curved region is pale, a sharp feature or a coarse "
                        "stair-step dark, and a band whose triangles are larger or smaller than its neighbours' shows as a change of shade; edge_mm gives the triangle sizes");
    }
    notes.push_back("the colours are shaded by the picture's lights: read the legend by hue and compare shades only between regions lit alike");
    if (!report.complete)
        notes.push_back("the call's wait ran out before every part was coloured; the parts in not_coloured with timed_out are drawn in their own colour (raise ORCAMCP_TIMEOUT or fit one object)");
    return notes;
}

nlohmann::json relief_report_json(const ReliefReport& report)
{
    nlohmann::json json;
    json["mode"]     = report.detail ? "detail" : "relief";
    json["unit"]     = report.detail ? "deg" : "mm";
    json["complete"] = report.complete;
    json["scale"]    = {{"from", rounded(report.ramp.from)}, {"to", rounded(report.ramp.to)}, {"bins", report.ramp.bins()},
                        {"step", rounded(report.ramp.step())}, {"range_source", report.range_asked ? "requested" : "measured"}};
    json["how_to_read"] = report.detail
                              ? "pale = flat or smooth, dark = a sharp facet angle; each colour step is " + number_text(report.ramp.step()) + " degrees, from 0 to " +
                                    number_text(report.ramp.to) + " (beyond that, the darkest)"
                              : "red = raised above the surface before the texture, blue = sunk, white = within one step of it; each colour step is " +
                                    number_text(report.ramp.step()) + " mm, from " + number_text(report.ramp.from) + " to " +
                                    number_text(report.ramp.to) + " (beyond that, the end colours)";
    json["legend"] = ramp_legend_json(report.ramp);
    std::map<int, nlohmann::json> by_object;
    nlohmann::json                skipped = nlohmann::json::array();
    for (const ReliefPartReport& p : report.parts) {
        if (!p.coloured) {
            skipped.push_back({{"object_id", p.object_id}, {"volume_id", p.volume_id}, {"reason", p.reason}});
            continue;
        }
        nlohmann::json& object = by_object[p.object_id];
        if (object.is_null())
            object = {{"object_id", p.object_id}, {"parts", nlohmann::json::array()}};
        object["parts"].push_back(part_json(p, report.detail));
    }
    nlohmann::json objects = nlohmann::json::array();
    for (auto& item : by_object)
        objects.push_back(std::move(item.second));
    json["objects"]      = std::move(objects);
    json["not_coloured"] = std::move(skipped);
    const std::vector<std::string> notes = relief_notes(report);
    if (!notes.empty())
        json["notes"] = notes;
    return json;
}

}}} // namespace Slic3r::GUI::OrcaMCP
