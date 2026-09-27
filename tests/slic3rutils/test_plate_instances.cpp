#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "plate_list_fixtures.hpp"
#include "test_utils.hpp"

#include <memory>
#include <string>
#include <vector>

// Which plate each instance of an object is on. An object's instances can stand on different plates
// (a clone onto another plate, an arrange over several), and the plate list only knows an instance
// once something tells it (PartPlateList::notify_instance_update). Loading a project told it about
// each object's first instance only, so a project saved with an object on two plates came back with
// the second plate empty: nothing to slice there, and MCP's scene listed that instance nowhere.
//
// These tests call PartPlateList::notify_object_added, which ObjectList::add_object_to_list calls when
// an object joins the scene; that line itself needs the app, so reverting it leaves them green.

using Catch::Matchers::WithinAbs;
using Slic3r::Vec3d;
using Slic3r::GUI::PartPlateList;
using namespace plate_list_fixtures;

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
