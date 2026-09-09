#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.hpp"

#include <cmath>
#include <map>
#include <vector>

// get_print_estimate's arithmetic: the volume the slicer reports per extruder turned into the
// length, weight and cost an agent asks for. Mirrors DoExport::update_print_estimated_stats, which
// is what the G-code's own "total filament used" comments are computed from - the numbers here are
// the ones a caller compares against those comments.

using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinRel;

namespace {

// 1.75 mm filament, PETG-ish density and price.
const std::vector<float> k_diameters = {1.75f, 1.75f};
const std::vector<float> k_densities = {1.27f, 1.24f};
const std::vector<float> k_costs     = {24.99f, 19.99f};

double expected_length_mm(double volume_mm3, double diameter_mm)
{
    return volume_mm3 / (M_PI * (diameter_mm * 0.5) * (diameter_mm * 0.5));
}

} // namespace

TEST_CASE("compute_slice_estimate matches the G-code's own filament numbers", "[orcamcp][estimate]")
{
    // 13.71 g of PETG at 1.27 g/cm3 is 10795 mm3, the shape of the print that exposed this bug.
    const std::map<size_t, double> volumes = {{0, 10795.3}};

    const SliceEstimate estimate = compute_slice_estimate(volumes, k_diameters, k_densities, k_costs);

    REQUIRE(estimate.per_filament.size() == 1);
    CHECK(estimate.per_filament[0].filament_id == 0);
    CHECK_THAT(estimate.volume_mm3, WithinRel(10795.3, 1e-9));
    REQUIRE(estimate.weight_g.has_value());
    CHECK_THAT(*estimate.weight_g, WithinRel(13.71, 1e-3));
    REQUIRE(estimate.length_mm.has_value());
    CHECK_THAT(*estimate.length_mm, WithinRel(expected_length_mm(10795.3, 1.75), 1e-9));
    REQUIRE(estimate.cost.has_value());
    CHECK_THAT(*estimate.cost, WithinRel(13.71 * 24.99 * 0.001, 1e-3));
}

TEST_CASE("compute_slice_estimate totals every filament it was given", "[orcamcp][estimate]")
{
    const std::map<size_t, double> volumes = {{0, 1000.0}, {1, 2500.0}};

    const SliceEstimate estimate = compute_slice_estimate(volumes, k_diameters, k_densities, k_costs);

    REQUIRE(estimate.per_filament.size() == 2);
    CHECK(estimate.per_filament[1].filament_id == 1);
    CHECK_THAT(estimate.volume_mm3, WithinRel(3500.0, 1e-9));
    REQUIRE(estimate.weight_g.has_value());
    CHECK_THAT(*estimate.weight_g, WithinRel(1000.0 * 1.27 * 0.001 + 2500.0 * 1.24 * 0.001, 1e-6));
    REQUIRE(estimate.per_filament[0].weight_g.has_value());
    CHECK_THAT(*estimate.per_filament[0].weight_g, WithinRel(1.27, 1e-6));
}

TEST_CASE("compute_slice_estimate reports unknown properties as unknown, not zero", "[orcamcp][estimate]")
{
    const std::map<size_t, double> volumes = {{0, 1000.0}, {1, 2000.0}};

    SECTION("a filament past the end of the property vectors")
    {
        const SliceEstimate estimate = compute_slice_estimate(volumes, {1.75f}, {1.27f}, {24.99f});

        REQUIRE(estimate.per_filament.size() == 2);
        CHECK(estimate.per_filament[0].weight_g.has_value());
        CHECK_FALSE(estimate.per_filament[1].weight_g.has_value());
        CHECK_FALSE(estimate.per_filament[1].length_mm.has_value());
        // One unknown filament makes the totals unknown rather than short.
        CHECK_FALSE(estimate.weight_g.has_value());
        CHECK_FALSE(estimate.length_mm.has_value());
        CHECK_FALSE(estimate.cost.has_value());
        // The volume is always known: it comes from the result itself.
        CHECK_THAT(estimate.volume_mm3, WithinRel(3000.0, 1e-9));
    }

    SECTION("a zero or nonsense diameter/density")
    {
        const SliceEstimate estimate =
            compute_slice_estimate(volumes, {0.0f, 1.75f}, {-1.0f, 1.24f}, {24.99f, 19.99f});

        CHECK_FALSE(estimate.per_filament[0].length_mm.has_value());
        CHECK_FALSE(estimate.per_filament[0].weight_g.has_value());
        CHECK_FALSE(estimate.per_filament[0].cost.has_value());
        CHECK(estimate.per_filament[1].weight_g.has_value());
        CHECK_FALSE(estimate.weight_g.has_value());
    }

    SECTION("a known density but no cost still gives a weight")
    {
        const SliceEstimate estimate = compute_slice_estimate({{0, 1000.0}}, k_diameters, k_densities, {});

        REQUIRE(estimate.per_filament.size() == 1);
        CHECK(estimate.per_filament[0].weight_g.has_value());
        CHECK_FALSE(estimate.per_filament[0].cost.has_value());
        CHECK(estimate.weight_g.has_value());
        CHECK_FALSE(estimate.cost.has_value());
    }
}

TEST_CASE("compute_slice_estimate on an empty result", "[orcamcp][estimate]")
{
    const SliceEstimate estimate = compute_slice_estimate({}, k_diameters, k_densities, k_costs);

    CHECK(estimate.per_filament.empty());
    CHECK(estimate.volume_mm3 == 0.0);
    REQUIRE(estimate.weight_g.has_value());
    CHECK(*estimate.weight_g == 0.0);
}
