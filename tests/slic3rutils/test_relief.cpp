#include <catch2/catch_all.hpp>

#include <cmath>
#include <limits>
#include <string>

#include "slic3r/GUI/OrcaMCP/OrcaMCPRelief.hpp"

// color_by relief and detail apart from the app (OrcaMCPRelief.cpp): the distances from a reference surface, the facet
// angles, the ramps, the colour runs, the references kept after a bake, and compute_relief over captured parts.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

namespace {

// A flat grid of n x n quads (2 triangles each) at height z over [0, size]^2, facing +Z.
indexed_triangle_set plane(double size, int n, double z)
{
    indexed_triangle_set its;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            its.vertices.emplace_back(float(size * x / n), float(size * y / n), float(z));
    const auto at = [n](int x, int y) { return y * (n + 1) + x; };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            its.indices.emplace_back(at(x, y), at(x + 1, y), at(x + 1, y + 1));
            its.indices.emplace_back(at(x, y), at(x + 1, y + 1), at(x, y + 1));
        }
    return its;
}

// The inner part of a plane, so no centroid is nearer an edge of the reference than its face.
indexed_triangle_set patch(double z) { indexed_triangle_set p = plane(4., 4, z); for (Vec3f& v : p.vertices) v += Vec3f(3.f, 3.f, 0.f); return p; }

const std::function<bool()> never = [] { return false; };

std::shared_ptr<const TriangleMesh> shared_mesh(const indexed_triangle_set& its) { return std::make_shared<const TriangleMesh>(its); }

ReliefPartInput part_of(const indexed_triangle_set& its, int object, int volume, size_t volume_id)
{
    ReliefPartInput part;
    part.object_id       = object;
    part.volume_id       = volume;
    part.volume_identity = ObjectID(volume_id);
    part.mesh            = shared_mesh(its);
    return part;
}

} // namespace

TEST_CASE("A patch raised above a plane is that far above it on every facet", "[Relief][orcamcp]")
{
    const ReliefDistances d = relief_distances(patch(0.4), Transform3d::Identity(), plane(10., 5, 0.), Transform3d::Identity(), never);
    REQUIRE(d.complete);
    REQUIRE(d.mm.size() == 32);
    for (const float v : d.mm)
        CHECK_THAT(v, WithinAbs(0.4, 1e-5));
}

TEST_CASE("A patch sunk below a plane is negative, and a surface against itself is zero", "[Relief][orcamcp]")
{
    for (const float v : relief_distances(patch(-0.3), Transform3d::Identity(), plane(10., 5, 0.), Transform3d::Identity(), never).mm)
        CHECK_THAT(v, WithinAbs(-0.3, 1e-5));
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    const ReliefDistances same = relief_distances(cube, Transform3d::Identity(), cube, Transform3d::Identity(), never);
    REQUIRE(same.mm.size() == 12);
    for (const float v : same.mm)
        CHECK_THAT(v, WithinAbs(0., 1e-5));
}

TEST_CASE("The distance is measured in one frame whatever the matrices of the two meshes", "[Relief][orcamcp]")
{
    // The drawn patch sits at z = -0.6 in a volume lifted by 1 mm; the reference is at 0 in a volume not lifted.
    Transform3d lifted = Transform3d::Identity();
    lifted.translate(Vec3d(0., 0., 1.));
    for (const float v : relief_distances(patch(-0.6), lifted, plane(10., 5, 0.), Transform3d::Identity(), never).mm)
        CHECK_THAT(v, WithinAbs(0.4, 1e-5));

    // A mirrored volume turns its facets inside out; the sign still follows the outward normal.
    Transform3d mirror = Transform3d::Identity();
    mirror.scale(Vec3d(-1., 1., 1.));
    for (const float v : relief_distances(patch(0.4), mirror, plane(10., 5, 0.), mirror, never).mm)
        CHECK_THAT(v, WithinAbs(0.4, 1e-5));
}

TEST_CASE("A distance stopped by the call's wait is incomplete, and an empty reference measures nothing", "[Relief][orcamcp]")
{
    const std::function<bool()> now = [] { return true; };
    CHECK_FALSE(relief_distances(patch(0.4), Transform3d::Identity(), plane(10., 5, 0.), Transform3d::Identity(), now).complete);
    CHECK_FALSE(relief_distances(patch(0.4), Transform3d::Identity(), indexed_triangle_set(), Transform3d::Identity(), never).complete);
}

TEST_CASE("Every facet of a cube meets its neighbours at 90 degrees", "[Relief][orcamcp]")
{
    const DetailMetrics m = detail_metrics(its_make_cube(10., 10., 10.), Transform3d::Identity(), never);
    REQUIRE(m.complete);
    REQUIRE(m.angle_deg.size() == 12);
    for (const float a : m.angle_deg)
        CHECK_THAT(a, WithinAbs(90., 1e-3));
    CHECK(m.open_edges == 0);
    CHECK_THAT(m.edge_min_mm, WithinAbs(10., 1e-4));
    CHECK_THAT(m.edge_median_mm, WithinAbs(10., 1e-4));
    CHECK_THAT(m.edge_max_mm, WithinAbs(std::sqrt(200.), 1e-3));
}

TEST_CASE("A flat grid has no facet angle and open edges only at its border", "[Relief][orcamcp]")
{
    const DetailMetrics m = detail_metrics(plane(10., 4, 0.), Transform3d::Identity(), never);
    for (const float a : m.angle_deg)
        CHECK_THAT(a, WithinAbs(0., 1e-3));
    CHECK(m.open_edges == 16);   // 4 border edges per side
    // The matrix scales the edges, not the angles.
    Transform3d twice = Transform3d::Identity();
    twice.scale(2.);
    const DetailMetrics scaled = detail_metrics(its_make_cube(10., 10., 10.), twice, never);
    CHECK_THAT(scaled.edge_min_mm, WithinAbs(20., 1e-3));
    CHECK_THAT(scaled.angle_deg[0], WithinAbs(90., 1e-3));
}

TEST_CASE("A coarse tessellation of a curved surface has wider facet angles than a fine one", "[Relief][orcamcp]")
{
    const DetailMetrics coarse = detail_metrics(its_make_sphere(10., PI / 6.), Transform3d::Identity(), never);
    const DetailMetrics fine   = detail_metrics(its_make_sphere(10., PI / 24.), Transform3d::Identity(), never);
    REQUIRE(coarse.complete);
    REQUIRE(fine.complete);
    CHECK(quantile(coarse.angle_deg, 0.5) > 2. * quantile(fine.angle_deg, 0.5));
    CHECK(coarse.edge_median_mm > fine.edge_median_mm);
}

TEST_CASE("The relief ramp is symmetric around zero, which is white", "[Relief][orcamcp]")
{
    const Ramp ramp = relief_ramp(-0.2, 0.5, std::nullopt);
    REQUIRE(ramp.bins() == 15);
    CHECK_THAT(ramp.from, WithinAbs(-0.5, 1e-12));   // the larger extreme sets both ends
    CHECK_THAT(ramp.to, WithinAbs(0.5, 1e-12));
    CHECK_THAT(ramp.step(), WithinAbs(1. / 15., 1e-9));
    const ColorRGBA middle = ramp.colors[7];
    CHECK_THAT(middle.r(), WithinAbs(1., 1e-6));
    CHECK_THAT(middle.g(), WithinAbs(1., 1e-6));
    CHECK_THAT(middle.b(), WithinAbs(1., 1e-6));
    CHECK(ramp.colors[14].r() > ramp.colors[14].b());   // raised: warm
    CHECK(ramp.colors[0].b() > ramp.colors[0].r());     // sunk: cool
    CHECK_THAT(ramp.colors[14].r() - ramp.colors[14].b() + (ramp.colors[0].r() - ramp.colors[0].b()), WithinAbs(0.84 - 0.06 + 0.10 - 0.82, 1e-5));
    CHECK_THAT(relief_ramp(-0.2, 0.5, 0.1).to, WithinAbs(0.1, 1e-12));   // the request's range wins
    CHECK_THAT(relief_ramp(0., 0., std::nullopt).to, WithinAbs(0.001, 1e-12));   // a flat result still has a scale
}

TEST_CASE("Values fall into the ramp's bins, the extremes into the end bins and NaN into none", "[Relief][orcamcp]")
{
    const Ramp ramp = relief_ramp(-0.5, 0.5, std::nullopt);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<std::uint8_t> bins = ramp_bins({0.f, -0.5f, 0.5f, 0.49f, -0.49f, 2.f, -2.f, nan}, ramp);
    CHECK(bins == std::vector<std::uint8_t>{8, 1, 15, 15, 1, 15, 1, 0});
    const Ramp detail = detail_ramp(7.2, std::nullopt);
    CHECK(detail.bins() == 12);
    CHECK_THAT(detail.to, WithinAbs(8., 1e-12));
    CHECK_THAT(detail_ramp(0.2, std::nullopt).to, WithinAbs(1., 1e-12));
    CHECK_THAT(detail_ramp(7.2, 30.).to, WithinAbs(30., 1e-12));
    CHECK(ramp_bins({0.f, 7.9f, 9.f}, detail) == std::vector<std::uint8_t>{1, 12, 12});
}

TEST_CASE("Colour runs sort the triangles by bin and keep every one", "[Relief][orcamcp]")
{
    const indexed_triangle_set its = plane(4., 2, 0.);   // 8 triangles
    const Ramp ramp = relief_ramp(-1., 1., std::nullopt);
    const std::vector<std::uint8_t> bins = {15, 1, 15, 0, 8, 8, 1, 15};
    const ColourRuns runs = colour_runs(its, bins, ramp);
    CHECK(runs.mesh.vertices.size() == its.vertices.size());
    REQUIRE(runs.mesh.indices.size() == 8);
    std::size_t covered = 0;
    for (const ColourRun& run : runs.runs) {
        CHECK(run.range.first % 3 == 0);
        covered += (run.range.second - run.range.first) / 3;
    }
    CHECK(covered == 8);
    REQUIRE(runs.runs.size() == 4);
    CHECK(runs.runs[0].base);                                  // bin 0 first: the part's own colour
    CHECK((runs.runs[0].range.second - runs.runs[0].range.first) == 3);
    CHECK_THAT(runs.runs[1].color.b(), WithinAbs(ramp.colors[0].b(), 1e-6));   // bin 1
    CHECK((runs.runs[3].range.second - runs.runs[3].range.first) == 9);        // three triangles in bin 15
    // the triangle in bin 0 is the fourth one
    CHECK(runs.mesh.indices[0] == its.indices[3]);
}

TEST_CASE("A mesh fingerprint changes with one vertex and is the same for a copy", "[Relief][orcamcp]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    indexed_triangle_set copy = cube;
    CHECK(mesh_fingerprint(copy) == mesh_fingerprint(cube));
    copy.vertices[3].z() += 0.001f;
    CHECK(mesh_fingerprint(copy) != mesh_fingerprint(cube));
    copy = cube;
    std::swap(copy.indices[0](0), copy.indices[0](1));
    CHECK(mesh_fingerprint(copy) != mesh_fingerprint(cube));
}

TEST_CASE("A reference is found only for the mesh it was made for", "[Relief][orcamcp]")
{
    BakeReferences references;
    const indexed_triangle_set base = plane(10., 5, 0.);
    REQUIRE(references.remember(ObjectID(7), 111, base, Transform3d::Identity()));
    REQUIRE(references.find(ObjectID(7), 111) != nullptr);
    CHECK(references.find(ObjectID(7), 111)->mesh->indices.size() == base.indices.size());
    CHECK(references.find(ObjectID(7), 112) == nullptr);   // another mesh: undone, repaired or baked again
    CHECK(references.find(ObjectID(8), 111) == nullptr);
    // a new bake of the volume replaces its reference
    references.remember(ObjectID(7), 222, base, Transform3d::Identity());
    CHECK(references.find(ObjectID(7), 111) == nullptr);
    CHECK(references.find(ObjectID(7), 222) != nullptr);
    CHECK(references.size() == 1);
    references.clear();
    CHECK(references.size() == 0);
}

TEST_CASE("The references drop the least recently used past their part and byte caps", "[Relief][orcamcp]")
{
    const indexed_triangle_set base = plane(10., 5, 0.);
    BakeReferences by_count(3, 1ull << 30);
    for (std::size_t id = 1; id <= 3; ++id)
        by_count.remember(ObjectID(id), id, base, Transform3d::Identity());
    REQUIRE(by_count.find(ObjectID(1), 1) != nullptr);   // 1 is used again, so 2 is now the oldest
    by_count.remember(ObjectID(4), 4, base, Transform3d::Identity());
    CHECK(by_count.size() == 3);
    CHECK(by_count.find(ObjectID(2), 2) == nullptr);
    CHECK(by_count.find(ObjectID(1), 1) != nullptr);

    const std::size_t one = base.vertices.size() * sizeof(Vec3f) + base.indices.size() * sizeof(stl_triangle_vertex_indices);
    BakeReferences by_bytes(100, 2 * one + one / 2);
    for (std::size_t id = 1; id <= 3; ++id)
        by_bytes.remember(ObjectID(id), id, base, Transform3d::Identity());
    CHECK(by_bytes.size() == 2);
    CHECK(by_bytes.bytes() <= 2 * one + one / 2);
    CHECK(by_bytes.find(ObjectID(1), 1) == nullptr);
    BakeReferences too_big(100, one - 1);
    CHECK_FALSE(too_big.remember(ObjectID(1), 1, base, Transform3d::Identity()));
    CHECK(too_big.size() == 0);
}

TEST_CASE("A previewed part is coloured by what the texture added to the surface it had", "[Relief][orcamcp]")
{
    BakeReferences references;
    const indexed_triangle_set raised = patch(0.4), flat = patch(0.);
    ReliefPartInput part = part_of(flat, 1, 0, 5);
    part.previewed = &raised;
    const ReliefOutput out = compute_relief({part}, ReliefMode::Relief, std::nullopt, references, never);
    REQUIRE(out.report.parts.size() == 1);
    const ReliefPartReport& r = out.report.parts[0];
    CHECK(r.coloured);
    CHECK(r.reference == "unbaked_surface");
    CHECK_THAT(r.min, WithinAbs(0.4, 1e-4));
    CHECK_THAT(r.max, WithinAbs(0.4, 1e-4));
    CHECK_THAT(r.mean_abs, WithinAbs(0.4, 1e-4));
    CHECK_THAT(out.report.ramp.to, WithinAbs(0.4, 1e-4));   // the measured extreme sets the scale
    CHECK(out.report.complete);
    REQUIRE(out.draws.size() == 1);
    CHECK(out.draws[0].object_id == 1);
    CHECK(out.draws[0].runs.mesh.indices.size() == raised.indices.size());
}

TEST_CASE("A baked part is coloured against the mesh the bake displaced, through both matrices", "[Relief][orcamcp]")
{
    BakeReferences references;
    const indexed_triangle_set before = plane(10., 5, 0.), after = patch(0.4);
    references.remember(ObjectID(9), mesh_fingerprint(after), before, Transform3d::Identity());
    ReliefPartInput part = part_of(after, 2, 1, 9);
    const ReliefOutput out = compute_relief({part}, ReliefMode::Relief, std::nullopt, references, never);
    REQUIRE(out.report.parts[0].coloured);
    CHECK(out.report.parts[0].reference == "pre_bake_mesh");
    CHECK_THAT(out.report.parts[0].max, WithinAbs(0.4, 1e-4));

    // The volume was lifted since the bake: only the part's matrix moved, so the relief reads 1 mm more.
    part.volume_matrix.translate(Vec3d(0., 0., 1.));
    part.to_world = part.volume_matrix;   // the instance is the identity here
    CHECK_THAT(compute_relief({part}, ReliefMode::Relief, std::nullopt, references, never).report.parts[0].min, WithinAbs(1.4, 1e-4));
}

TEST_CASE("A part with no reference says so, and what would give it one", "[Relief][orcamcp]")
{
    BakeReferences references;
    ReliefPartInput plain = part_of(patch(0.4), 1, 0, 5);
    ReliefOutput    out   = compute_relief({plain}, ReliefMode::Relief, std::nullopt, references, never);
    CHECK_FALSE(out.report.parts[0].coloured);
    CHECK(out.report.parts[0].reason.find("no reference") != std::string::npos);
    CHECK(out.report.parts[0].reason.find("color_by detail") != std::string::npos);
    CHECK(out.draws.empty());
    const std::vector<std::string> notes = relief_notes(out.report);
    REQUIRE_FALSE(notes.empty());
    CHECK(notes[0].find("color_by detail") != std::string::npos);

    plain.has_unbaked_layers = true;
    out = compute_relief({plain}, ReliefMode::Relief, std::nullopt, references, never);
    CHECK(out.report.parts[0].reason.find("add texture_preview") != std::string::npos);

    // The object is previewed but the layers do not reach this part: texture_preview is not what it lacks.
    plain.object_previewed = true;
    out = compute_relief({plain}, ReliefMode::Relief, std::nullopt, references, never);
    CHECK(out.report.parts[0].reason.find("not_reached") != std::string::npos);
    CHECK(out.report.parts[0].reason.find("add texture_preview") == std::string::npos);
    plain.object_previewed = false;

    plain.unusable = "the texture layers move nothing on this part";
    out = compute_relief({plain}, ReliefMode::Relief, std::nullopt, references, never);
    CHECK(out.report.parts[0].reason == plain.unusable);

    // a reference made for another mesh is no reference
    references.remember(ObjectID(5), mesh_fingerprint(patch(0.5)), plane(10., 5, 0.), Transform3d::Identity());
    plain.unusable.clear();
    plain.has_unbaked_layers = false;
    CHECK_FALSE(compute_relief({plain}, ReliefMode::Relief, std::nullopt, references, never).report.parts[0].coloured);
}

TEST_CASE("The requested relief range clamps what lies beyond it and is reported", "[Relief][orcamcp]")
{
    BakeReferences references;
    const indexed_triangle_set raised = patch(0.4);
    ReliefPartInput part = part_of(patch(0.), 1, 0, 5);
    part.previewed = &raised;
    const ReliefOutput out = compute_relief({part}, ReliefMode::Relief, 0.1, references, never);
    CHECK_THAT(out.report.ramp.to, WithinAbs(0.1, 1e-12));
    CHECK(out.report.parts[0].clamped_high == 32);
    CHECK(out.report.parts[0].clamped_low == 0);
    const nlohmann::json json = relief_report_json(out.report);
    CHECK(json["scale"]["range_source"] == "requested");
    CHECK(json["notes"].dump().find("beyond the requested scale") != std::string::npos);
}

TEST_CASE("Detail colours any mesh without a reference", "[Relief][orcamcp]")
{
    BakeReferences references;
    ReliefPartInput cube = part_of(its_make_cube(10., 10., 10.), 3, 0, 1);
    ReliefPartInput grid = part_of(plane(10., 4, 0.), 3, 1, 2);
    const ReliefOutput out = compute_relief({cube, grid}, ReliefMode::Detail, std::nullopt, references, never);
    REQUIRE(out.report.parts.size() == 2);
    CHECK(out.report.parts[0].coloured);
    CHECK(out.report.parts[1].coloured);
    CHECK_THAT(out.report.parts[0].max, WithinAbs(90., 1e-3));
    CHECK_THAT(out.report.parts[1].max, WithinAbs(0., 1e-3));
    CHECK(out.report.parts[0].reference.empty());
    CHECK(out.report.ramp.bins() == 12);
    CHECK_THAT(out.report.ramp.to, WithinAbs(90., 1e-9));   // one scale for both parts: the cube's 98th percentile
    CHECK(out.draws.size() == 2);
    const nlohmann::json json = relief_report_json(out.report);
    CHECK(json["mode"] == "detail");
    CHECK(json["unit"] == "deg");
    CHECK(json["objects"][0]["parts"].size() == 2);
    CHECK(json["objects"][0]["parts"][0].contains("edge_mm"));
}

TEST_CASE("A call whose wait runs out leaves the parts it did not reach in their own colour and says so", "[Relief][orcamcp]")
{
    BakeReferences references;
    int looks = 0;
    // compute_relief looks once before a part and detail_metrics twice inside it: the first part passes, the wait is over before the second.
    const std::function<bool()> after_first = [&looks] { return ++looks > 3; };
    ReliefPartInput a = part_of(its_make_cube(10., 10., 10.), 1, 0, 1), b = part_of(its_make_cube(5., 5., 5.), 1, 1, 2);
    const ReliefOutput out = compute_relief({a, b}, ReliefMode::Detail, std::nullopt, references, after_first);
    CHECK_FALSE(out.report.complete);
    CHECK(out.report.parts[0].coloured);
    CHECK_FALSE(out.report.parts[1].coloured);
    CHECK(out.report.parts[1].reason.find("timed_out") != std::string::npos);
    CHECK(relief_report_json(out.report)["complete"] == false);
}

TEST_CASE("The relief answer gives the scale in mm per step and its range", "[Relief][orcamcp]")
{
    BakeReferences references;
    const indexed_triangle_set raised = patch(0.3);
    ReliefPartInput part = part_of(patch(0.), 4, 2, 5);
    part.previewed = &raised;
    const nlohmann::json json = relief_report_json(compute_relief({part}, ReliefMode::Relief, std::nullopt, references, never).report);
    CHECK(json["mode"] == "relief");
    CHECK(json["unit"] == "mm");
    CHECK(json["complete"] == true);
    CHECK(json["scale"]["bins"] == 15);
    CHECK_THAT(json["scale"]["from"].get<double>(), WithinAbs(-0.3, 1e-4));
    CHECK_THAT(json["scale"]["to"].get<double>(), WithinAbs(0.3, 1e-4));
    CHECK_THAT(json["scale"]["step"].get<double>(), WithinAbs(0.04, 1e-4));
    CHECK(json["scale"]["range_source"] == "measured_p99");
    CHECK(json["how_to_read"].get<std::string>().find("0.04 mm") != std::string::npos);
    CHECK(json["how_to_read"].get<std::string>().find("within half a step") != std::string::npos);
    REQUIRE(json["legend"].size() == 15);
    CHECK(json["legend"][7]["color"] == "#ffffff");
    CHECK_THAT(json["legend"][0]["from"].get<double>(), WithinAbs(-0.3, 1e-4));
    CHECK_THAT(json["legend"][14]["to"].get<double>(), WithinAbs(0.3, 1e-4));
    CHECK(json["objects"][0]["object_id"] == 4);
    CHECK(json["objects"][0]["parts"][0]["reference"] == "unbaked_surface");
    CHECK(json["not_coloured"].empty());
}

namespace {

// A vertical sheet in the XZ plane, x from 0 to `width`, z from z0 to z1, cut into nx by nz quads of two triangles each.
indexed_triangle_set sheet(double width, double z0, double z1, int nx, int nz)
{
    indexed_triangle_set its;
    for (int k = 0; k <= nz; ++k)
        for (int i = 0; i <= nx; ++i)
            its.vertices.emplace_back(float(width * i / nx), 0.f, float(z0 + (z1 - z0) * k / nz));
    const auto at = [nx](int i, int k) { return k * (nx + 1) + i; };
    for (int k = 0; k < nz; ++k)
        for (int i = 0; i < nx; ++i) {
            its.indices.emplace_back(at(i, k), at(i + 1, k), at(i + 1, k + 1));
            its.indices.emplace_back(at(i, k), at(i + 1, k + 1), at(i, k + 1));
        }
    return its;
}

// 0 to 10 mm and 20 to 30 mm cut finely (0.5 mm quads), 10 to 20 mm in 5 mm quads: a band of coarse flat triangles.
indexed_triangle_set banded_wall()
{
    indexed_triangle_set wall;
    for (const indexed_triangle_set& part : {sheet(10., 0., 10., 20, 20), sheet(10., 10., 20., 2, 2), sheet(10., 20., 30., 20, 20)}) {
        const int offset = int(wall.vertices.size());
        wall.vertices.insert(wall.vertices.end(), part.vertices.begin(), part.vertices.end());
        for (const stl_triangle_vertex_indices& t : part.indices)
            wall.indices.emplace_back(t(0) + offset, t(1) + offset, t(2) + offset);
    }
    return wall;
}

} // namespace

TEST_CASE("Relief is measured in bed millimetres whatever the instance's scale", "[Relief][orcamcp]")
{
    const indexed_triangle_set before = plane(10., 5, 0.), after = patch(0.4);
    const Vec3d scales[] = {Vec3d(1.5, 1.5, 1.5), Vec3d(1.2, 1., 1.5)};   // uniform, and not
    for (const Vec3d& scale : scales) {
        Transform3d instance = Transform3d::Identity();
        instance.scale(scale);

        // Previewed: the preview and the mesh it was made from are both in the volume's frame.
        BakeReferences      none;
        ReliefPartInput     previewed = part_of(before, 1, 0, 5);
        const indexed_triangle_set raised = after;
        previewed.previewed = &raised;
        previewed.to_world  = instance;
        CHECK_THAT(compute_relief({previewed}, ReliefMode::Relief, std::nullopt, none, never).report.parts[0].max, WithinAbs(0.4 * scale.z(), 1e-4));

        // After a bake: the reference is the pre-bake mesh in the frame the volume had then.
        BakeReferences kept;
        kept.remember(ObjectID(9), mesh_fingerprint(after), before, Transform3d::Identity());
        ReliefPartInput baked = part_of(after, 2, 1, 9);
        baked.to_world = instance;   // the volume matrix is the identity: the world matrix is the instance's
        const ReliefOutput baked_out = compute_relief({baked}, ReliefMode::Relief, std::nullopt, kept, never);
        const ReliefPartReport& r = baked_out.report.parts[0];
        REQUIRE(r.coloured);
        CHECK_THAT(r.max, WithinAbs(0.4 * scale.z(), 1e-4));
        CHECK_THAT(r.min, WithinAbs(0.4 * scale.z(), 1e-4));

        // The volume was lifted by 1 mm after the bake: the drawn part is 1 mm (in the object's frame) above the kept one.
        baked.volume_matrix = Transform3d::Identity();
        baked.volume_matrix.translate(Vec3d(0., 0., 1.));
        baked.to_world = instance * baked.volume_matrix;
        CHECK_THAT(compute_relief({baked}, ReliefMode::Relief, std::nullopt, kept, never).report.parts[0].min, WithinAbs(1.4 * scale.z(), 1e-4));
    }
}

TEST_CASE("The relief scale spans the 99th percentile, so one spike does not wash the rest out", "[Relief][orcamcp]")
{
    BakeReferences references;
    indexed_triangle_set drawn = plane(10., 20, 0.1);
    drawn.vertices[10 * 21 + 10].z() = 5.f;   // one vertex pulled up: six facets of 800
    ReliefPartInput part = part_of(plane(10., 5, 0.), 1, 0, 5);
    part.previewed = &drawn;
    const ReliefOutput out = compute_relief({part}, ReliefMode::Relief, std::nullopt, references, never);
    REQUIRE(out.report.parts[0].coloured);
    CHECK_THAT(out.report.ramp.to, WithinAbs(0.1, 0.02));
    CHECK(out.report.parts[0].max > 1.);                  // the true extreme is reported
    CHECK(out.report.parts[0].clamped_high == 6);
    const nlohmann::json json = relief_report_json(out.report);
    CHECK(json["scale"]["range_source"] == "measured_p99");
    CHECK(json["notes"].dump().find("99th percentile") != std::string::npos);
}

TEST_CASE("A degenerate facet is no neighbour: a sliver does not read 90 degrees", "[Relief][orcamcp]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0), Vec3f(0.5f, 0, 0)};
    its.indices  = {stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(1, 0, 3)};   // the second is three points on a line
    const DetailMetrics m = detail_metrics(its, Transform3d::Identity(), never);
    REQUIRE(m.complete);
    CHECK_THAT(m.angle_deg[0], WithinAbs(0., 1e-3));
    CHECK_THAT(m.angle_deg[1], WithinAbs(0., 1e-3));
}

TEST_CASE("A band of coarse flat triangles is invisible to detail and shown by triangle_size and the slab medians", "[Relief][orcamcp]")
{
    const indexed_triangle_set wall = banded_wall();

    const DetailMetrics m = detail_metrics(wall, Transform3d::Identity(), never);
    REQUIRE(m.complete);
    for (const float a : m.angle_deg)
        CHECK_THAT(a, WithinAbs(0., 1e-3));   // every facet is flat: the angle cannot tell the band from the rest

    REQUIRE_FALSE(m.slabs.empty());
    for (const SizeSlab& slab : m.slabs) {
        const double middle = (slab.z_from + slab.z_to) / 2.;
        if (middle > 11. && middle < 19.)
            CHECK(slab.median_edge_mm > 5.);
        else if (middle < 9. || middle > 21.)
            CHECK(slab.median_edge_mm < 1.);
    }

    BakeReferences references;
    const ReliefOutput out = compute_relief({part_of(wall, 1, 0, 1)}, ReliefMode::TriangleSize, std::nullopt, references, never);
    REQUIRE(out.report.parts[0].coloured);
    CHECK(out.report.ramp.log);
    CHECK(out.report.ramp.bins() == 10);
    // the coarse facets (the 8 after the first 800) fall in a higher bin than the fine ones
    const std::vector<std::uint8_t> bins = ramp_bins(m.size_mm, out.report.ramp);
    CHECK(bins[800] > bins[0]);
    CHECK(bins[807] > bins[0]);
    CHECK(bins[808] == bins[0]);   // the second fine strip

    const nlohmann::json json = relief_report_json(out.report);
    CHECK(json["mode"] == "triangle_size");
    CHECK(json["scale"]["spacing"] == "log");
    const nlohmann::json& part = json["objects"][0]["parts"][0];
    CHECK(part["longest_edge_mm"]["max"].get<double>() > 7.);
    CHECK(part["slabs"].size() == m.slabs.size());
    CHECK_THAT(json["legend"][0]["from"].get<double>(), WithinAbs(out.report.ramp.from, 1e-3));
    CHECK_THAT(json["legend"][9]["to"].get<double>(), WithinAbs(out.report.ramp.to, 1e-3));
    CHECK(json["notes"].dump().find("unbudgeted preview") != std::string::npos);

    // detail reports the slabs too
    const nlohmann::json detail = relief_report_json(compute_relief({part_of(wall, 1, 0, 1)}, ReliefMode::Detail, std::nullopt, references, never).report);
    CHECK(detail["objects"][0]["parts"][0]["slabs"].size() == m.slabs.size());
}

TEST_CASE("Slabs are 2 mm, at most 64, the median of the longest edges, and leave empty slabs out", "[Relief][orcamcp]")
{
    const std::vector<SizeSlab> slabs = size_slabs({0.5f, 1.f, 1.5f, 5.f}, {1.f, 3.f, 2.f, 9.f});
    REQUIRE(slabs.size() == 2);
    CHECK_THAT(slabs[0].z_from, WithinAbs(0.5, 1e-9));
    CHECK(slabs[0].triangles == 3);
    CHECK_THAT(slabs[0].median_edge_mm, WithinAbs(2., 1e-6));
    CHECK(slabs[1].triangles == 1);

    std::vector<float> z, size;
    for (int i = 0; i <= 1000; ++i) { z.push_back(float(i)); size.push_back(1.f); }
    const std::vector<SizeSlab> tall = size_slabs(z, size);
    CHECK(tall.size() <= size_slab_max);
    std::size_t counted = 0;
    for (const SizeSlab& slab : tall) counted += slab.triangles;
    CHECK(counted == z.size());
    CHECK(size_slabs({}, {}).empty());
}
