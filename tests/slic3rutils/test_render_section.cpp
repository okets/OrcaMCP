#include <catch2/catch_all.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderSection.hpp"

// render_plate_view's section: the request, the plane in both conventions, the cut of a body (area, islands, faces),
// the answer, and the 3D view's own plane arithmetic that "app" and the ratio form use.

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using json = nlohmann::json;

namespace {

std::string refusal_of(const json& section)
{
    try {
        read_section(section);
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

void shift(indexed_triangle_set& its, const Vec3f& by)
{
    for (Vec3f& v : its.vertices)
        v += by;
}

std::shared_ptr<const TriangleMesh> cube(double edge, const Vec3d& corner = Vec3d::Zero())
{
    indexed_triangle_set its = its_make_cube(edge, edge, edge);
    shift(its, corner.cast<float>());
    return std::make_shared<const TriangleMesh>(std::move(its));
}

SectionBody body_of(std::shared_ptr<const TriangleMesh> mesh, const Transform3d& world = Transform3d::Identity())
{
    SectionBody body;
    body.object_id = 4;
    body.instance_id = 1;
    body.parts.push_back({std::move(mesh), world, ObjectID(), 0, false});
    return body;
}

const std::function<bool()> never = [] { return false; };

double triangles_area(const indexed_triangle_set& its)
{
    double area = 0.;
    for (const stl_triangle_vertex_indices& t : its.indices)
        area += 0.5 * (its.vertices[t[1]] - its.vertices[t[0]]).cross(its.vertices[t[2]] - its.vertices[t[0]]).cast<double>().norm();
    return area;
}

} // namespace

TEST_CASE("A section is read in each of its forms and refused otherwise", "[RenderSection][orcamcp]")
{
    const SectionRequest app = read_section("app");
    CHECK(app.form == SectionRequest::Form::App);

    const SectionRequest axis = read_section({{"plane", "z"}, {"at_mm", 15}});
    CHECK(axis.form == SectionRequest::Form::Axis);
    CHECK(axis.normal == Vec3d(0, 0, 1));
    CHECK(axis.point == Vec3d(0, 0, 15));
    const SectionRequest below = read_section({{"plane", "x"}, {"at_mm", 4}, {"remove", "below"}, {"frame", "plate_local"}});
    CHECK(below.normal == Vec3d(-1, 0, 0));
    CHECK(below.point == Vec3d(4, 0, 0));
    CHECK(below.plate_local);

    const SectionRequest point = read_section({{"normal", {0, 0, 5}}, {"point", {1, 2, 3}}});
    CHECK(point.form == SectionRequest::Form::PointNormal);
    CHECK(point.normal.isApprox(Vec3d(0, 0, 1)));
    CHECK(point.point == Vec3d(1, 2, 3));

    const SectionRequest ratio = read_section({{"normal", {0, 1, 0}}, {"ratio", 0.25}});
    CHECK(ratio.form == SectionRequest::Form::Ratio);
    CHECK(ratio.ratio == 0.25);

    CHECK(refusal_of("sideways").find("section must be") == 0);
    CHECK(refusal_of(json::array()).find("section must be") == 0);
    CHECK(refusal_of({{"plane", "w"}, {"at_mm", 1}}).find("\"x\", \"y\" or \"z\"") != std::string::npos);
    CHECK(refusal_of({{"plane", "z"}}).find("at_mm") != std::string::npos);
    CHECK(refusal_of({{"plane", "z"}, {"at_mm", 1}, {"normal", {0, 0, 1}}}).find("one form") != std::string::npos);
    CHECK(refusal_of({{"normal", {0, 0, 0}}, {"point", {0, 0, 0}}}).find("zero vector") != std::string::npos);
    CHECK(refusal_of({{"normal", {0, 0, 1}}}).find("needs a point or a ratio") != std::string::npos);
    CHECK(refusal_of({{"normal", {0, 0, 1}}, {"point", {0, 0, 0}}, {"ratio", 0.5}}).find("not both") != std::string::npos);
    CHECK(refusal_of({{"normal", {0, 0, 1}}, {"ratio", 0.}}).find("above 0") != std::string::npos);
    CHECK(refusal_of({{"normal", {0, 0, 1}}, {"ratio", 1.5}}).find("at most 1") != std::string::npos);
    CHECK(refusal_of({{"normal", {0, 0, 1}}, {"ratio", 0.5}, {"frame", "plate_local"}}).find("frame is for a point") != std::string::npos);
    CHECK(refusal_of({{"normal", {0, 0}}, {"point", {0, 0, 0}}}).find("three numbers") != std::string::npos);
    CHECK(refusal_of({{"plane", "z"}, {"at_mm", 1}, {"remove", "left"}}).find("above") != std::string::npos);
    CHECK(refusal_of({{"plane", "z"}, {"at_mm", 1}, {"depth", 1}}).find("has no key \"depth\"") != std::string::npos);
}

TEST_CASE("A section is refused with a layer_view and read from a whole request", "[RenderSection][RenderRequest][orcamcp]")
{
    std::string error;
    auto request = read_render_request({{"plate_index", 0}, {"section", {{"plane", "z"}, {"at_mm", 5}}}}, error);
    REQUIRE(request.has_value());
    REQUIRE(request->section.has_value());
    CHECK(request->section->form == SectionRequest::Form::Axis);

    std::string refused;
    REQUIRE_FALSE(read_render_request({{"plate_index", 0}, {"section", "app"}, {"layer_view", "first_layer"}}, refused).has_value());
    CHECK(refused.find("section is for 3D views") == 0);
    refused.clear();
    REQUIRE_FALSE(read_render_request({{"plate_index", 0}, {"section", {{"plane", "q"}, {"at_mm", 5}}}}, refused).has_value());
    CHECK(refused.find("section.plane must be") == 0);
    CHECK_FALSE(read_render_request({{"plate_index", 0}}, error)->section.has_value());
}

TEST_CASE("The plane removes the side its normal points to, in the 3D view's convention and the shader's", "[RenderSection][orcamcp]")
{
    const SectionPlane plane = plane_through_point(Vec3d(0, 0, 2), Vec3d(7, 8, 15));
    CHECK(plane.normal == Vec3d(0, 0, 1));
    CHECK(plane.offset == Catch::Approx(15.));

    // The 3D view's own ClippingPlane says which points it clips; the section must agree with it.
    const ClippingPlane app(plane.normal, plane.offset);
    const std::array<double, 4> shader = shader_clipping_plane(plane);
    for (const Vec3d& p : {Vec3d(0, 0, 20), Vec3d(3, 4, 15.5), Vec3d(0, 0, 10), Vec3d(9, 9, -3)}) {
        INFO(p.transpose());
        const bool removed = plane.signed_distance(p) > 0.;
        CHECK(removed == app.is_point_clipped(p));
        // The shader discards where dot(position, plane) < 0.
        CHECK(removed == (shader[0] * p.x() + shader[1] * p.y() + shader[2] * p.z() + shader[3] < 0.));
    }
    CHECK(plane.signed_distance(Vec3d(0, 0, 20)) == Catch::Approx(5.));
}

TEST_CASE("The section view's plane sweeps the bounding sphere", "[RenderSection][orcamcp]")
{
    BoundingBoxf3 box(Vec3d(0, 0, 0), Vec3d(20, 20, 20));
    const double radius = 0.5 * std::sqrt(1200.);   // half the diagonal, written by hand
    // ratio 0: nothing is cut (the section view is off).
    CHECK_FALSE(GLCanvas3D::section_view_plane(box, Vec3d::UnitZ(), 0.).is_active());
    // ratio 1: the plane is on the far end of the sphere along the normal: everything is removed above 10 - r.
    const ClippingPlane far_end = GLCanvas3D::section_view_plane(box, Vec3d::UnitZ(), 1.);
    REQUIRE(far_end.is_active());
    CHECK(far_end.get_offset() == Catch::Approx(10. - radius));
    // A small ratio starts at the near end: just inside 10 + r.
    CHECK(GLCanvas3D::section_view_plane(box, Vec3d::UnitZ(), 1e-6).get_offset() == Catch::Approx(10. + radius).margin(1e-3));
    // Half way: through the centre; and along another normal the centre's own coordinate along it.
    CHECK(GLCanvas3D::section_view_plane(box, Vec3d::UnitZ(), 0.5).get_offset() == Catch::Approx(10.));
    const Vec3d diagonal = Vec3d(1, 1, 0).normalized();
    CHECK(GLCanvas3D::section_view_plane(box, diagonal, 0.5).get_offset() == Catch::Approx(diagonal.dot(Vec3d(10, 10, 10))));
    CHECK_FALSE(GLCanvas3D::section_view_plane(BoundingBoxf3(), Vec3d::UnitZ(), 0.5).is_active());
}

TEST_CASE("A cube cut through its middle has the area of its square, one island and faces on the plane", "[RenderSection][orcamcp]")
{
    const SectionPlane plane = plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 10));
    const SectionCap   cap   = section_cap(body_of(cube(20)), plane, never);
    CHECK(cap.complete);
    CHECK(cap.effect == SectionEffect::Cut);
    CHECK(cap.area_mm2 == Catch::Approx(400.).epsilon(1e-4));
    CHECK(cap.islands == 1);
    CHECK(cap.object_id == 4);
    CHECK(cap.instance_id == 1);
    // The faces cover the same square, sit on the plane (a hair toward the removed side) and face it.
    CHECK(triangles_area(cap.triangles) == Catch::Approx(400.).epsilon(1e-4));
    REQUIRE_FALSE(cap.triangles.vertices.empty());
    for (const Vec3f& v : cap.triangles.vertices)
        CHECK(v.z() == Catch::Approx(10.001).margin(1e-4));
    const stl_triangle_vertex_indices& t = cap.triangles.indices.front();
    const Vec3f normal = (cap.triangles.vertices[t[1]] - cap.triangles.vertices[t[0]]).cross(cap.triangles.vertices[t[2]] - cap.triangles.vertices[t[0]]);
    CHECK(normal.z() > 0.f);
}

TEST_CASE("A tilted plane cuts a longer section", "[RenderSection][orcamcp]")
{
    // x + z = 20 through the cube's centre: 20 wide in y, 20 * sqrt(2) long.
    const SectionPlane plane = plane_through_point(Vec3d(1, 0, 1), Vec3d(10, 10, 10));
    const SectionCap   cap   = section_cap(body_of(cube(20)), plane, never);
    CHECK(cap.effect == SectionEffect::Cut);
    CHECK(cap.area_mm2 == Catch::Approx(400. * std::sqrt(2.)).epsilon(1e-4));
    CHECK(cap.islands == 1);
    CHECK(triangles_area(cap.triangles) == Catch::Approx(400. * std::sqrt(2.)).epsilon(1e-4));
    // The faces lie on the plane.
    for (const Vec3f& v : cap.triangles.vertices)
        CHECK(plane.signed_distance(v.cast<double>()) == Catch::Approx(0.001).margin(1e-4));
}

TEST_CASE("A hollow box cuts to a frame: the outer square less the inner one", "[RenderSection][orcamcp]")
{
    // The inner cube as a negative volume of the body.
    SectionBody body = body_of(cube(20));
    body.parts.push_back({cube(10, Vec3d(5, 5, 5)), Transform3d::Identity(), ObjectID(), 1, true});
    const SectionPlane plane = plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 10));
    const SectionCap   cap   = section_cap(body, plane, never);
    CHECK(cap.area_mm2 == Catch::Approx(300.).epsilon(1e-4));
    CHECK(cap.islands == 1);
    CHECK(triangles_area(cap.triangles) == Catch::Approx(300.).epsilon(1e-4));

    // The same box as one mesh with a cavity (an inner shell turned inside out).
    indexed_triangle_set walls = its_make_cube(20, 20, 20);
    indexed_triangle_set cavity = its_make_cube(10, 10, 10);
    shift(cavity, Vec3f(5, 5, 5));
    its_flip_triangles(cavity);
    its_merge(walls, cavity);
    const SectionCap shell = section_cap(body_of(std::make_shared<const TriangleMesh>(std::move(walls))), plane, never);
    CHECK(shell.area_mm2 == Catch::Approx(300.).epsilon(1e-4));
    CHECK(shell.islands == 1);

    // A cut above the cavity is the whole square again.
    CHECK(section_cap(body, plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 18)), never).area_mm2 == Catch::Approx(400.).epsilon(1e-4));
}

TEST_CASE("Two separate pieces cut to two islands, and a mirrored placement cuts the same", "[RenderSection][orcamcp]")
{
    SectionBody body = body_of(cube(10));
    body.parts.push_back({cube(10, Vec3d(30, 0, 0)), Transform3d::Identity(), ObjectID(), 1, false});
    const SectionPlane plane = plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 5));
    const SectionCap   two   = section_cap(body, plane, never);
    CHECK(two.islands == 2);
    CHECK(two.area_mm2 == Catch::Approx(200.).epsilon(1e-4));

    // Mirrored in x and moved: still a 10 mm square.
    Transform3d mirrored = Transform3d::Identity();
    mirrored.translate(Vec3d(50, 0, 0));
    mirrored.scale(Vec3d(-1, 1, 1));
    const SectionCap flipped = section_cap(body_of(cube(10), mirrored), plane, never);
    CHECK(flipped.area_mm2 == Catch::Approx(100.).epsilon(1e-4));
    CHECK(flipped.islands == 1);
    REQUIRE_FALSE(flipped.triangles.vertices.empty());
    for (const Vec3f& v : flipped.triangles.vertices) {
        CHECK(v.x() <= 50.001f);
        CHECK(v.x() >= 39.999f);
    }
}

TEST_CASE("A plane that misses a body keeps it or removes it whole, and says so", "[RenderSection][orcamcp]")
{
    // Above the cube, removing what is above: nothing is removed.
    SectionCap above = section_cap(body_of(cube(20)), plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 30)), never);
    CHECK(above.effect == SectionEffect::KeepsAll);
    CHECK(above.area_mm2 == 0.);
    CHECK(above.islands == 0);
    CHECK(above.triangles.indices.empty());
    // Below it, removing what is above: everything.
    CHECK(section_cap(body_of(cube(20)), plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, -5)), never).effect == SectionEffect::RemovesAll);
    // Below it with the normal turned down: the cube is on the kept side.
    CHECK(section_cap(body_of(cube(20)), plane_through_point(-Vec3d::UnitZ(), Vec3d(0, 0, -5)), never).effect == SectionEffect::KeepsAll);
    // Touching it is not a cut.
    CHECK(section_cap(body_of(cube(20)), plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 20)), never).effect == SectionEffect::KeepsAll);
    CHECK(std::string(section_effect_name(SectionEffect::RemovesAll)) == "removes_all");
}

TEST_CASE("A section stops where it is told to", "[RenderSection][orcamcp]")
{
    SectionBody body = body_of(cube(20));
    body.parts.push_back({cube(5), Transform3d::Identity(), ObjectID(), 1, false});
    const SectionPlane plane = plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 3));
    int calls = 0;
    const SectionCap cut = section_cap(body, plane, [&calls] { return ++calls > 1; });   // asked per part: the second part is not reached
    CHECK_FALSE(cut.complete);
    CHECK(calls >= 2);
    CHECK(section_cap(body, plane, [] { return true; }).complete == false);
}

TEST_CASE("The answer lists the drawn objects, adds instances up and explains an empty cut", "[RenderSection][orcamcp]")
{
    const SectionPlane plane = plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 10));
    SectionRequest asked = read_section({{"plane", "z"}, {"at_mm", 10}});

    SectionCap first;  first.object_id = 2; first.instance_id = 0; first.effect = SectionEffect::Cut; first.area_mm2 = 400.; first.islands = 1;
    SectionCap second; second.object_id = 2; second.instance_id = 1; second.effect = SectionEffect::Cut; second.area_mm2 = 100.; second.islands = 2;
    SectionCap hidden; hidden.object_id = 5; hidden.instance_id = 0; hidden.effect = SectionEffect::Cut; hidden.area_mm2 = 9.; hidden.islands = 1;
    SectionCap other;  other.object_id = 3; other.instance_id = 0; other.effect = SectionEffect::KeepsAll;

    const json answer = section_json(asked, plane, {first, second, hidden, other}, {{2, 0}, {2, 1}, {3, 0}});
    CHECK(answer["normal"] == json::array({0.0, 0.0, 1.0}));
    CHECK(answer["point"] == json::array({0.0, 0.0, 10.0}));
    CHECK(answer["source"] == "plane");
    CHECK(answer["empty"] == false);
    CHECK_FALSE(answer.contains("notes"));
    CHECK_FALSE(answer.contains("ratio_used"));
    REQUIRE(answer["objects"].size() == 2);
    CHECK(answer["objects"][0]["object_id"] == 2);
    CHECK(answer["objects"][0]["section_area_mm2"] == Catch::Approx(500.));
    CHECK(answer["objects"][0]["islands"] == 3);
    CHECK(answer["objects"][0]["instances"].size() == 2);
    CHECK(answer["objects"][1]["effect"] == "keeps_all");

    // Nothing cut: empty, with the reason.
    const json none = section_json(asked, plane, {other}, {{3, 0}});
    CHECK(none["empty"] == true);
    REQUIRE(none["notes"].size() == 1);
    CHECK(none["notes"][0].get<std::string>().find("uncut view") != std::string::npos);
    CHECK_FALSE(none.contains("applied"));
    SectionCap gone; gone.object_id = 3; gone.instance_id = 0; gone.effect = SectionEffect::RemovesAll;
    CHECK(section_json(asked, plane, {gone}, {{3, 0}})["notes"][0].get<std::string>().find("removes every object") != std::string::npos);
    CHECK(section_json(asked, plane, {}, {})["notes"][0].get<std::string>().find("Nothing was drawn") != std::string::npos);

    // The ratio form says the ratio it used.
    SectionRequest swept = read_section({{"normal", {0, 0, 1}}, {"ratio", 0.5}});
    CHECK(section_json(swept, plane, {first}, {{2, 0}})["ratio_used"] == 0.5);
    CHECK(section_json(swept, plane, {first}, {{2, 0}})["source"] == "ratio");
}

TEST_CASE("What a section answers goes back in as the same plane", "[RenderSection][orcamcp]")
{
    const SectionPlane plane = plane_through_point(Vec3d(1, 0, 1), Vec3d(10, 10, 10));
    SectionCap cut; cut.object_id = 0; cut.instance_id = 0; cut.effect = SectionEffect::Cut; cut.area_mm2 = 5.; cut.islands = 1;
    const json answer = section_json(read_section({{"normal", {1, 0, 1}}, {"ratio", 0.4}}), plane, {cut}, {{0, 0}});
    const SectionRequest again = read_section(answer);
    REQUIRE(again.form == SectionRequest::Form::PointNormal);
    const SectionPlane back = plane_through_point(again.normal, again.point);
    CHECK(back.normal.isApprox(plane.normal, 1e-9));
    CHECK(back.offset == Catch::Approx(plane.offset));
}

TEST_CASE("A sunk part is cut and counted above the bed only", "[RenderSection][orcamcp]")
{
    // A 20 mm cube from z = -10 to 10, cut by x = 5: the full square is 400, the part at z >= 0 is 20 x 10.
    Transform3d sunk = Transform3d::Identity();
    sunk.translate(Vec3d(0, 0, -10));
    const SectionCap vertical = section_cap(body_of(cube(20), sunk), plane_through_point(Vec3d::UnitX(), Vec3d(5, 0, 0)), never);
    CHECK(vertical.area_mm2 == Catch::Approx(200.).margin(0.1));
    CHECK(vertical.islands == 1);
    CHECK(triangles_area(vertical.triangles) == Catch::Approx(vertical.area_mm2).margin(0.1));
    for (const Vec3f& v : vertical.triangles.vertices)
        CHECK(v.z() >= -0.0011f);
    // A tilted plane (x + z = 10 here) is clipped on its slanted face too: the cut runs from (10, -, 0) up to (0, -, 10).
    const SectionCap tilted = section_cap(body_of(cube(20), sunk), plane_through_point(Vec3d(1, 0, 1), Vec3d(5, 0, 5)), never);
    for (const Vec3f& v : tilted.triangles.vertices)
        CHECK(v.z() >= -0.0011f);
    CHECK(tilted.area_mm2 > 0.);
    // A horizontal cut wholly below the bed has nothing left; one above it is whole.
    CHECK(section_cap(body_of(cube(20), sunk), plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, -5)), never).area_mm2 == 0.);
    CHECK(section_cap(body_of(cube(20), sunk), plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 5)), never).area_mm2 == Catch::Approx(400.).epsilon(1e-4));
}

TEST_CASE("An object with open edges is named in the section's notes", "[RenderSection][orcamcp]")
{
    SectionBody body = body_of(cube(20));
    body.parts[0].open_edges = true;
    body.object_id = 7;
    const SectionPlane plane = plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, 10));
    const SectionCap   cap   = section_cap(body, plane, never);
    CHECK(cap.open_edges);
    CHECK_FALSE(section_cap(body_of(cube(20)), plane, never).open_edges);
    const json answer = section_json(read_section({{"plane", "z"}, {"at_mm", 10}}), plane, {cap}, {{7, 1}});
    REQUIRE(answer.contains("notes"));
    CHECK(answer["notes"][0].get<std::string>().find("Object 7 has open edges") == 0);
    // Not for an object the picture does not show, nor a negative part's open edges.
    CHECK(section_json(read_section({{"plane", "z"}, {"at_mm", 10}}), plane, {cap}, {{3, 0}}).dump().find("open edges") == std::string::npos);
    body.parts[0].open_edges = false;
    body.parts.push_back({cube(5), Transform3d::Identity(), ObjectID(), 1, true, true});
    CHECK_FALSE(section_cap(body, plane, never).open_edges);
}

TEST_CASE("A plane that removed everything is told apart from one that cut nothing", "[RenderSection][orcamcp]")
{
    const SectionPlane plane = plane_through_point(Vec3d::UnitZ(), Vec3d(0, 0, -5));
    SectionCap gone; gone.object_id = 1; gone.instance_id = 0; gone.effect = SectionEffect::RemovesAll;
    SectionCap kept; kept.object_id = 2; kept.instance_id = 0; kept.effect = SectionEffect::KeepsAll;
    const SectionRequest asked = read_section({{"plane", "z"}, {"at_mm", -5}});
    CHECK(section_removed_everything(section_json(asked, plane, {gone}, {{1, 0}})));
    CHECK_FALSE(section_removed_everything(section_json(asked, plane, {kept}, {{2, 0}})));
    CHECK_FALSE(section_removed_everything(section_json(asked, plane, {gone, kept}, {{1, 0}, {2, 0}})));
    CHECK_FALSE(section_removed_everything(section_json(asked, plane, {}, {})));
}

TEST_CASE("The box a ratio sweeps is the plate's instances, not its footprint", "[RenderSection][orcamcp]")
{
    const BoundingBoxf3 footprint(Vec3d(0, 0, 0), Vec3d(256, 256, 10));
    const BoundingBoxf3 cube_box(Vec3d(100, 100, 0), Vec3d(120, 120, 20));
    const BoundingBoxf3 other(Vec3d(130, 100, 0), Vec3d(150, 120, 20));
    const BoundingBoxf3 current(Vec3d(0, 0, 0), Vec3d(40, 40, 40)), all(Vec3d(0, 0, 0), Vec3d(500, 500, 40)), none;

    // Another plate: its own instances merged -- the sphere of two 20 mm cubes, not of the 256 mm bed.
    BoundingBoxf3 box = section_sweep_box(false, false, current, all, {cube_box, other}, footprint);
    CHECK(box.min == Vec3d(100, 100, 0));
    CHECK(box.max == Vec3d(150, 120, 20));
    // A ratio of 0.5 through a lone cube's sphere is through its centre (a bed-sized box would put it elsewhere).
    CHECK(GLCanvas3D::section_view_plane(section_sweep_box(false, false, current, all, {cube_box}, footprint), Vec3d::UnitZ(), 0.5).get_offset() == Catch::Approx(10.));
    // The current plate: the view's own box; an empty plate with no instances: the footprint.
    CHECK(section_sweep_box(false, true, current, all, {cube_box}, footprint).max == Vec3d(40, 40, 40));
    CHECK(section_sweep_box(false, true, none, all, {cube_box}, footprint).max == Vec3d(120, 120, 20));   // a ratio does not borrow the other plates
    CHECK(section_sweep_box(false, false, current, all, {}, footprint).max == Vec3d(256, 256, 10));
    // "app" is the view's: the current plate's volumes whatever plate is drawn, else all volumes (upstream's fallback).
    CHECK(section_sweep_box(true, false, current, all, {cube_box}, footprint).max == Vec3d(40, 40, 40));
    CHECK(section_sweep_box(true, false, none, all, {cube_box}, footprint).max == Vec3d(500, 500, 40));
    CHECK(section_sweep_box(true, true, none, none, {cube_box}, footprint).max == Vec3d(120, 120, 20));
}
