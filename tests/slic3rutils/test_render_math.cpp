#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>

#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "slic3r/GUI/Camera.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;

// An orthographic camera straight above a w x h mm patch of bed, 1 mm per pixel, +Y up in the
// image. View stays identity (camera at the origin looking down -Z), so the projection alone maps
// x in [0,w] -> [-1,1] and y in [0,h] -> [-1,1].
static CameraFrame ortho_top(int w, int h)
{
    CameraFrame c;
    c.viewport   = {0, 0, w, h};
    c.projection = Eigen::Matrix4d::Identity();
    c.projection(0, 0) = 2.0 / w;
    c.projection(1, 1) = 2.0 / h;
    c.projection(0, 3) = -1.0;
    c.projection(1, 3) = -1.0;
    c.projection(2, 2) = -1.0;
    return c;
}

TEST_CASE("project_point_to_pixel maps bed mm to top-left pixels", "[RenderMath]")
{
    Vec2d px;
    REQUIRE(project_point_to_pixel(ortho_top(100, 100), Vec3d(25., 75., 0.), px));
    CHECK_THAT(px.x(), Catch::Matchers::WithinAbs(25.0, 1e-9));
    CHECK_THAT(px.y(), Catch::Matchers::WithinAbs(25.0, 1e-9));  // 75 mm up the bed is 25 px down from the top

    CameraFrame flat = ortho_top(100, 100);
    flat.viewport    = {0, 0, 0, 0};
    CHECK_FALSE(project_point_to_pixel(flat, Vec3d(1., 1., 0.), px));
}

TEST_CASE("screen_bbox_of reports visibility and clipping", "[RenderMath]")
{
    const CameraFrame cam = ortho_top(100, 100);

    ScreenBBox inside = screen_bbox_of(cam, BoundingBoxf3(Vec3d(10., 10., 0.), Vec3d(30., 30., 5.)));
    CHECK(inside.visible);
    CHECK_FALSE(inside.clipped);
    CHECK_THAT(inside.x0, Catch::Matchers::WithinAbs(10., 1e-9));
    CHECK_THAT(inside.x1, Catch::Matchers::WithinAbs(30., 1e-9));
    CHECK_THAT(inside.y0, Catch::Matchers::WithinAbs(70., 1e-9));
    CHECK_THAT(inside.y1, Catch::Matchers::WithinAbs(90., 1e-9));

    ScreenBBox off = screen_bbox_of(cam, BoundingBoxf3(Vec3d(200., 200., 0.), Vec3d(300., 300., 5.)));
    CHECK_FALSE(off.visible);

    ScreenBBox part = screen_bbox_of(cam, BoundingBoxf3(Vec3d(90., 90., 0.), Vec3d(150., 150., 5.)));
    CHECK(part.visible);
    CHECK(part.clipped);
    CHECK_THAT(part.x1, Catch::Matchers::WithinAbs(100., 1e-9));
    CHECK_THAT(part.y0, Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("is_uniform_rgba spots a single-colour image", "[RenderMath]")
{
    std::vector<unsigned char> black(4u * 4u * 4u, 0);
    CHECK(is_uniform_rgba(black, 4, 4));
    black[4 * 5 + 1] = 200;
    CHECK_FALSE(is_uniform_rgba(black, 4, 4));
    std::vector<unsigned char> grey(4u * 4u * 4u, 237);
    CHECK(is_uniform_rgba(grey, 4, 4));
}

TEST_CASE("palette cycles every 12 and stays off the wipe-tower grey", "[RenderMath]")
{
    const ColorRGBA tower = wipe_tower_color();
    for (int i = 0; i < 24; ++i) {
        const ColorRGBA c = object_palette_color(i);
        CHECK(c == object_palette_color(i + 12));
        const bool near_tower = std::abs(c.r() - tower.r()) < 0.05f && std::abs(c.g() - tower.g()) < 0.05f && std::abs(c.b() - tower.b()) < 0.05f;
        CHECK_FALSE(near_tower);
    }
    // Neighbours are distinguishable: no two consecutive colours within a small distance.
    for (int i = 0; i < 12; ++i) {
        const ColorRGBA a = object_palette_color(i), b = object_palette_color(i + 1);
        const float d = std::abs(a.r() - b.r()) + std::abs(a.g() - b.g()) + std::abs(a.b() - b.b());
        CHECK(d > 0.3f);
    }
    CHECK(object_palette_color(-1) == object_palette_color(11));
}

TEST_CASE("preset cameras frame the box from the named side", "[RenderMath]")
{
    const BoundingBoxf3 box(Vec3d(300., 0., 0.), Vec3d(560., 256., 100.));
    Vec3d        p, t;
    CameraPreset preset;

    REQUIRE(camera_preset_from_string("iso", preset));
    preset_camera(preset, box, p, t);
    CHECK((t - box.center()).norm() < 1e-9);
    CHECK(p.x() > box.max.x());
    CHECK(p.y() < box.min.y());
    CHECK(p.z() > box.max.z());

    REQUIRE(camera_preset_from_string("top", preset));
    preset_camera(preset, box, p, t);
    CHECK(p.z() > box.max.z() + 100.);
    CHECK(std::abs(p.x() - t.x()) < 1e-9);

    REQUIRE(camera_preset_from_string("front", preset));
    preset_camera(preset, box, p, t);
    CHECK(p.y() < box.min.y());
    CHECK(p.z() > t.z());

    REQUIRE(camera_preset_from_string("low", preset));
    preset_camera(preset, box, p, t);
    CHECK(p.z() < box.center().z());        // below mid-height ...
    CHECK(p.z() > t.z());                   // ... but looking slightly down at the first layers
    CHECK(p.y() < box.min.y());
    CHECK(t.z() < box.center().z());

    // A tiny box still gets a camera at least 50 mm away.
    const BoundingBoxf3 tiny(Vec3d(0., 0., 0.), Vec3d(2., 2., 2.));
    REQUIRE(camera_preset_from_string("right", preset));
    preset_camera(preset, tiny, p, t);
    CHECK_THAT((p - t).norm(), Catch::Matchers::WithinAbs(50.0, 1e-9));

    CHECK_FALSE(camera_preset_from_string("sideways", preset));
    CHECK(camera_preset_name(CameraPreset::Low) == "low");
}

TEST_CASE("grid segments cover the plate at the step, majors every n", "[RenderMath]")
{
    const auto g = grid_segments(BoundingBoxf3(Vec3d(0., 0., 0.), Vec3d(100., 50., 0.)), 10.0, 5);
    CHECK(g.size() == 11u + 6u);  // x = 0..100 (11 lines) + y = 0..50 (6 lines)
    CHECK(std::count_if(g.begin(), g.end(), [](const GridSegment& s) { return s.major; }) == 3 + 2);  // x 0,50,100; y 0,50

    // A plate that does not start on a multiple of the step gets lines only inside it.
    const auto shifted = grid_segments(BoundingBoxf3(Vec3d(307.2, 0., 0.), Vec3d(563.2, 256., 0.)), 10.0, 5);
    for (const GridSegment& s : shifted) {
        CHECK(s.a.x() >= 307.2 - 1e-9);
        CHECK(s.a.x() <= 563.2 + 1e-9);
    }
    CHECK(grid_segments(BoundingBoxf3(Vec3d(0., 0., 0.), Vec3d(10., 10., 0.)), 0.0, 5).empty());
}

// Framing. The renderer used to size its zoom from the fitted box flattened to z = 0, along the
// Camera's default orientation, before look_at turned it to the requested view: the object's height
// never entered into it, so a figurine taller than it was wide overflowed every edge of an
// object-fit render and came back `clipped`, its screen box the full [0, 0, 512, 512].

namespace {

const BoundingBoxf3 k_plate(Vec3d(0., 0., 0.), Vec3d(270., 270., 300.));

// What RenderThumbnail does with its Camera, minus the GL calls.
CameraFrame framed(const Vec3d& position, const Vec3d& target, const BoundingBoxf3& fit, int w = 512, int h = 512)
{
    Slic3r::GUI::Camera camera;
    camera.set_type(Slic3r::GUI::Camera::EType::Perspective);
    camera.set_viewport(0, 0, w, h);
    frame_camera(camera, position, target, fit, k_plate);
    return camera_frame_of(camera);
}

// How much of the image the box's larger screen dimension spans, 0..1.
double fill_of(const ScreenBBox& sb, int w = 512, int h = 512)
{
    return std::max((sb.x1 - sb.x0) / w, (sb.y1 - sb.y0) / h);
}

} // namespace

TEST_CASE("every preset frames the whole box without clipping it", "[RenderMath]")
{
    // A figurine taller than wide (the case that clipped), a flat plate-sized sheet and a cube.
    const BoundingBoxf3 box = GENERATE(BoundingBoxf3(Vec3d(110., 115., 0.), Vec3d(160., 155., 99.)),
                                       BoundingBoxf3(Vec3d(30., 30., 0.), Vec3d(240., 240., 5.)),
                                       BoundingBoxf3(Vec3d(125., 125., 0.), Vec3d(145., 145., 20.)));
    const std::string name = GENERATE(as<std::string>{}, "iso", "top", "front", "back", "left", "right", "low");
    DYNAMIC_SECTION(name << " on " << box.size().transpose())
    {
        CameraPreset preset;
        REQUIRE(camera_preset_from_string(name, preset));
        Vec3d position, target;
        preset_camera(preset, box, position, target);

        const ScreenBBox sb = screen_bbox_of(framed(position, target, box), box);
        CHECK(sb.visible);
        CHECK_FALSE(sb.clipped);
        // Framed, not merely somewhere in view. "low" aims below the centre, at the first layers, so
        // the box sits off-centre in its frame and cannot fill it.
        if ((target - box.center()).norm() < 1e-9)
            CHECK(fill_of(sb) > 0.6);
    }
}

TEST_CASE("a camera aimed off the box's centre still fits the box", "[RenderMath]")
{
    // The turntable's camera looks at 40% of the objects' height, not their centre.
    const BoundingBoxf3 box(Vec3d(100., 100., 0.), Vec3d(140., 130., 80.));
    const Vec3d target(120., 115., 16.);
    const Vec3d position = target + Vec3d(250., 0., 175.);

    const ScreenBBox sb = screen_bbox_of(framed(position, target, box), box);
    CHECK(sb.visible);
    CHECK_FALSE(sb.clipped);
}

TEST_CASE("framing a wide image leaves the height as the limit", "[RenderMath]")
{
    const BoundingBoxf3 box(Vec3d(110., 115., 0.), Vec3d(160., 155., 99.));
    Vec3d position, target;
    preset_camera(CameraPreset::Front, box, position, target);

    const ScreenBBox sb = screen_bbox_of(framed(position, target, box, 1024, 256), box);
    CHECK_FALSE(sb.clipped);
    CHECK((sb.y1 - sb.y0) / 256. > 0.6);
}

TEST_CASE("fit_zoom_to_box has nothing to fit in an empty box", "[RenderMath]")
{
    CHECK(fit_zoom_to_box(Vec3d(0., -100., 50.), Vec3d::Zero(), Vec3d::UnitZ(), BoundingBoxf3(), 512, 512, k_fit_margin) == 0.0);
}

TEST_CASE("a volume partly off its plate is still in the plate's picture", "[RenderMath]")
{
    const BoundingBoxf3 overhanging(Vec3d(250., 100., 0.), Vec3d(300., 140., 30.));
    CHECK(belongs_in_plate_view(true, true, overhanging));
    CHECK_FALSE(belongs_in_plate_view(false, true, overhanging));  // not printable
    CHECK_FALSE(belongs_in_plate_view(true, false, overhanging));  // another plate's
    CHECK_FALSE(belongs_in_plate_view(true, true, BoundingBoxf3(Vec3d(0., 0., -20.), Vec3d(10., 10., -1.))));  // under the bed
}

TEST_CASE("the blank-picture hint names what was missing", "[RenderMath]")
{
    const BoundingBoxf3 plate(Vec3d(0., 0., 0.), Vec3d(270., 270., 300.));

    const std::string empty_scene = uniform_image_hint({0, 0, true}, 0, plate);
    CHECK(empty_scene.find("no model volumes") != std::string::npos);

    const std::string other_plate = uniform_image_hint({3, 0, true}, 2, plate);
    CHECK(other_plate.find("3 model volume(s)") != std::string::npos);
    CHECK(other_plate.find("plate 2") != std::string::npos);
    CHECK(other_plate.find("get_scene_info") != std::string::npos);

    const std::string looked_away = uniform_image_hint({3, 2, true}, 0, plate);
    CHECK(looked_away.find("none inside this view") != std::string::npos);

    const std::string stale = uniform_image_hint({0, 0, false}, 0, plate);
    CHECK(stale.find("could not be refreshed") != std::string::npos);
    CHECK(looked_away.find("could not be refreshed") == std::string::npos);
}
