#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPMeshHealth.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPQuit.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "mesh_fixtures.hpp"

#include <fstream>
#include <set>
#include <sstream>
#include <string>

// How get_scene_info (and load_model's loaded_objects, which share the object serializer) name the
// objects and plates every other tool takes by number, and the reference example agents copy from:
// its keys are held to what the serializers write, so the example cannot drift from a real response.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using namespace mesh_fixtures;
using Slic3r::GUI::OrcaMCPPlateUtils;
using Slic3r::GUI::PlateEntry;
using Slic3r::GUI::PrimeTowerState;
using json = nlohmann::json;

namespace {

// A one-part object whose one instance stands on the plate being described.
struct PlateObject : OnePartObject
{
    PlateObject() : OnePartObject(TriangleMesh(its_make_cube(10.0, 10.0, 10.0)), "cube") {}

    InstancesOnPlate here() const { return instances_on_plate(*object, [](int) { return true; }); }

    json entry(bool with_features = false) const
    {
        const DynamicPrintConfig print_cfg = DynamicPrintConfig::full_print_config();
        const MeshHealth         health    = object_mesh_health(*object);
        const auto               footprint = OrcaMCPPlateUtils::GetObjectFootprint(*object, plate_box_of(*object, here()), print_cfg);
        return OrcaMCPPlateUtils::ScenePlateObjectJson(*object, 0, here(), health, footprint, with_features);
    }
};

json plate_entry(bool is_current)
{
    PlateEntry plate;
    plate.name       = "";
    plate.index      = 1;
    plate.is_current = is_current;
    plate.box        = BoundingBoxf3(Vec3d(0, 0, 0), Vec3d(256, 256, 256));
    return OrcaMCPPlateUtils::PlateJson(plate);
}

} // namespace

TEST_CASE("An object description's object_id is the index every tool takes", "[McpSceneDescription][orcamcp]")
{
    // Every tool's parameter is object_id and means the index; an agent reading "id": "71" beside
    // "object_index": 0 passed 71.
    const PlateObject plate_object;
    for (const json& description : {model_object_summary_json(*plate_object.object, 4), plate_object.entry()}) {
        CHECK(description.at("object_id") == description.at("object_index"));
        CHECK_FALSE(description.contains("id"));
        CHECK(description.at("internal_id") == std::to_string(plate_object.object->id().id));
    }
    CHECK(model_object_summary_json(*plate_object.object, 4).at("object_id") == 4);
}

TEST_CASE("An unplaced object is named the same way", "[McpSceneDescription][orcamcp]")
{
    const PlateObject      plate_object;
    const InstancesOnPlate nowhere = plate_object.here();
    const json unplaced = OrcaMCPPlateUtils::UnplacedObjectJson(*plate_object.object, 2, nowhere, object_mesh_health(*plate_object.object), false);
    CHECK(unplaced.at("object_id") == 2);
    CHECK(unplaced.at("object_index") == 2);
    CHECK_FALSE(unplaced.contains("id"));
    CHECK(unplaced.contains("internal_id"));
}

TEST_CASE("A plate says its plate_index and whether it is the current one, keeping index", "[McpSceneDescription][orcamcp]")
{
    // select_plate, the transforms and get_slicing_status all say plate_index; nothing said which plate
    // per-plate tools act on.
    const json current = plate_entry(true);
    CHECK(current.at("plate_index") == 1);
    CHECK(current.at("index") == 1);
    CHECK(current.at("is_current") == true);
    CHECK(plate_entry(false).at("is_current") == false);
}

TEST_CASE("get_slicing_status's plates say plate_index too, keeping index", "[McpSceneDescription][orcamcp]")
{
    const json plate = plate_slicing_json(3, true, 100, {{"ok", true}});
    CHECK(plate.at("plate_index") == 3);
    CHECK(plate.at("index") == 3);
    CHECK(plate.at("slice_result_valid") == true);
    CHECK(plate.at("percent") == 100);
    CHECK(plate_slicing_json(0, false, std::nullopt, nullptr).at("percent").is_null());
}
