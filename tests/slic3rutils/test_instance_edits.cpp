#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPInstanceEdits.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <nlohmann/json.hpp>

#include <string>

// What set_instance_count, fill_bed_with_instances and delete_object with instance_id decide before they
// change anything: the arguments read as given, never reinterpreted, and the refusals of the GUI's own
// instance actions (Plater::can_increase_instances, the object list's last-instance rule).

using namespace Slic3r::GUI::OrcaMCP;
using json = nlohmann::json;

namespace {

Slic3r::ModelObject& cube_with_instances(Slic3r::Model& model, int count)
{
    Slic3r::ModelObject* object = model.add_object();
    object->name                = "cube";
    object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(20, 20, 20)));
    for (int i = 0; i < count; ++i)
        object->add_instance()->set_offset(Slic3r::Vec3d(30.0 * i, 0.0, 0.0));
    return *object;
}

Slic3r::BoundingBoxf3 box_at(double x, double y, double size = 20.0)
{
    return Slic3r::BoundingBoxf3(Slic3r::Vec3d(x, y, 0.0), Slic3r::Vec3d(x + size, y + size, size));
}

} // namespace

TEST_CASE("An instance_id is read as a whole number 0 or more, and anything else is refused", "[InstanceEdits][orcamcp]")
{
    std::string error;
    CHECK_FALSE(read_instance_id(json::object(), error));
    CHECK(error.empty());
    CHECK(read_instance_id({{"instance_id", 2}}, error) == 2);
    CHECK(read_instance_id({{"instance_id", "2"}}, error) == 2); // a stale client's string, as every integer argument takes it
    const json bad = GENERATE(json(nullptr), json(-1), json(1.5), json("first"));
    error.clear();
    CHECK_FALSE(read_instance_id({{"instance_id", bad}}, error));
    CHECK(error.find("instance_id must be") != std::string::npos);
}

TEST_CASE("An instance_id the object does not have names the ones it has", "[InstanceEdits][orcamcp]")
{
    Slic3r::Model model;
    const auto&   cube = cube_with_instances(model, 3);
    CHECK_FALSE(instance_id_error(0, cube, 2));
    const auto error = instance_id_error(0, cube, 3);
    REQUIRE(error);
    CHECK(error->find("0 to 2") != std::string::npos);
}

TEST_CASE("The count is 1 to 1000; 0 points to delete_object", "[InstanceEdits][orcamcp]")
{
    std::string error;
    CHECK(read_instance_count({{"count", 1}}, error) == 1);
    CHECK(read_instance_count({{"count", 1000}}, error) == 1000);
    CHECK_FALSE(read_instance_count({{"count", 0}}, error));
    CHECK(error.find("delete_object") != std::string::npos);
    error.clear();
    CHECK_FALSE(read_instance_count({{"count", 1001}}, error));
    CHECK(error.find("at most 1000") != std::string::npos);
    error.clear();
    CHECK_FALSE(read_instance_count({{"count", 2.5}}, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Instances are not added to an object with an unprintable instance", "[InstanceEdits][orcamcp]")
{
    Slic3r::Model model;
    auto&         cube = cube_with_instances(model, 2);
    CHECK_FALSE(instance_edit_refusal(0, cube, {}, "set_instance_count"));
    cube.instances[1]->printable = false;
    const auto refusal = instance_edit_refusal(0, cube, {}, "set_instance_count");
    REQUIRE(refusal);
    CHECK(refusal->find("set_object_printable") != std::string::npos);
    CHECK(refusal->find("set_instance_count again") != std::string::npos);
}

TEST_CASE("Instances are not added to a piece of a cut", "[InstanceEdits][orcamcp]")
{
    Slic3r::Model model;
    const auto&   cube    = cube_with_instances(model, 1);
    const auto    refusal = instance_edit_refusal(0, cube, {0, 1}, "fill_bed_with_instances");
    REQUIRE(refusal);
    CHECK(refusal->find("invalidate_cut_info") != std::string::npos);
}

TEST_CASE("An object's only instance is not deleted on its own: that is the object", "[InstanceEdits][orcamcp]")
{
    Slic3r::Model model;
    const auto&   single = cube_with_instances(model, 1);
    const auto    refusal = instance_delete_refusal(0, single, 0, false);
    REQUIRE(refusal);
    CHECK(refusal->find("without instance_id") != std::string::npos);
    const auto& pair = cube_with_instances(model, 2);
    CHECK_FALSE(instance_delete_refusal(1, pair, 1, false));
    CHECK(instance_delete_refusal(1, pair, 2, false));
}

TEST_CASE("instance_id and volume_id together are refused", "[InstanceEdits][orcamcp]")
{
    Slic3r::Model model;
    const auto&   pair    = cube_with_instances(model, 2);
    const auto    refusal = instance_delete_refusal(0, pair, 1, true);
    REQUIRE(refusal);
    CHECK(refusal->find("not both") != std::string::npos);
}

TEST_CASE("Only the candidates whose footprint overlaps another are crowded", "[InstanceEdits][orcamcp]")
{
    // 0 and 1 overlap; 2 stands clear; 3 touches 2's edge only.
    const std::vector<Slic3r::BoundingBoxf3> boxes = {box_at(0, 0), box_at(10, 10), box_at(100, 0), box_at(120, 0)};
    CHECK(overlapping_boxes(boxes, {1}) == std::vector<int>{1});
    CHECK(overlapping_boxes(boxes, {2, 3}).empty());
    CHECK(overlapping_boxes(boxes, {0, 1, 2}) == std::vector<int>{0, 1});
}
