#include <catch2/catch_all.hpp>

#include <algorithm>
#include <limits>

#include "slic3r/GUI/OrcaMCP/OrcaMCPFirstLayerPlan.hpp"
#include "fff_print/test_helpers.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "mcp_slice_fixtures.hpp"

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

// The plan of a sliced plate shows what the printer lays down first. With a raft that is the raft's
// base, and the object's own first layer prints several layers up, on top of it: drawing the
// object's footprint there showed a part standing on the bed that never touches it.

TEST_CASE("a print on a raft shows the raft as its first layer, not the object above it", "[FirstLayerPlan]")
{
    Print print;
    Model model;
    Test::init_print({Test::cube(10)}, print, model, {
        {"raft_layers", 2},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
    });
    print.process();
    REQUIRE(print.objects().front()->support_layer_count() >= 2);  // the raft's layers

    const FirstLayerPlan plan = plan_from_print(print, model);
    REQUIRE(plan.objects.size() == 1);
    const PlanObject& cube = plan.objects.front();
    CHECK(cube.object_index == 0);
    CHECK(cube.body.empty());
    REQUIRE_FALSE(cube.raft.empty());
    CHECK(plan.support.empty());  // the raft is the cube's own, drawn with it

    // The raft spreads at least as far as the 10 mm cube it carries.
    const BoundingBox raft = get_extents(cube.raft);
    CHECK(unscale<double>(raft.size().x()) >= 10.0 - 1e-3);
    CHECK(unscale<double>(raft.size().y()) >= 10.0 - 1e-3);

    // The label and the object's place in the frame come from the raft, not from nothing.
    const auto labels = plan_labels(plan);
    REQUIRE(labels.size() == 1);
    CHECK(labels[0].text == "0");
}

TEST_CASE("without a raft the first layer is the object's own", "[FirstLayerPlan]")
{
    Print print;
    Model model;
    Test::init_print({Test::cube(10)}, print, model, {
        {"raft_layers", 0},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
    });
    print.process();

    const FirstLayerPlan plan = plan_from_print(print, model);
    REQUIRE(plan.objects.size() == 1);
    CHECK(plan.source == "sliced");
    CHECK_FALSE(plan.objects[0].body.empty());
    CHECK(plan.objects[0].raft.empty());
    const BoundingBox body = get_extents(plan.objects[0].body);
    CHECK_THAT(unscale<double>(body.size().x()), WithinAbs(10.0, 0.5));
}

TEST_CASE("support that starts above the bed is not drawn on the first layer", "[FirstLayerPlan]")
{
    // The support under the shelf stands on the base's top at z 2, so nothing of it prints first.
    Print print;
    Model model;
    Test::init_print({mcp_test::shelf_over_base()}, print, model, {
        {"enable_support", 1},
        {"support_on_build_plate_only", 0},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
    });
    print.process();
    // The support's lowest extrusions are on the base, above the first layer.
    const PrintObject& object     = *print.objects().front();
    double             lowest_fill = std::numeric_limits<double>::max();
    for (const SupportLayer* layer : object.support_layers())
        if (!layer->support_fills.empty())
            lowest_fill = std::min(lowest_fill, double(layer->print_z));
    REQUIRE(lowest_fill > 2.0);

    const FirstLayerPlan plan = plan_from_print(print, model);
    CHECK(plan.support.empty());
    REQUIRE(plan.objects.size() == 1);
    CHECK_FALSE(plan.objects[0].body.empty());
}
