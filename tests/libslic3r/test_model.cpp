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
// did MCP's set_object_layer_range, and the next slice crashed. A range is completed from the ACTIVE
// print settings the caller passes (the app's presets, the CLI's config), never from a guess.
namespace {
// Active settings: a layer height, and one nozzle per entry with its layer-height limits.
DynamicPrintConfig active_config(double layer_height, std::vector<double> nozzles = {0.4},
                                 std::vector<double> min_heights = {0.08}, std::vector<double> max_heights = {0.0})
{
    DynamicPrintConfig config;
    config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(nozzles));
    config.set_key_value("min_layer_height", new ConfigOptionFloats(min_heights));
    config.set_key_value("max_layer_height", new ConfigOptionFloats(max_heights));
    return config;
}
} // namespace

TEST_CASE("A new layer range starts at the object's layer height, else the active one, on the object's extruder", "[Model][LayerRanges]")
{
    Model        model;
    ModelObject* object = model.add_object();
    const DynamicPrintConfig from_active = layer_range_defaults(*object, active_config(0.2));
    CHECK_THAT(from_active.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK(from_active.opt_int("extruder") == 0);

    object->config.set_key_value("layer_height", new ConfigOptionFloat(0.16));
    CHECK_THAT(layer_range_defaults(*object, active_config(0.2)).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.16, 1e-9));
}

TEST_CASE("A new layer range's height is one the object's nozzle can print", "[Model][LayerRanges]")
{
    Model        model;
    ModelObject* object = model.add_object();
    // A 0.4 mm nozzle with no maximum prints up to 0.3 mm, and from its minimum up.
    CHECK_THAT(layer_range_defaults(*object, active_config(0.5)).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.3, 1e-9));
    CHECK_THAT(layer_range_defaults(*object, active_config(0.05)).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.08, 1e-9));

    // On a toolchanger, the nozzle of the object's own extruder.
    object->config.set_key_value("extruder", new ConfigOptionInt(2));
    const DynamicPrintConfig toolchanger = active_config(0.5, {0.4, 0.8}, {0.08, 0.2}, {0.0, 0.0});
    CHECK_THAT(layer_range_defaults(*object, toolchanger).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.5, 1e-9));
}

// A range can name its own extruder, and it prints at that extruder's nozzle: its default height is
// clamped to that nozzle, not the object's. On a toolchanger with a 0.4 mm T1 and a 0.8 mm T2, a
// range moved to T1 of an object printing at 0.5 mm on T2 kept 0.5 mm on the 0.4 mm nozzle.
TEST_CASE("A layer range is completed within the nozzle of the extruder that prints it", "[Model][LayerRanges]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->config.set_key_value("layer_height", new ConfigOptionFloat(0.5));
    const DynamicPrintConfig toolchanger = active_config(0.2, {0.4, 0.8}, {0.08, 0.2}, {0.0, 0.0});

    ModelConfig on_t1;
    on_t1.set_key_value("extruder", new ConfigOptionInt(1));
    complete_layer_range(on_t1, *object, toolchanger);
    CHECK_THAT(on_t1.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.3, 1e-9));
    CHECK(on_t1.opt_int("extruder") == 1);

    // Extruder 0, or none, is the object's.
    ModelConfig on_object;
    on_object.set_key_value("sparse_infill_density", new ConfigOptionPercent(30));
    complete_layer_range(on_object, *object, toolchanger);
    CHECK_THAT(on_object.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.5, 1e-9));
    CHECK(on_object.opt_int("extruder") == 0);

    // A model's ranges each by their own extruder.
    object->layer_config_ranges[{1.0, 2.0}].assign_config(on_t1.get());
    ModelConfig bare_t1;
    bare_t1.set_key_value("extruder", new ConfigOptionInt(1));
    object->layer_config_ranges[{3.0, 4.0}].assign_config(bare_t1);
    complete_layer_ranges(model, toolchanger);
    CHECK_THAT(object->layer_config_ranges.at({3.0, 4.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.3, 1e-9));
}

TEST_CASE("Completing a layer range adds what it lacks and keeps what it has", "[Model][LayerRanges]")
{
    ModelConfig range;
    range.set_key_value("sparse_infill_density", new ConfigOptionPercent(30));
    range.set_key_value("extruder", new ConfigOptionInt(2));
    DynamicPrintConfig defaults;
    defaults.set_key_value("layer_height", new ConfigOptionFloat(0.24));
    defaults.set_key_value("extruder", new ConfigOptionInt(0));
    complete_layer_range(range, defaults);
    CHECK_THAT(range.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.24, 1e-9));
    CHECK(range.opt_int("extruder") == 2);
    CHECK(range.has("sparse_infill_density"));
}

TEST_CASE("A model's layer ranges are completed from each object's layer height and the active one", "[Model][LayerRanges]")
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

    complete_layer_ranges(model, active_config(0.28));
    CHECK_THAT(own->layer_config_ranges.at({2.0, 5.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.12, 1e-9));
    CHECK_THAT(global->layer_config_ranges.at({1.0, 3.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.28, 1e-9));
    CHECK_THAT(global->layer_config_ranges.at({4.0, 6.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.08, 1e-9));
    CHECK(global->layer_config_ranges.at({1.0, 3.0}).opt_int("extruder") == 0);
}
