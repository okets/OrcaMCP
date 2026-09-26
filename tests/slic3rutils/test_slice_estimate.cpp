#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPSliceEstimate.hpp"
#include "fff_print/test_helpers.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"

#include <cmath>
#include <map>
#include <optional>
#include <regex>
#include <string>
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

// get_print_estimate's layer counts. It used to report the tallest object's total_layer_count(),
// which is layer_count() + support_layer_count() -- two counts of mostly the same heights added
// together (Print.hpp says as much: "not supposed to be compared"), so a 99 mm figurine at 0.12 mm
// came back as 1567 layers instead of about 825. The G-code counts distinct print heights instead.

TEST_CASE("count_distinct_heights merges heights closer than EPSILON", "[orcamcp][estimate]")
{
    CHECK(count_distinct_heights({}) == 0);
    CHECK(count_distinct_heights({0.2, 0.4, 0.6}) == 3);
    CHECK(count_distinct_heights({0.6, 0.2, 0.4, 0.2, 0.4}) == 3);        // duplicates, any order
    CHECK(count_distinct_heights({0.2, 0.2 + EPSILON / 10., 0.4}) == 2); // numerically equal
    CHECK(count_distinct_heights({0.2, 0.2 + 3 * EPSILON, 0.4}) == 3);   // genuinely different
}

namespace {

// A 30 x 30 mm cap on an 8 x 8 mm stem, 12 mm tall: the cap's underside needs support.
Slic3r::TriangleMesh supported_cap()
{
    Slic3r::TriangleMesh model = Slic3r::make_cube(8, 8, 10);
    model.translate(11, 11, 0);
    Slic3r::TriangleMesh cap = Slic3r::make_cube(30, 30, 2);
    cap.translate(0, 0, 10);
    model.merge(cap);
    return model;
}

// The count the G-code itself states in its header, "; total layers count = N".
int gcode_layer_count(const std::string& gcode)
{
    std::smatch match;
    static const std::regex k_line("; total layers count = ([0-9]+)");
    return std::regex_search(gcode, match, k_line) ? std::stoi(match[1]) : -1;
}

} // namespace

TEST_CASE("printed layers are the G-code's layer count, not object plus support", "[orcamcp][estimate]")
{
    const bool independent = GENERATE(false, true);
    DYNAMIC_SECTION("independent_support_layer_height " << independent)
    {
        // 12 mm at 0.2 mm, first layer 0.2 mm: 60 object layers.
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({supported_cap()}, print, model, {
            {"enable_support", 1},
            {"layer_height", 0.2},
            {"initial_layer_print_height", 0.2},
            {"independent_support_layer_height", independent ? 1 : 0},
        });
        const std::string gcode = Slic3r::Test::gcode(print);
        REQUIRE(print.objects().front()->support_layer_count() > 0);

        const LayerCounts counts = count_print_layers(print);
        CHECK(counts.object == 60);
        CHECK(counts.support > 0);
        CHECK(int(counts.printed) == gcode_layer_count(gcode));
        // Distinct heights: never fewer than the object's own, never the doubled sum.
        CHECK(counts.printed >= counts.object);
        CHECK(counts.printed < counts.object + counts.support);
        if (!independent)
            CHECK(counts.printed == counts.object);  // support shares the object's heights
    }
}

TEST_CASE("printed layers of a by-object print add up every object and instance", "[orcamcp][estimate]")
{
    // Two 5 mm cubes at 0.25 mm, printed one after the other: 20 layers each, 40 in the G-code.
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({Slic3r::Test::cube(5), Slic3r::Test::cube(5)}, print, model, {
        {"print_sequence", "by object"},
        {"layer_height", 0.25},
        {"initial_layer_print_height", 0.25},
    });
    const std::string gcode = Slic3r::Test::gcode(print);

    const LayerCounts counts = count_print_layers(print);
    CHECK(counts.object == 40);
    CHECK(counts.support == 0);
    CHECK(counts.printed == 40);
    CHECK(int(counts.printed) == gcode_layer_count(gcode));
}

// apply_adaptive_layer_height's estimated_layer_count: the layers the new profile is cut into.
// generate_object_layers asserts a non-empty profile, and with precise Z reads the last of its
// layers, so a profile with nothing to cut must be caught before it gets there.

namespace {

Slic3r::SlicingParameters cube_slicing_parameters(Slic3r::Model& model, double size)
{
    Slic3r::ModelObject* object = model.add_object();
    object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(size, size, size)));
    object->add_instance();
    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2}});
    return Slic3r::PrintObject::slicing_parameters(config, *object, float(size), Slic3r::Vec3d::Ones());
}

} // namespace

TEST_CASE("an adaptive profile's layers are counted as the slicer cuts them", "[orcamcp][estimate]")
{
    // 10 mm at a constant 0.2 mm, first layer 0.2 mm: 50 layers, with or without precise Z.
    Slic3r::Model model;
    const Slic3r::SlicingParameters params = cube_slicing_parameters(model, 10.0);
    const std::vector<double> constant = {0.0, 0.2, 10.0, 0.2};
    CHECK(count_profile_layers(params, constant, false) == std::optional<size_t>(50));
    CHECK(count_profile_layers(params, constant, true) == std::optional<size_t>(50));
}

TEST_CASE("an adaptive profile with nothing to cut has no layer count", "[orcamcp][estimate]")
{
    Slic3r::Model model;
    const Slic3r::SlicingParameters params = cube_slicing_parameters(model, 10.0);
    CHECK_FALSE(count_profile_layers(params, {}, false).has_value());
    CHECK_FALSE(count_profile_layers(params, {}, true).has_value());
    CHECK_FALSE(count_profile_layers(params, {0.0, 0.2}, true).has_value());
}
