#include <catch2/catch_all.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPFirstLayerPlan.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

TEST_CASE("plan mapping scales the plate's long side to the resolution and flips y", "[FirstLayerPlan]")
{
    const BoundingBoxf3 plate(Vec3d(307.2, 0., 0.), Vec3d(563.2, 256., 256.));  // 256 x 256 plate, plate 2's position
    const PlanMapping   m = plan_mapping(plate, 512, 0.0);
    CHECK(m.width == 512);
    CHECK(m.height == 512);
    CHECK_THAT(m.scale, WithinAbs(2.0, 1e-9));

    const Vec2d front_left = m.to_px(Vec2d(307.2, 0.));
    CHECK_THAT(front_left.x(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(front_left.y(), WithinAbs(512.0, 1e-9));  // bottom row: +Y on the bed is up in the image

    const Vec2d back_right = m.to_px(Vec2d(563.2, 256.));
    CHECK_THAT(back_right.x(), WithinAbs(512.0, 1e-9));
    CHECK_THAT(back_right.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("plan mapping keeps a margin and handles a non-square plate", "[FirstLayerPlan]")
{
    const BoundingBoxf3 plate(Vec3d(0., 0., 0.), Vec3d(200., 100., 50.));
    const PlanMapping   m = plan_mapping(plate, 400, 10.0);
    // 220 mm across incl. margins -> 400 px; 120 mm deep -> proportionally fewer rows.
    CHECK(m.width == 400);
    CHECK_THAT(double(m.height), WithinAbs(400 * 120.0 / 220.0, 1.0));
    const Vec2d origin_px = m.to_px(Vec2d(0., 0.));
    CHECK(origin_px.x() > 0.);                 // the margin is inside the image
    CHECK(origin_px.y() < m.height);
}

TEST_CASE("plan camera projects bed mm where the mapping puts them", "[FirstLayerPlan]")
{
    const BoundingBoxf3 plate(Vec3d(307.2, 0., 0.), Vec3d(563.2, 256., 256.));
    const PlanMapping   m   = plan_mapping(plate, 512, 5.0);
    const CameraFrame   cam = plan_camera(m);
    for (const Vec2d& mm : {Vec2d(307.2, 0.), Vec2d(563.2, 256.), Vec2d(400., 100.), Vec2d(310., 250.)}) {
        Vec2d px;
        REQUIRE(project_point_to_pixel(cam, Vec3d(mm.x(), mm.y(), 0.), px));
        const Vec2d want = m.to_px(mm);
        CHECK_THAT(px.x(), WithinAbs(want.x(), 1e-6));
        CHECK_THAT(px.y(), WithinAbs(want.y(), 1e-6));
    }
}

TEST_CASE("footprint fallback makes one rectangle per object with its brim ring", "[FirstLayerPlan]")
{
    std::vector<FootprintInput> in;
    in.push_back({8, "Top Frame-SOLID-2", BoundingBoxf3(Vec3d(326., 36., 0.), Vec3d(543., 132., 96.)), 5.0});
    in.push_back({14, "Top Frame-SOLID-12", BoundingBoxf3(Vec3d(430., 4., 0.), Vec3d(509., 25., 12.)), 0.0});

    const FirstLayerPlan plan = plan_from_footprints(in);
    CHECK(plan.source == "footprints");
    REQUIRE(plan.objects.size() == 2);
    CHECK(plan.objects[0].object_index == 8);
    CHECK(plan.objects[0].name == "Top Frame-SOLID-2");
    REQUIRE(plan.objects[0].body.size() == 1);
    CHECK(plan.objects[0].body.front().contour.points.size() == 4);
    CHECK(plan.objects[0].brim.size() == 1);                // one ring, 5 mm out
    CHECK(plan.objects[1].brim.empty());                    // no brim, no ring
    CHECK(plan.objects[0].color == object_palette_color(8));

    // The brim ring sits outside the body by the brim extent.
    const BoundingBox body_bb = get_extents(plan.objects[0].body);
    const BoundingBox brim_bb = get_extents(plan.objects[0].brim);
    CHECK_THAT(unscale<double>(brim_bb.min.x()), WithinAbs(326. - 5., 1e-6));
    CHECK_THAT(unscale<double>(brim_bb.max.y()), WithinAbs(132. + 5., 1e-6));
    CHECK(brim_bb.contains(body_bb.min));
    CHECK(plan.support.empty());
    CHECK_FALSE(plan.wipe_tower.has_value());
}

TEST_CASE("plan labels sit at each body's centroid", "[FirstLayerPlan]")
{
    std::vector<FootprintInput> in;
    in.push_back({3, "cube", BoundingBoxf3(Vec3d(10., 20., 0.), Vec3d(30., 60., 10.)), 0.0});
    const auto labels = plan_labels(plan_from_footprints(in));
    REQUIRE(labels.size() == 1);
    CHECK(labels[0].text == "3");
    CHECK_THAT(labels[0].world_anchor.x(), WithinAbs(20., 1e-6));
    CHECK_THAT(labels[0].world_anchor.y(), WithinAbs(40., 1e-6));
    CHECK_THAT(labels[0].world_anchor.z(), WithinAbs(0., 1e-9));
}
