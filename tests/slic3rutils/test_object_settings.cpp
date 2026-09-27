#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "libslic3r/PrintConfig.hpp"

// What reset_object_config clears with no keys given. It erased every key of the object's config,
// its "extruder" with them, so the object lost its filament; the GUI's reset keeps the object's
// filament. Every other override goes, whether or not a settings tab shows it.

using Slic3r::GUI::OrcaMCP::object_overrides_to_reset;

TEST_CASE("resetting an object's settings clears every override but its filament", "[orcamcp][ObjectSettings]")
{
    Slic3r::DynamicPrintConfig overrides;
    // A feature's filament (sparse_infill_filament_id) is a setting like any other: only the object's own
    // filament assignment stays.
    overrides.set_deserialize_strict({{"extruder", "2"}, {"wall_loops", "4"}, {"layer_height", "0.12"}, {"brim_type", "no_brim"},
                                      {"sparse_infill_filament_id", "3"}});
    std::vector<std::string> reset = object_overrides_to_reset(overrides.keys());
    std::sort(reset.begin(), reset.end());
    CHECK(reset == std::vector<std::string>{"brim_type", "layer_height", "sparse_infill_filament_id", "wall_loops"});
    CHECK(object_overrides_to_reset({"extruder"}).empty());
    CHECK(object_overrides_to_reset({}).empty());
}

// A tool call's undo snapshot: one per call, right before its first change, and none for a call that
// changes nothing. apply_adaptive_layer_height took one per object (N undo steps for one call), and
// every per-object tool took one before knowing whether anything would change, so a no-op call threw
// away the redo stack.
TEST_CASE("a tool call takes one snapshot before its first change, and none when it changes nothing", "[orcamcp][ObjectSettings]")
{
    int taken = 0;
    {
        Slic3r::GUI::OrcaMCP::SnapshotOnce snapshot([&taken] { ++taken; });
        CHECK_FALSE(snapshot.taken());
    }
    CHECK(taken == 0);

    Slic3r::GUI::OrcaMCP::SnapshotOnce snapshot([&taken] { ++taken; });
    snapshot.before_change();
    snapshot.before_change();
    snapshot.before_change();
    CHECK(taken == 1);
    CHECK(snapshot.taken());
}
