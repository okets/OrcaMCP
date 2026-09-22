#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPFilamentModel.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// The shape every object in the user's project had on 2026-09-22: one printed part that
// inherits, plus modifiers pinned to their own slot. The pinned modifiers are what kept the
// prime tower alive after the object-level slot had been changed.
struct Fixture
{
    Model        model;
    ModelObject* object = nullptr;
    ModelVolume* part   = nullptr;

    Fixture()
    {
        object = model.add_object();
        object->name = "Base";
        object->config.set_key_value("extruder", new ConfigOptionInt(3));
        part       = object->add_volume(TriangleMesh(its_make_cube(10.0, 10.0, 10.0)), false);
        part->name = "Base";
    }

    ModelVolume* add(ModelVolumeType type, const char* name, int own_filament)
    {
        ModelVolume* v = object->add_volume(TriangleMesh(its_make_cube(2.0, 2.0, 2.0)), false);
        v->set_type(type);
        v->name = name;
        if (own_filament != 0)
            v->config.set_key_value("extruder", new ConfigOptionInt(own_filament));
        return v;
    }
};

} // namespace

TEST_CASE("describe_volume_filaments reports own and effective slot per volume", "[FilamentModel]")
{
    Fixture f;
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Base Modifier", 1);
    f.add(ModelVolumeType::NEGATIVE_VOLUME, "Hole", 0);

    const std::vector<VolumeFilament> v = describe_volume_filaments(*f.object);
    REQUIRE(v.size() == 3);

    CHECK(v[0].volume_id == 0);
    CHECK(v[0].name == "Base");
    CHECK(v[0].type == "part");
    CHECK(v[0].own_filament == 0);        // inherits
    CHECK(v[0].effective_filament == 3);  // from the object

    CHECK(v[1].volume_id == 1);
    CHECK(v[1].type == "modifier");
    CHECK(v[1].own_filament == 1);
    CHECK(v[1].effective_filament == 1);  // the pin wins over the object's 3

    CHECK(v[2].type == "negative_volume");
}

TEST_CASE("effective_object_filaments unions parts, modifiers and layer ranges", "[FilamentModel]")
{
    Fixture f;
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Base Modifier", 1);
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Wheel Modifier", 1);
    f.object->layer_config_ranges[{1.4, 4.0}].set_key_value("extruder", new ConfigOptionInt(2));

    CHECK(effective_object_filaments(*f.object) == std::vector<int>{1, 2, 3});
}

TEST_CASE("effective_object_filaments ignores slot 0 ranges and unprintable volumes", "[FilamentModel]")
{
    Fixture f;
    f.add(ModelVolumeType::NEGATIVE_VOLUME, "Hole", 1);
    f.add(ModelVolumeType::SUPPORT_BLOCKER, "Blocker", 4);
    f.object->layer_config_ranges[{1.4, 4.0}].set_key_value("extruder", new ConfigOptionInt(0));

    CHECK(effective_object_filaments(*f.object) == std::vector<int>{3});
}

TEST_CASE("clear_volume_filament_overrides erases parts and modifiers and reports each", "[FilamentModel]")
{
    Fixture f;
    f.part->config.set_key_value("extruder", new ConfigOptionInt(1));
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Base Modifier", 1);
    f.add(ModelVolumeType::NEGATIVE_VOLUME, "Hole", 1);

    const std::vector<ClearedOverride> cleared = clear_volume_filament_overrides(*f.object, /*include_modifiers=*/true);

    REQUIRE(cleared.size() == 2);
    CHECK(cleared[0].volume_id == 0);
    CHECK(cleared[0].type == "part");
    CHECK(cleared[0].was_filament == 1);
    CHECK(cleared[1].volume_id == 1);
    CHECK(cleared[1].name == "Base Modifier");
    CHECK(cleared[1].was_filament == 1);

    CHECK_FALSE(f.object->volumes[0]->config.has("extruder"));
    CHECK_FALSE(f.object->volumes[1]->config.has("extruder"));
    // A negative volume prints nothing; its setting is left alone.
    CHECK(f.object->volumes[2]->config.has("extruder"));
    CHECK(effective_object_filaments(*f.object) == std::vector<int>{3});
}

TEST_CASE("clear_volume_filament_overrides keeps modifiers when told to, like the GUI", "[FilamentModel]")
{
    Fixture f;
    f.part->config.set_key_value("extruder", new ConfigOptionInt(1));
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Inlay", 2);

    const std::vector<ClearedOverride> cleared = clear_volume_filament_overrides(*f.object, /*include_modifiers=*/false);

    REQUIRE(cleared.size() == 1);
    CHECK(cleared[0].volume_id == 0);
    CHECK(f.object->volumes[1]->config.opt_int("extruder") == 2);
    CHECK(other_volume_filaments(*f.object, 3) == std::vector<int>{2});
}

TEST_CASE("clear_volume_filament_overrides reports nothing when there is nothing to clear", "[FilamentModel]")
{
    Fixture f;
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Plain", 0);

    CHECK(clear_volume_filament_overrides(*f.object, true).empty());
    CHECK(other_volume_filaments(*f.object, 3).empty());
}

TEST_CASE("volume_filament_override_count counts parts and modifiers with their own slot", "[FilamentModel]")
{
    Fixture f;
    f.part->config.set_key_value("extruder", new ConfigOptionInt(3));   // same as the object, still an override
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Pinned", 1);
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Inherits", 0);
    f.add(ModelVolumeType::NEGATIVE_VOLUME, "Hole", 1);                  // prints nothing, not counted

    CHECK(volume_filament_override_count(*f.object) == 2);
}

TEST_CASE("other_volume_filaments skips volumes already on the object's slot", "[FilamentModel]")
{
    Fixture f;
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Same", 3);
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Other", 1);
    f.add(ModelVolumeType::PARAMETER_MODIFIER, "Other again", 1);

    CHECK(other_volume_filaments(*f.object, 3) == std::vector<int>{1});
}
