#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPArrangeOptions.hpp"

#include <nlohmann/json.hpp>

#include <string>

// The arrange menu's options as arrange_objects and fill_bed_with_instances take them: read from a
// call, checked as the menu would, and written under the keys the menu writes and the app reads back
// (GLCanvas3D::_render_arrange_menu, GLCanvas3D::load_arrange_settings).

using Catch::Matchers::WithinAbs;
using namespace Slic3r::GUI::OrcaMCP;
using json = nlohmann::json;

namespace {

ArrangeOptions read_and_apply(const json& params, ArrangeOptions current, bool avoid_region_offered = false,
                              const ArrangeOptions& defaults = ArrangeOptions{})
{
    std::string error;
    const auto  request = read_arrange_options(params, error);
    REQUIRE(request);
    REQUIRE(error.empty());
    REQUIRE_FALSE(apply_arrange_options(*request, defaults, avoid_region_offered, current));
    return current;
}

std::string refusal_of(const json& params, ArrangeOptions current, bool avoid_region_offered = false)
{
    std::string error;
    const auto  request = read_arrange_options(params, error);
    if (!request)
        return error;
    const auto refusal = apply_arrange_options(*request, ArrangeOptions{}, avoid_region_offered, current);
    return refusal ? *refusal : std::string();
}

} // namespace

TEST_CASE("The option keys are the menu's: spacing and auto-rotate per print sequence, the rest shared", "[ArrangeOptions][orcamcp]")
{
    const ArrangeOptionKeys by_layer  = arrange_option_keys(ArrangeMode::by_layer);
    const ArrangeOptionKeys by_object = arrange_option_keys(ArrangeMode::by_object);
    const ArrangeOptionKeys sla       = arrange_option_keys(ArrangeMode::sla);
    CHECK(by_layer.spacing == "min_object_distance_fff");
    CHECK(by_layer.auto_rotate == "enable_rotation_fff");
    CHECK(by_object.spacing == "min_object_distance_fff_seq_print");
    CHECK(by_object.auto_rotate == "enable_rotation_fff_seq_print");
    CHECK(sla.spacing == "min_object_distance_sla");
    CHECK(by_object.allow_multiple_materials == "allow_multi_materials_on_same_plate");
    CHECK(by_layer.allow_multiple_materials == by_object.allow_multiple_materials);
    CHECK(by_layer.avoid_calibration_region == "avoid_extrusion_cali_region");
}

TEST_CASE("A call without options asks nothing, and one with any does", "[ArrangeOptions][orcamcp]")
{
    std::string error;
    CHECK_FALSE(read_arrange_options(json::object(), error)->any());
    CHECK(read_arrange_options({{"spacing_mm", 0}}, error)->any());
    CHECK(read_arrange_options({{"reset_options", true}}, error)->any());
    CHECK(error.empty());
}

TEST_CASE("Only the options a call gives change", "[ArrangeOptions][orcamcp]")
{
    ArrangeOptions current;
    current.allow_multiple_materials = false;
    const ArrangeOptions after = read_and_apply({{"spacing_mm", 5.5}}, current);
    CHECK_THAT(after.spacing_mm, WithinAbs(5.5, 1e-9));
    CHECK_FALSE(after.allow_multiple_materials);
    CHECK(changed_arrange_options(current, after) == std::vector<std::string>{"spacing_mm"});
}

TEST_CASE("Reset gives the defaults before the options the call gives", "[ArrangeOptions][orcamcp]")
{
    ArrangeOptions current;
    current.spacing_mm               = 12.;
    current.allow_multiple_materials = false;
    ArrangeOptions defaults;
    defaults.align_to_y_axis = true; // an i3 printer's default
    const ArrangeOptions after = read_and_apply({{"reset_options", true}, {"auto_rotate", false}}, current, false, defaults);
    CHECK_THAT(after.spacing_mm, WithinAbs(0., 1e-9));
    CHECK(after.allow_multiple_materials);
    CHECK(after.align_to_y_axis);
}

TEST_CASE("Turning auto-rotate on turns align to Y off, as the menu does", "[ArrangeOptions][orcamcp]")
{
    ArrangeOptions current;
    current.align_to_y_axis = true;
    const ArrangeOptions after = read_and_apply({{"auto_rotate", true}}, current);
    CHECK(after.auto_rotate);
    CHECK_FALSE(after.align_to_y_axis);
}

TEST_CASE("Align to Y is refused while auto-rotate is on, and nothing changes", "[ArrangeOptions][orcamcp]")
{
    ArrangeOptions current;
    current.auto_rotate = true;
    CHECK(refusal_of({{"align_to_y_axis", true}}, current).find("auto_rotate: false") != std::string::npos);
    CHECK(refusal_of({{"align_to_y_axis", true}, {"auto_rotate", true}, {"spacing_mm", 3}}, ArrangeOptions{}).find("auto_rotate") !=
          std::string::npos);
    CHECK(refusal_of({{"align_to_y_axis", true}, {"auto_rotate", false}}, current).empty());
}

TEST_CASE("A negative spacing is refused", "[ArrangeOptions][orcamcp]")
{
    CHECK(refusal_of({{"spacing_mm", -1}}, ArrangeOptions{}).find("0 or more") != std::string::npos);
}

TEST_CASE("The calibration-area option is refused where the menu does not offer it", "[ArrangeOptions][orcamcp]")
{
    CHECK(refusal_of({{"avoid_calibration_region", false}}, ArrangeOptions{}, false).find("scans its first layer") != std::string::npos);
    CHECK(refusal_of({{"avoid_calibration_region", false}}, ArrangeOptions{}, true).empty());
}

TEST_CASE("An option of the wrong kind is refused, never read as left out", "[ArrangeOptions][orcamcp]")
{
    const auto bad = GENERATE(json{{"spacing_mm", "wide"}}, json{{"auto_rotate", nullptr}}, json{{"align_to_y_axis", 1.5}},
                              json{{"reset_options", "yes"}});
    std::string error;
    CHECK_FALSE(read_arrange_options(bad, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("The options are reported in the arguments' names, with the print sequence whose set they are", "[ArrangeOptions][orcamcp]")
{
    ArrangeOptions options;
    options.spacing_mm = 2.;
    const json reported = arrange_options_json(options, ArrangeMode::by_object, false);
    CHECK_THAT(reported["spacing_mm"].get<double>(), WithinAbs(2., 1e-9));
    CHECK(reported["for_print_sequence"] == "by object");
    CHECK_FALSE(reported.contains("avoid_calibration_region"));
    CHECK(arrange_options_json(options, ArrangeMode::by_layer, true).contains("avoid_calibration_region"));
    // What the answer reports is what the arguments take: every option name is a declared argument.
    const json properties = arrange_option_properties();
    for (const auto& [key, value] : reported.items())
        if (key != "for_print_sequence")
            CHECK(properties.contains(key));
}
