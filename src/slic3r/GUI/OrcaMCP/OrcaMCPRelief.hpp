#ifndef slic3r_OrcaMCPRelief_hpp_
#define slic3r_OrcaMCPRelief_hpp_

// render_plate_view's color_by relief and detail, apart from the app. relief colours each facet by the signed distance
// of its centroid from the surface the part had before its texture (the reference): the unbaked mesh with
// texture_preview, or -- kept in memory for a bake made through MCP -- the mesh the bake displaced. detail needs no
// reference: it colours each facet by its largest angle to the facets it shares an edge with, which shows the
// pattern, the aliasing of a coarse texture and the bands where the mesh's triangles change size. Mesh work on copies
// of shared meshes (any thread), the ramps and legends, and the references kept between calls: no GL, no wx, no scene,
// unit-tested in tests/slic3rutils/test_relief.cpp. triangle_size colours each facet by its longest edge in mm (log scale), the
// one view that shows a band of coarse flat triangles, whose facet angles are all 0. The capture, the drawing and the call's flow are
// OrcaMCPPlateUtils.cpp's.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A part with more triangles than this (drawn, or its reference) is not coloured (too_large); one render colours at most
// max_total_triangles in all.
constexpr std::size_t relief_max_part_triangles  = 1000000;
constexpr std::size_t relief_max_total_triangles = 2000000;

// What a picture is coloured by: the depth from a reference surface, the angle between neighbouring facets, or the size of
// each facet (its longest edge).
enum class ReliefMode { Relief, Detail, TriangleSize };

// A fingerprint of a mesh: its counts and FNV-1a over the bytes of its vertices and indices. Equal meshes have equal
// fingerprints; one moved vertex changes it.
std::uint64_t mesh_fingerprint(const indexed_triangle_set& its);

// What the bake displaced, kept so a later render can colour the relief: the volume it produced, the fingerprint of that
// volume's mesh (a reference is used only while the volume still has that mesh), the mesh before the bake and the
// volume's matrix then.
struct BakeReference
{
    ObjectID                                    volume;
    std::uint64_t                               fingerprint = 0;
    std::shared_ptr<const indexed_triangle_set> mesh;
    Transform3d                                 volume_matrix = Transform3d::Identity();
    std::size_t                                 bytes = 0;
    std::uint64_t                               used  = 0;
};

// The references kept between calls, by volume: at most 8 parts and 256 MB in all, the least recently
// used dropped first. A reference alone over the byte cap is not kept. Thread safe: lookups run on the HTTP thread.
class BakeReferences
{
public:
    static constexpr std::size_t default_max_parts = 8;
    static constexpr std::size_t default_max_bytes = 256ull * 1024 * 1024;

    explicit BakeReferences(std::size_t max_parts = default_max_parts, std::size_t max_bytes = default_max_bytes)
        : m_max_parts(max_parts), m_max_bytes(max_bytes) {}

    // Replaces the volume's older reference. False when the mesh alone is over the byte cap (nothing is kept).
    bool remember(const ObjectID& volume, std::uint64_t fingerprint, indexed_triangle_set mesh, const Transform3d& volume_matrix);
    // The reference of `volume` if it was made for a mesh with this fingerprint; counts as a use.
    std::shared_ptr<const BakeReference> find(const ObjectID& volume, std::uint64_t fingerprint);
    void clear();
    std::size_t size() const;
    std::size_t bytes() const;

private:
    void evict_to_caps();   // the lock held

    std::size_t                                 m_max_parts, m_max_bytes;
    mutable std::mutex                          m_mutex;
    std::vector<std::shared_ptr<BakeReference>> m_entries;
    std::uint64_t                               m_clock = 0;
};

BakeReferences& bake_references();
// Dropped with the previews when the project is reset (drop_texture_previews).
void drop_bake_references();

// Signed distance in mm of each triangle's centroid in `drawn` from the nearest point of `reference`, the sign that of
// the nearest reference facet's outward normal (positive: raised above the surface). Both are brought to one frame by
// their matrices. Not along the displacement: on steep relief it understates the depth. `complete` is false when
// `stop` said so (the distances are then partly unset: NaN).
struct ReliefDistances
{
    std::vector<float> mm;
    bool               complete = true;
};
ReliefDistances relief_distances(const indexed_triangle_set& drawn, const Transform3d& drawn_to_frame, const indexed_triangle_set& reference,
                                 const Transform3d& reference_to_frame, const std::function<bool()>& stop);

// One horizontal slab of a mesh and the triangles whose centroids lie in it: the median of their longest edges, so a band of
// coarser or finer triangles shows in numbers.
struct SizeSlab
{
    double      z_from = 0., z_to = 0.;
    std::size_t triangles = 0;
    double      median_edge_mm = 0.;
};
// The slabs of height `slab_mm` from the lowest centroid up, at most `max_slabs` (the slab grows to fit more); empty slabs are
// left out. `z` and `longest_edge` are per triangle; a NaN in either leaves the triangle out.
constexpr double      size_slab_mm   = 2.;
constexpr std::size_t size_slab_max  = 64;
std::vector<SizeSlab> size_slabs(const std::vector<float>& z, const std::vector<float>& longest_edge, double slab_mm = size_slab_mm,
                                 std::size_t max_slabs = size_slab_max);

// The facet angle detail colours by, per triangle: the largest angle in degrees between its normal and that of a facet
// it shares an edge with (an open edge counts for nothing and is counted; a degenerate facet, with no normal, is skipped
// as a neighbour), measured in `to_mm`'s frame. Plus the mesh's edge lengths in that frame, each facet's longest edge
// (`size_mm`, what triangle_size colours by) and the median of those per Z slab (z in the frame). NaN entries when `stop` said so.
struct DetailMetrics
{
    std::vector<float>    angle_deg;
    std::vector<float>    size_mm;
    std::vector<SizeSlab> slabs;
    std::size_t           open_edges = 0;
    double                edge_min_mm = 0., edge_median_mm = 0., edge_max_mm = 0.;
    bool                  complete = true;
};
DetailMetrics detail_metrics(const indexed_triangle_set& its, const Transform3d& to_mm, const std::function<bool()>& stop);

// A value scale: `colors.size()` equal bins from `from` to `to`; a value outside is the end bin. `diverging` ramps have
// their middle bin on zero (white).
struct Ramp
{
    double                 from = 0., to = 0.;
    bool                   diverging = false;
    bool                   log = false;       // the bins are equal ratios from `from` to `to` (both above 0), not equal steps
    std::vector<ColorRGBA> colors;

    std::size_t bins() const { return colors.size(); }
    double      step() const { return colors.empty() ? 0. : (to - from) / double(colors.size()); }
    double      ratio() const { return colors.empty() || !(from > 0.) ? 1. : std::pow(to / from, 1. / double(colors.size())); }
    double      edge(std::size_t i) const { return log ? from * std::pow(ratio(), double(i)) : from + step() * double(i); }   // the lower end of bin i; edge(bins()) is `to`
};

// relief: 15 bins from -half_range to +half_range (raised warm, sunk cool, zero white). `half_range` is the request's
// relief_range_mm, else the larger of |lo| and |hi| given (compute_relief gives the 99th percentile of |distance|, so one
// spike does not wash the rest out; at least 0.001 mm so a flat result still has a scale).
Ramp relief_ramp(double min_mm, double max_mm, std::optional<double> half_range);
// detail: 12 bins from 0 to `max_deg` (pale to dark red). `max_deg` is the request's detail_range_deg, else the
// measured 98th percentile rounded up to a whole degree (at least 1).
Ramp detail_ramp(double measured_p98_deg, std::optional<double> max_deg);

// The 1-based bin of each value (the value NaN: 0, drawn in the part's own colour).
// triangle_size: 10 bins on a log scale from `fine_mm` to `coarse_mm` (the measured 2nd and 98th percentiles of the longest
// edge, widened a little when they are about equal): fine cool, coarse warm.
Ramp triangle_size_ramp(double fine_mm, double coarse_mm);

std::vector<std::uint8_t> ramp_bins(const std::vector<float>& values, const Ramp& ramp);

// The triangles of a mesh sorted into runs of one colour: `mesh` has the same vertices and the index buffer permuted;
// `range` is in index elements (three per triangle), as GLModel::render takes it. Bin 0 triangles are in a run of
// `own` colour (base true).
struct ColourRun
{
    std::pair<std::size_t, std::size_t> range;
    ColorRGBA                           color;
    bool                                base = false;
};
struct ColourRuns
{
    indexed_triangle_set   mesh;
    std::vector<ColourRun> runs;
};
ColourRuns colour_runs(const indexed_triangle_set& its, const std::vector<std::uint8_t>& bins, const Ramp& ramp);

// Order statistics of a set of values (NaN left out): the value `q` (0..1) of the sorted rest; 0 for none.
double quantile(std::vector<float> values, double q);

// One part's numbers for the report.
struct ReliefPartReport
{
    int         object_id = -1;
    int         volume_id = -1;
    std::string reference;             // relief: "unbaked_surface" | "pre_bake_mesh"; "" when not coloured
    std::string reason;                // why it was not coloured ("" when it was)
    std::size_t triangles = 0;
    double      min = 0., max = 0., mean_abs = 0.;   // relief: mm; detail: degrees (mean, not mean_abs: see mean)
    std::size_t clamped_low = 0, clamped_high = 0;   // facets beyond the scale
    // detail only
    double      mean = 0., p50 = 0., p90 = 0.;
    std::size_t open_edges = 0;
    double      edge_min_mm = 0., edge_median_mm = 0., edge_max_mm = 0.;
    std::vector<SizeSlab> slabs;       // detail and triangle_size: the median longest edge per Z slab
    bool        coloured = false;
};

struct ReliefReport
{
    ReliefMode                    mode = ReliefMode::Relief;
    Ramp                          ramp;
    std::vector<ReliefPartReport> parts;
    bool                          complete = true;   // false: the call's wait ran out before every part was coloured
    std::optional<double>         range_asked;
};

// The answer's `relief` / `detail`: {mode, unit, scale: {from, to, bins, step, requested_range?}, legend: [{from, to, color}],
// objects: [{object_id, parts: [...]}], not_coloured: [{object_id, volume_id, reason}], complete, notes}.
nlohmann::json relief_report_json(const ReliefReport& report);

// One model part of the picture, as the main thread captured it, for compute_relief.
struct ReliefPartInput
{
    int                                 object_id = -1;
    int                                 volume_id = -1;                 // the part's position among the object's volumes
    ObjectID                            object_identity;
    ObjectID                            volume_identity;
    std::shared_ptr<const TriangleMesh> mesh;                           // the volume's mesh now (volume frame)
    Transform3d                         volume_matrix = Transform3d::Identity();   // volume -> object frame, now
    Transform3d                         to_world      = Transform3d::Identity();   // volume -> bed mm, the first instance on the plate
    bool                                has_unbaked_layers = false;     // its object holds painted texture layers
    // texture_preview: the displaced mesh drawn instead of `mesh` (volume frame). Null when the part is not previewed.
    const indexed_triangle_set*         previewed = nullptr;
    // Set when the part's object is previewed but this part has no usable preview ("..." says why): not coloured.
    std::string                         unusable;
    // Set when the part's object is previewed but the texture layers do not reach this part: it has no relief, and
    // texture_preview is not what it lacks.
    bool                                object_previewed = false;
};

// The triangles of one coloured part, ready to upload: sorted into runs of one colour, in the volume's frame.
struct ReliefPartDraw
{
    int        object_id = -1;
    int        volume_id = -1;
    ColourRuns runs;
};

struct ReliefOutput
{
    ReliefReport                report;
    std::vector<ReliefPartDraw> draws;
};

// Colours every part that can be coloured (any thread; `stop` is looked at between chunks): relief against its reference
// (previewed: the unbaked mesh; else the pre-bake mesh `references` holds for the volume's present mesh; else none),
// detail and triangle_size on the drawn mesh itself. Parts that cannot be coloured are in report.parts with their reason; a part not
// reached before `stop` said so is `timed_out` and the report incomplete. The scale is the request's range, else the
// measured over all the parts (relief: the 99th percentile of |distance|).
ReliefOutput compute_relief(const std::vector<ReliefPartInput>& parts, ReliefMode mode, std::optional<double> range, BakeReferences& references,
                            const std::function<bool()>& stop);

// The ramp's legend: per bin its range and #rrggbb.
nlohmann::json ramp_legend_json(const Ramp& ramp);

// What to tell the reader beyond the numbers (a scale that clamps, nothing coloured, a flat result).
std::vector<std::string> relief_notes(const ReliefReport& report);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
