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

TEST_CASE("print_order_json orders by position and maps instances to indices", "[McpObjectOrder][orcamcp]")
{
    Slic3r::Model model;
    Slic3r::ModelObject* a = cube_of_height(model, "a", 10);
    Slic3r::ModelObject* b = cube_of_height(model, "b", 10);
    b->add_instance();

    // b's second instance prints first, then a, then b's first.
    const std::vector<std::pair<const Slic3r::ModelInstance*, int>> positions = {
        {a->instances[0], 2}, {b->instances[0], 3}, {b->instances[1], 1}};
    const nlohmann::json json = print_order_json(positions, model, "b is printed later\nthan a");

    REQUIRE(json["print_order"].size() == 3);
    CHECK(json["print_order"][0] == nlohmann::json({{"object_id", 1}, {"instance_id", 1}}));
    CHECK(json["print_order"][1] == nlohmann::json({{"object_id", 0}, {"instance_id", 0}}));
    CHECK(json["print_order"][2] == nlohmann::json({{"object_id", 1}, {"instance_id", 0}}));
    CHECK(json["print_order_differs"] == true);
    CHECK(json["print_order_note"] == "b is printed later\nthan a");
    CHECK(json["print_order_unknown"].is_null());
}

TEST_CASE("print_order_json: the object list's order is not a difference", "[McpObjectOrder][orcamcp]")
{
    Slic3r::Model model;
    Slic3r::ModelObject* a = cube_of_height(model, "a", 10);
    Slic3r::ModelObject* b = cube_of_height(model, "b", 10);
    const nlohmann::json json = print_order_json({{b->instances[0], 2}, {a->instances[0], 1}}, model, "");
    CHECK(json["print_order_differs"] == false);
    CHECK(json["print_order_note"].is_null());
    CHECK(json["print_order"][0]["object_id"] == 0);

    const nlohmann::json unknown = print_order_unknown_json("not by object");
    CHECK(unknown["print_order"].is_null());
    CHECK(unknown["print_order_differs"].is_null());
    CHECK(unknown["print_order_note"].is_null());
    CHECK(unknown["print_order_unknown"] == "not by object");
}
