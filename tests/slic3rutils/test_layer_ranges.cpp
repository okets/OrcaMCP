#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPLayerRanges.hpp"
#include "libslic3r/PrintConfig.hpp"

// The layer heights set_object_layer_range accepts for a range: the ones the printer's extruder can
// print, as the object list's range editor bounds them (ObjectList::edit_layer_range). A range's
// defaults and their completion are Model's (tests/libslic3r/test_model.cpp, [LayerRanges]).

using Slic3r::DynamicPrintConfig;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

namespace {

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
