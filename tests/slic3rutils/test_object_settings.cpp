#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

// What reset_object_config clears with no keys given. It erased every key of the object's config,
// its "extruder" with them, so the object lost its filament. The GUI's reset (the object list's
// reset icon, TabPrintModel::reset_model_config) clears only the settings the object tab edits,
// which "extruder" is not; so does reset_object_config now.

using Slic3r::GUI::OrcaMCP::object_overrides_to_reset;

namespace {
// The object tab's settings, as TabPrintModel's constructor picks them for the object tab
// (Tab.cpp: intersect(Preset::print_options(), PrintObjectConfig + PrintRegionConfig keys)).
bool object_tab_edits(const std::string& key)
{
    std::vector<std::string> keys = Slic3r::PrintObjectConfig().keys();
    const std::vector<std::string> region = Slic3r::PrintRegionConfig().keys();
    keys.insert(keys.end(), region.begin(), region.end());
    const auto& print = Slic3r::Preset::print_options();
    return std::find(keys.begin(), keys.end(), key) != keys.end() && std::find(print.begin(), print.end(), key) != print.end();
}
} // namespace

TEST_CASE("resetting an object's settings keeps its filament, as the GUI's reset does", "[orcamcp][ObjectSettings]")
{
    Slic3r::DynamicPrintConfig overrides;
    overrides.set_deserialize_strict({{"extruder", "2"}, {"wall_loops", "4"}, {"layer_height", "0.12"}, {"brim_type", "no_brim"}});
    std::vector<std::string> reset = object_overrides_to_reset(overrides.keys(), object_tab_edits);
    std::sort(reset.begin(), reset.end());
    CHECK(reset == std::vector<std::string>{"brim_type", "layer_height", "wall_loops"});
}
