#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPObjectOrder.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>
#include <vector>

// order_objects' decisions: the names the plate menu's Order By offers, and whether a plate's objects
// already stand in the order asked for.

using namespace Slic3r::GUI::OrcaMCP;

namespace {

Slic3r::ModelObject* cube_of_height(Slic3r::Model& model, const std::string& name, double height)
{
    Slic3r::ModelObject* object = model.add_object();
    object->name                = name;
    object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(10, 10, height)));
    object->add_instance();
    return object;
}

} // namespace

TEST_CASE("order_objects' names map to the menu's orders, and another is refused naming the four", "[McpObjectOrder][orcamcp]")
{
    CHECK(object_order_named("name") == Slic3r::GUI::ObjectOrder::Name);
    CHECK(object_order_named("filament") == Slic3r::GUI::ObjectOrder::Filament);
    CHECK(object_order_named("height") == Slic3r::GUI::ObjectOrder::Height);
    CHECK(object_order_named("size") == Slic3r::GUI::ObjectOrder::Size);
    CHECK_FALSE(object_order_named("weight").has_value());
    CHECK_FALSE(object_order_named("Height").has_value());
    CHECK(object_order_names() == "name, filament, height or size");
}

TEST_CASE("plate_in_order is true for objects already in the direction asked", "[McpObjectOrder][orcamcp]")
{
    Slic3r::Model model;
    auto*         low  = cube_of_height(model, "low", 5);
    auto*         mid  = cube_of_height(model, "mid", 10);
    auto*         high = cube_of_height(model, "high", 20);

    CHECK(plate_in_order({low, mid, high}, Slic3r::GUI::ObjectOrder::Height, false));
    CHECK_FALSE(plate_in_order({low, mid, high}, Slic3r::GUI::ObjectOrder::Height, true));
    CHECK(plate_in_order({high, mid, low}, Slic3r::GUI::ObjectOrder::Height, true));
    CHECK_FALSE(plate_in_order({high, mid, low}, Slic3r::GUI::ObjectOrder::Height, false));
    CHECK_FALSE(plate_in_order({mid, low, high}, Slic3r::GUI::ObjectOrder::Height, false));
}

TEST_CASE("plate_in_order holds for a single object and for an empty plate", "[McpObjectOrder][orcamcp]")
{
    Slic3r::Model model;
    auto*         only = cube_of_height(model, "only", 5);
    CHECK(plate_in_order({only}, Slic3r::GUI::ObjectOrder::Name, false));
    CHECK(plate_in_order({only}, Slic3r::GUI::ObjectOrder::Name, true));
    CHECK(plate_in_order({}, Slic3r::GUI::ObjectOrder::Size, false));
}
