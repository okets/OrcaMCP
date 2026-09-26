#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

#include <boost/filesystem.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"

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

// Where the images go. They used to be written to a literal /tmp/, which is not a directory on
// Windows, and the render files were never cleaned up.

TEST_CASE("images are written to the temp directory under names that never repeat", "[RenderMath]")
{
    namespace fs = boost::filesystem;
    const std::string first  = new_mcp_image_path(k_render_image_prefix, "0", ".png");
    const std::string second = new_mcp_image_path(k_render_image_prefix, "0", ".png");
    CHECK(first != second);  // two renders in the same second used to overwrite each other
    CHECK(fs::path(first).parent_path() == fs::path(mcp_image_directory()));
    CHECK(fs::equivalent(fs::path(mcp_image_directory()), fs::temp_directory_path()));
    CHECK(is_mcp_image_file_name(fs::path(first).filename().string()));
}

TEST_CASE("only this server's image names count as its images", "[RenderMath]")
{
    CHECK(is_mcp_image_file_name("orcamcp_render_1789763152_1_0.png"));
    CHECK(is_mcp_image_file_name("orcamcp_preview_1789763152_0_grid.jpg"));
    CHECK_FALSE(is_mcp_image_file_name("orcamcp_render_1789763152_1_0.txt"));
    CHECK_FALSE(is_mcp_image_file_name("my_orcamcp_render_1.png"));
    CHECK_FALSE(is_mcp_image_file_name("id_rsa"));
}

TEST_CASE("an image path is readable only directly inside the temp directory", "[RenderMath]")
{
    namespace fs = boost::filesystem;
    const std::string image = new_mcp_image_path(k_preview_image_prefix, "test", ".jpg");
    std::ofstream(image) << "not really a jpeg";
    CHECK(is_mcp_image_path(image));

    const fs::path dir(mcp_image_directory());
    CHECK_FALSE(is_mcp_image_path((dir / "orcamcp_render_" / ".." / "secret.png").string()));  // a name check passed this
    CHECK_FALSE(is_mcp_image_path((dir / "sub" / "orcamcp_render_1_0_0.png").string()));
    CHECK_FALSE(is_mcp_image_path((dir / "notes.png").string()));
    CHECK_FALSE(is_mcp_image_path("orcamcp_render_1_0_0.png"));  // relative: not inside it
    fs::remove(image);
}
