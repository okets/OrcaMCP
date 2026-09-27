#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/Model.hpp"
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

// set_object_printable with the value every instance already has took an undo snapshot and marked the
// object's plates unsliced, and set_brim_ears with the ears the object already has did the same.
TEST_CASE("setting an object printable as it already is changes nothing", "[orcamcp][ObjectSettings]")
{
    Slic3r::Model        model;
    Slic3r::ModelObject* object = model.add_object();
    object->add_instance();
    object->add_instance();
    CHECK_FALSE(Slic3r::GUI::OrcaMCP::printable_changes(*object, true));
    CHECK(Slic3r::GUI::OrcaMCP::printable_changes(*object, false));
    object->instances[1]->printable = false;
    CHECK(Slic3r::GUI::OrcaMCP::printable_changes(*object, true));
    CHECK(Slic3r::GUI::OrcaMCP::printable_changes(*object, false));
}

TEST_CASE("brim ears change only when they are replaced by others or added to", "[orcamcp][ObjectSettings]")
{
    using Slic3r::BrimPoint;
    const std::vector<BrimPoint> current = {BrimPoint(1.f, 2.f, 0.f, 3.f), BrimPoint(4.f, 5.f, 0.f, 3.f)};
    using Slic3r::GUI::OrcaMCP::brim_ears_change;
    CHECK_FALSE(brim_ears_change(current, current, /*append=*/false));
    CHECK(brim_ears_change(current, {BrimPoint(1.f, 2.f, 0.f, 4.f)}, false));
    CHECK(brim_ears_change(current, {BrimPoint(9.f, 9.f, 0.f, 3.f)}, /*append=*/true));
    CHECK_FALSE(brim_ears_change(current, {}, /*append=*/true));
}

// A project write that leaves its values as they were (set_filament_color with the slot's own colour,
// set_flush_volumes with the matrix it has) marked every plate unsliced. The test is the values the call
// writes, copied before it writes them.
TEST_CASE("a config write changes something only when a value it writes differs", "[orcamcp][ObjectSettings]")
{
    using Slic3r::GUI::OrcaMCP::WrittenValues;
    Slic3r::DynamicPrintConfig config;
    config.set_deserialize_strict({{"filament_colour", "#FF0000;#00FF00"}, {"flush_multiplier", "1"}});

    const WrittenValues written(config, {"filament_colour", "flush_multiplier", "wipe_tower_x"});
    CHECK_FALSE(written.changed_in(config));

    Slic3r::DynamicPrintConfig same_colour = config;
    same_colour.set_deserialize_strict({{"filament_colour", "#FF0000;#00FF00"}});
    CHECK_FALSE(written.changed_in(same_colour));

    Slic3r::DynamicPrintConfig recoloured = config;
    recoloured.set_deserialize_strict({{"filament_colour", "#FF0000;#0000FF"}});
    CHECK(written.changed_in(recoloured));

    // A key the config gains counts as a change; one it never had and still lacks does not.
    Slic3r::DynamicPrintConfig gained = config;
    gained.set_deserialize_strict({{"wipe_tower_x", "10"}});
    CHECK(written.changed_in(gained));

    // Keys the call does not write are not compared.
    Slic3r::DynamicPrintConfig elsewhere = config;
    elsewhere.set_deserialize_strict({{"layer_height", "0.1"}});
    CHECK_FALSE(written.changed_in(elsewhere));
}
