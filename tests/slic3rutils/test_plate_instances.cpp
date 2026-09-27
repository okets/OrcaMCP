#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_utils.hpp"

#include <memory>
#include <string>
#include <vector>

// Which plate each instance of an object is on, and how MCP reports it. An object's instances can
// stand on different plates (a clone onto another plate, an arrange over several), and the plate list
// only knows an instance once something tells it (PartPlateList::notify_instance_update). Loading a
// project told it about each object's first instance only, so a project saved with an object on two
// plates came back with the second plate empty: nothing to slice there, and MCP's scene listed that
// instance nowhere.
//
// These tests call PartPlateList::notify_object_added, which ObjectList::add_object_to_list calls when
// an object joins the scene; that line itself needs the app, so reverting it leaves them green.

using Catch::Matchers::WithinAbs;
using Slic3r::Vec3d;
using Slic3r::GUI::PartPlateList;

namespace {

constexpr double k_plate_size = 256.0;
constexpr double k_cube_size  = 20.0;

// The plate list of a 256 mm square bed, with `count` plates.
std::unique_ptr<PartPlateList> plate_list_for(Slic3r::Model& model, int count)
{
    auto plates = std::make_unique<PartPlateList>(int(k_plate_size), int(k_plate_size), k_plate_size, nullptr, &model,
                                                  Slic3r::ptFFF);
    const Slic3r::Pointfs bed{{0.0, 0.0}, {k_plate_size, 0.0}, {k_plate_size, k_plate_size}, {0.0, k_plate_size}};
    plates->set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    while (plates->get_plate_count() < count)
        plates->create_plate(false);
    return plates;
}

// Where a cube resting on `plate` sits when it is centred on it.
Vec3d centre_of(PartPlateList& plates, int plate)
{
    const Vec3d centre = plates.get_plate(plate)->get_plate_box().center();
    return {centre.x(), centre.y(), k_cube_size / 2.0};
}

// A 20 mm cube with an instance at each of `positions`, added to the scene as the app adds a loaded
// object (ObjectList::add_object_to_list: PartPlateList::notify_object_added).
Slic3r::ModelObject& add_cube(Slic3r::Model& model, PartPlateList& plates, const std::vector<Vec3d>& positions)
{
    Slic3r::ModelObject* object = model.add_object();
    object->name                = "cube";
    object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(k_cube_size, k_cube_size, k_cube_size)));
    object->center_around_origin(false);
    for (const Vec3d& position : positions)
        object->add_instance()->set_offset(position);
    plates.notify_object_added(int(model.objects.size()) - 1);
    return *object;
}

} // namespace

TEST_CASE("An object added to the plate list has every instance on the plate it stands on", "[PlateInstances][orcamcp]")
{
    Slic3r::Model                        model;
    const std::unique_ptr<PartPlateList> plates = plate_list_for(model, 2);
    add_cube(model, *plates, {centre_of(*plates, 0), centre_of(*plates, 1)});

    CHECK(plates->find_instance(0, 0) == 0);
    CHECK(plates->find_instance(0, 1) == 1);
}

TEST_CASE("A project saved with an object on two plates reloads with each instance on its plate", "[PlateInstances][orcamcp]")
{
    Slic3r::Model      model;
    ScopedTemporaryDir backup_dir("orca_two_plates_src");
    model.set_backup_path(backup_dir.string());
    const std::unique_ptr<PartPlateList> plates = plate_list_for(model, 2);
    const Vec3d                          second = centre_of(*plates, 1);
    add_cube(model, *plates, {centre_of(*plates, 0), second});

    ScopedTemporaryFile        file(".3mf");
    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::StoreParams        store;
    store.path     = file.string();
    store.model    = &model;
    store.config   = &config;
    store.strategy = Slic3r::SaveStrategy::Zip64 | Slic3r::SaveStrategy::Silence;
    REQUIRE(plates->store_to_3mf_structure(store.plate_data_list, false) == 0);
    REQUIRE(store_bbs_3mf(store));
    Slic3r::release_PlateData_list(store.plate_data_list);

    Slic3r::Model      loaded;
    ScopedTemporaryDir loaded_backup_dir("orca_two_plates_dst");
    loaded.set_backup_path(loaded_backup_dir.string());
    Slic3r::DynamicPrintConfig        loaded_config;
    Slic3r::ConfigSubstitutionContext substitutions{Slic3r::ForwardCompatibilitySubstitutionRule::Enable};
    Slic3r::PlateDataPtrs             plate_data;
    std::vector<Slic3r::Preset*>      project_presets;
    bool                              is_bbl_3mf = false, is_orca_3mf = false;
    Slic3r::Semver                    version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &loaded_config, &substitutions, &loaded, &plate_data, &project_presets,
                         &is_bbl_3mf, &is_orca_3mf, &version, nullptr,
                         Slic3r::LoadStrategy::LoadModel | Slic3r::LoadStrategy::LoadConfig));
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.objects[0]->instances.size() == 2);
    // The file has it right: the second plate lists the object, and the second instance kept its place.
    REQUIRE(plate_data.size() == 2);
    CHECK(plate_data[1]->obj_inst_map.size() == 1);
    CHECK_THAT(loaded.objects[0]->instances[1]->get_offset().x(), WithinAbs(second.x(), 1e-6));

    // As the app opens it: the plates from the file first, over the scene the project replaces (empty
    // by then), then each loaded object added to the scene (Plater::priv::load_model_objects, then
    // ObjectList::add_object_to_list). A plate made while the object is already in the scene would
    // take its instance itself (PartPlateList::construct_objects_list_for_new_plate).
    Slic3r::Model                        scene;
    const std::unique_ptr<PartPlateList> reloaded = plate_list_for(scene, 1);
    REQUIRE(reloaded->load_from_3mf_structure(plate_data) == 0);
    Slic3r::release_PlateData_list(plate_data);
    REQUIRE(reloaded->get_plate_count() == 2);
    scene.add_object(*loaded.objects[0]);
    reloaded->notify_object_added(0);

    CHECK(reloaded->find_instance(0, 0) == 0);
    CHECK(reloaded->find_instance(0, 1) == 1);
}

TEST_CASE("Each instance is measured by its own box on the plate it is on", "[PlateInstances][orcamcp]")
{
    Slic3r::Model                        model;
    const std::unique_ptr<PartPlateList> plates = plate_list_for(model, 2);
    const Vec3d                          second = centre_of(*plates, 1);
    const Slic3r::ModelObject&           cube   = add_cube(model, *plates, {centre_of(*plates, 0), second});

    const std::vector<Slic3r::GUI::OrcaMCP::InstancePlacement> placements =
        Slic3r::GUI::OrcaMCP::instance_placements(cube, 0, *plates);
    REQUIRE(placements.size() == 2);
    CHECK(placements[0].plate_index == 0);
    CHECK(placements[0].on_bed);
    CHECK(placements[1].plate_index == 1);
    CHECK(placements[1].on_bed); // the box around both instances fits neither plate
    CHECK_THAT(placements[1].position.x(), WithinAbs(second.x(), 1e-6));
}

TEST_CASE("An instance on no plate is listed as unplaced, even when the object's other instances are on plates",
          "[PlateInstances][orcamcp]")
{
    Slic3r::Model                        model;
    const std::unique_ptr<PartPlateList> plates = plate_list_for(model, 2);
    const Vec3d                          off_every_plate(2000.0, 128.0, k_cube_size / 2.0);
    add_cube(model, *plates, {centre_of(*plates, 0), off_every_plate}); // object 0: one copy placed, one not
    add_cube(model, *plates, {centre_of(*plates, 1)});                   // object 1: on its plate
    add_cube(model, *plates, {off_every_plate});                         // object 2: on no plate at all
    REQUIRE(plates->find_instance(0, 1) == -1);

    const nlohmann::json unplaced = Slic3r::GUI::OrcaMCPPlateUtils::UnplacedObjectsJson(model, *plates, {}, false);

    REQUIRE(unplaced.size() == 2);
    CHECK(unplaced[0]["object_index"] == 0);
    CHECK(unplaced[0]["instance_count"] == 2);
    CHECK(unplaced[0]["unplaced_instances"] == nlohmann::json::array({1}));
    CHECK_THAT(unplaced[0]["position"]["x"].get<double>(), WithinAbs(off_every_plate.x(), 1e-6)); // that copy's
    CHECK(unplaced[1]["object_index"] == 2);
    CHECK(unplaced[1]["unplaced_instances"] == nlohmann::json::array({0}));
}

TEST_CASE("A later instance on a spiral-vase plate is placed without giving the object vase settings", "[PlateInstances][orcamcp]")
{
    // The first instance is notified as upstream always did (is_new: a vase plate's settings are
    // applied to the whole object); a later one is placed only, since its user may have declined them
    // when it was put there. That first-instance side effect needs the app's presets, so it is not
    // reached here; a later instance that reached the vase code would need them (or its dialog) too,
    // and this test would crash rather than fail.
    Slic3r::Model                        model;
    const std::unique_ptr<PartPlateList> plates = plate_list_for(model, 2);
    plates->get_plate(1)->config()->set_key_value("spiral_mode", new Slic3r::ConfigOptionBool(true));

    const Slic3r::ModelObject& cube = add_cube(model, *plates, {centre_of(*plates, 0), centre_of(*plates, 1)});

    CHECK(plates->find_instance(0, 1) == 1);
    CHECK_FALSE(cube.config.has("wall_loops"));
    CHECK_FALSE(cube.config.has("sparse_infill_density"));
}
