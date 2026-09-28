#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"

// Adding, deleting and merging filament slots: what the app's own code does to the model and the
// project when a slot goes, and add_filament_slot / delete_filament_slot's decisions apart from the app.

using namespace Slic3r;

namespace {

ModelObject* object_with_two_parts(Model& model)
{
    ModelObject* object = model.add_object();
    object->add_volume(TriangleMesh(its_make_cube(10, 10, 10)));
    object->add_volume(TriangleMesh(its_make_cube(5, 5, 5)));
    object->add_instance();
    return object;
}

int support_filament(const ModelConfigObject& config, const char* key) { return config.has(key) ? config.opt_int(key) : -1; }

} // namespace

TEST_CASE("deleting a filament renumbers each support filament in its own config", "[FilamentSlots]")
{
    Model        model;
    ModelObject* object = object_with_two_parts(model);
    object->config.set_key_value("support_filament", new ConfigOptionInt(3));
    object->volumes[0]->config.set_key_value("support_interface_filament", new ConfigOptionInt(4));
    object->volumes[1]->config.set_key_value("support_filament", new ConfigOptionInt(2));

    // Slot 2 (index 1) goes: 3 and 4 move down one, a reference to 2 falls back to the default.
    GUI::renumber_support_filaments_after_delete(*object, 1);

    CHECK(support_filament(object->config, "support_filament") == 2);
    CHECK(support_filament(object->volumes[0]->config, "support_interface_filament") == 3);
    CHECK_FALSE(object->volumes[1]->config.has("support_filament"));
    // Upstream wrote a volume's renumbered value into the object: the object gained the volume's key.
    CHECK_FALSE(object->config.has("support_interface_filament"));
}

TEST_CASE("deleting a filament leaves the support filaments before it as they are", "[FilamentSlots]")
{
    Model        model;
    ModelObject* object = object_with_two_parts(model);
    object->config.set_key_value("support_filament", new ConfigOptionInt(1));
    object->volumes[1]->config.set_key_value("support_interface_filament", new ConfigOptionInt(2));

    GUI::renumber_support_filaments_after_delete(*object, 3);

    CHECK(support_filament(object->config, "support_filament") == 1);
    CHECK(support_filament(object->volumes[1]->config, "support_interface_filament") == 2);
    CHECK_FALSE(object->volumes[0]->config.has("support_filament"));
}
