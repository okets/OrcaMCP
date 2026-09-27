#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"

using namespace Slic3r;

// convex_hull_2d does not clip geometry below the bed, so these cases avoid
// sinking transforms.
TEST_CASE("A part's 2D convex hull is its footprint projected onto the bed", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    // Keep the cube's raw coordinates ([0,20] on every axis): the default
    // add_volume re-centers the geometry, which would move the footprint.
    object->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);

    SECTION("identity transform yields the 20 mm square") {
        const Polygon hull   = object->convex_hull_2d(Geometry::Transformation{}.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(0.));
        CHECK(bb.min.y() == scaled(0.));
        CHECK(bb.max.x() == scaled(20.));
        CHECK(bb.max.y() == scaled(20.));
    }

    SECTION("scaling and offset move and grow the footprint") {
        Geometry::Transformation t;
        t.set_scaling_factor({2, 2, 2}); // cube now spans [0,40]
        t.set_offset({10, 5, 0});        // then shift +10 in X, +5 in Y

        const Polygon hull   = object->convex_hull_2d(t.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(10.));
        CHECK(bb.min.y() == scaled(5.));
        CHECK(bb.max.x() == scaled(50.));
        CHECK(bb.max.y() == scaled(45.));
    }
}

// Every layer range must carry a layer_height: the slicer reads it unchecked
// (layer_height_profile_from_ranges), and so do the GUI's object list and Print::apply's range
// comparison. The object list gives a new range one; a file can carry a range without it, and so
// did MCP's set_object_layer_range, and the next slice crashed.
namespace {
DynamicPrintConfig print_config_at(double layer_height)
{
    DynamicPrintConfig config;
    config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    return config;
}
} // namespace

TEST_CASE("A new layer range starts at the object's own layer height, else the print settings', on the object's extruder", "[Model][LayerRanges]")
{
    Model        model;
    ModelObject* object = model.add_object();
    const DynamicPrintConfig from_print = layer_range_defaults(*object, print_config_at(0.2));
    CHECK_THAT(from_print.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK(from_print.opt_int("extruder") == 0);

    object->config.set_key_value("layer_height", new ConfigOptionFloat(0.16));
    CHECK_THAT(layer_range_defaults(*object, print_config_at(0.2)).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.16, 1e-9));

    // Settings without a layer height (a file's that has none): the setting's own default.
    const double def = print_config_def.get("layer_height")->get_default_value<ConfigOptionFloat>()->value;
    object->config.erase("layer_height");
    CHECK_THAT(layer_range_defaults(*object, DynamicPrintConfig()).opt_float("layer_height"), Catch::Matchers::WithinAbs(def, 1e-9));
}

TEST_CASE("Completing a layer range adds what it lacks and keeps what it has", "[Model][LayerRanges]")
{
    ModelConfig range;
    range.set_key_value("sparse_infill_density", new ConfigOptionPercent(30));
    range.set_key_value("extruder", new ConfigOptionInt(2));
    DynamicPrintConfig defaults = print_config_at(0.24);
    defaults.set_key_value("extruder", new ConfigOptionInt(0));
    complete_layer_range(range, defaults);
    CHECK_THAT(range.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.24, 1e-9));
    CHECK(range.opt_int("extruder") == 2);
    CHECK(range.has("sparse_infill_density"));
}

TEST_CASE("A loaded model's layer ranges are completed from each object's own layer height", "[Model][LayerRanges]")
{
    Model        model;
    ModelObject* own    = model.add_object();
    ModelObject* global = model.add_object();
    own->config.set_key_value("layer_height", new ConfigOptionFloat(0.12));
    ModelConfig bare;
    bare.set_key_value("sparse_infill_density", new ConfigOptionPercent(30));
    own->layer_config_ranges[{2.0, 5.0}].assign_config(bare);
    global->layer_config_ranges[{1.0, 3.0}].assign_config(bare);
    ModelConfig set;
    set.set_key_value("layer_height", new ConfigOptionFloat(0.08));
    global->layer_config_ranges[{4.0, 6.0}].assign_config(set);

    complete_layer_ranges(model, print_config_at(0.28));
    CHECK_THAT(own->layer_config_ranges.at({2.0, 5.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.12, 1e-9));
    CHECK_THAT(global->layer_config_ranges.at({1.0, 3.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.28, 1e-9));
    CHECK_THAT(global->layer_config_ranges.at({4.0, 6.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.08, 1e-9));
    CHECK(global->layer_config_ranges.at({1.0, 3.0}).opt_int("extruder") == 0);
}
