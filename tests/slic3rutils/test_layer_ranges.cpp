#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPLayerRanges.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"

// A layer range as set_object_layer_range writes it. It wrote only the settings it was given, so a
// range with just a sparse_infill_density had no layer_height, and the next slice dereferenced the
// missing option (layer_height_profile_from_ranges) and crashed the app. The GUI's object list
// gives every range a layer_height and an extruder when it creates one
// (ObjectList::get_default_layer_config); so does set_object_layer_range now.

using Slic3r::DynamicPrintConfig;
using Slic3r::ModelConfig;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

namespace {

// What ObjectList::get_default_layer_config gives an object printing at 0.24 mm.
DynamicPrintConfig gui_defaults(double layer_height)
{
    DynamicPrintConfig defaults;
    defaults.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(layer_height));
    defaults.set_key_value("extruder", new Slic3r::ConfigOptionInt(0));
    return defaults;
}

// A printer with one extruder, a 0.4 mm nozzle and the given layer limits.
DynamicPrintConfig printer(double min_layer_height, double max_layer_height)
{
    DynamicPrintConfig config;
    config.set_key_value("nozzle_diameter", new Slic3r::ConfigOptionFloats({0.4}));
    config.set_key_value("min_layer_height", new Slic3r::ConfigOptionFloats({min_layer_height}));
    config.set_key_value("max_layer_height", new Slic3r::ConfigOptionFloats({max_layer_height}));
    return config;
}

} // namespace

TEST_CASE("a range given only other settings takes the object's layer height and extruder", "[orcamcp][LayerRanges]")
{
    ModelConfig range;
    range.set_key_value("sparse_infill_density", new Slic3r::ConfigOptionPercent(30));
    complete_layer_range(range, gui_defaults(0.24));
    REQUIRE(range.has("layer_height"));
    CHECK_THAT(range.opt_float("layer_height"), WithinAbs(0.24, 1e-9));
    CHECK(range.opt_int("extruder") == 0);
    CHECK(range.has("sparse_infill_density"));
}

TEST_CASE("a new range starts at the object's own layer height, else the process preset's", "[orcamcp][LayerRanges]")
{
    Slic3r::Model        model;
    Slic3r::ModelObject* object = model.add_object();
    DynamicPrintConfig   preset;
    preset.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(0.2));

    // No extruder in the object's config either: reset_object_config leaves an object so.
    DynamicPrintConfig from_preset = default_layer_config(*object, preset);
    CHECK_THAT(from_preset.opt_float("layer_height"), WithinAbs(0.2, 1e-9));
    CHECK(from_preset.opt_int("extruder") == 0);

    object->config.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(0.16));
    CHECK_THAT(default_layer_config(*object, preset).opt_float("layer_height"), WithinAbs(0.16, 1e-9));
}

TEST_CASE("a range's own layer height and extruder are kept", "[orcamcp][LayerRanges]")
{
    ModelConfig range;
    range.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(0.12));
    range.set_key_value("extruder", new Slic3r::ConfigOptionInt(2));
    complete_layer_range(range, gui_defaults(0.24));
    CHECK_THAT(range.opt_float("layer_height"), WithinAbs(0.12, 1e-9));
    CHECK(range.opt_int("extruder") == 2);
}

TEST_CASE("the layer height profile of a completed range prints it at the object's layer height", "[orcamcp][LayerRanges]")
{
    ModelConfig range;
    range.set_key_value("sparse_infill_density", new Slic3r::ConfigOptionPercent(30));
    complete_layer_range(range, gui_defaults(0.24));
    Slic3r::t_layer_config_ranges ranges;
    ranges[{2.0, 5.0}].assign_config(range);

    Slic3r::SlicingParameters params;
    params.layer_height              = 0.24;
    params.first_object_layer_height = 0.2;
    params.object_print_z_min        = 0.;
    params.object_print_z_max        = 10.;
    const std::vector<double> profile = Slic3r::layer_height_profile_from_ranges(params, ranges);
    // Pairs of (z, height): the heights between 2 and 5 mm are the range's, here the object's.
    bool saw_range = false;
    for (size_t i = 0; i + 1 < profile.size(); i += 2)
        if (profile[i] >= 2.0 - 1e-9 && profile[i] <= 5.0 + 1e-9) {
            saw_range = true;
            CHECK_THAT(profile[i + 1], WithinAbs(0.24, 1e-9));
        }
    CHECK(saw_range);
}

TEST_CASE("a range's layer height must be one the printer's extruder can print", "[orcamcp][LayerRanges]")
{
    const LayerHeightLimits limits = layer_height_limits(printer(0.08, 0.28), /*extruder=*/0);
    CHECK_THAT(limits.min, WithinAbs(0.08, 1e-9));
    CHECK_THAT(limits.max, WithinAbs(0.28, 1e-9));

    CHECK_FALSE(layer_range_height_error(0.2, limits).has_value());
    CHECK_FALSE(layer_range_height_error(0.08, limits).has_value());
    CHECK_FALSE(layer_range_height_error(0.28, limits).has_value());
    CHECK(layer_range_height_error(0.3, limits).has_value());
    CHECK(layer_range_height_error(0.05, limits).has_value());
    CHECK(layer_range_height_error(0.0, limits).has_value());
    CHECK(layer_range_height_error(-0.1, limits).has_value());
    CHECK(layer_range_height_error(0.3, limits)->find("0.08") != std::string::npos);
    CHECK(layer_range_height_error(0.3, limits)->find("0.28") != std::string::npos);
}

TEST_CASE("a printer with no maximum layer height allows three quarters of its nozzle, as the GUI does", "[orcamcp][LayerRanges]")
{
    const LayerHeightLimits limits = layer_height_limits(printer(0.0, 0.0), /*extruder=*/1);
    CHECK_THAT(limits.max, WithinAbs(0.3, 1e-9));
    CHECK(layer_range_height_error(0.31, limits).has_value());
    // No minimum: any height above 0 is allowed from below.
    CHECK_FALSE(layer_range_height_error(0.01, limits).has_value());
}
