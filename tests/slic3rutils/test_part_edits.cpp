#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "mesh_fixtures.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPartEdits.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <map>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// The decisions of the part and mesh-edit tools (split_object, add_volume, set_volume_type, the
// volume_id forms of move/rotate/scale/mirror, delete, rename and the settings tools,
// assemble_objects, merge_parts): what the object list refuses or asks about, said before anything
// changes, and the plate-frame geometry of one volume's transform. The tools run the object list's own
// actions in the app; what is decided before them, and the volume transform, is checked here.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

namespace {

TriangleMesh cube_at(double size, const Vec3d& min_corner)
{
    indexed_triangle_set its = its_make_cube(size, size, size);
    return TriangleMesh(mesh_fixtures::translated(its, min_corner.cast<float>()));
}

// A volume holding `mesh` as it is: no re-centring, so its coordinates are the object's.
ModelVolume* add_part(ModelObject& object, const TriangleMesh& mesh, ModelVolumeType type = ModelVolumeType::MODEL_PART)
{
    ModelVolume* volume = object.add_volume(mesh, false);
    volume->set_type(type);
    return volume;
}

// Two 10 mm cubes, part 0 at the origin and part 1 20 mm along the object's X; one instance at
// `offset`, turned `z_degrees` about the plate's Z.
ModelObject& two_part_object(Model& model, const Vec3d& offset = Vec3d(100, 100, 0), double z_degrees = 0.)
{
    ModelObject& object = *model.add_object();
    object.name         = "pair";
    add_part(object, cube_at(10., Vec3d::Zero()));
    add_part(object, cube_at(10., Vec3d(20., 0., 0.)));
    ModelInstance* instance = object.add_instance();
    instance->set_offset(offset);
    instance->set_rotation(Vec3d(0., 0., Geometry::deg2rad(z_degrees)));
    return object;
}

ModelObject& one_part_object(Model& model, const indexed_triangle_set& its)
{
    ModelObject& object = *model.add_object();
    add_part(object, TriangleMesh(its));
    object.add_instance();
    return object;
}

void check_vec(const Vec3d& actual, const Vec3d& expected, double tol = 1e-6)
{
    CHECK_THAT(actual.x(), WithinAbs(expected.x(), tol));
    CHECK_THAT(actual.y(), WithinAbs(expected.y(), tol));
    CHECK_THAT(actual.z(), WithinAbs(expected.z(), tol));
}

bool mentions(const std::optional<std::string>& message, const std::string& text)
{
    return message && message->find(text) != std::string::npos;
}

} // namespace

// ---- Volumes ----

TEST_CASE("The part tools take the volume type names get_object_info reports", "[PartEdits][orcamcp]")
{
    CHECK(volume_type_names() == std::vector<std::string>{"part", "negative_volume", "modifier", "support_blocker", "support_enforcer"});
    CHECK(volume_type_from_name("part") == ModelVolumeType::MODEL_PART);
    CHECK(volume_type_from_name("negative_volume") == ModelVolumeType::NEGATIVE_VOLUME);
    CHECK(volume_type_from_name("modifier") == ModelVolumeType::PARAMETER_MODIFIER);
    CHECK(volume_type_from_name("support_blocker") == ModelVolumeType::SUPPORT_BLOCKER);
    CHECK(volume_type_from_name("support_enforcer") == ModelVolumeType::SUPPORT_ENFORCER);
    CHECK_FALSE(volume_type_from_name("negative_part"));
}

TEST_CASE("A volume id names one of the object's volumes", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model);
    CHECK_FALSE(volume_id_error(0, object, 0));
    CHECK_FALSE(volume_id_error(0, object, 1));
    CHECK(mentions(volume_id_error(0, object, 2), "0 to 1"));
    CHECK(mentions(volume_id_error(0, object, -1), "0 to 1"));
}

TEST_CASE("A volume is reported by its box on instance 0, in plate millimetres", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model, Vec3d(100, 100, 0), 90.);
    // Turned 90 degrees about Z, the object's X runs along the plate's Y: part 1 sits at y 120..130.
    const nlohmann::json row = volume_row_json(object, 1);
    CHECK(row["volume_id"] == 1);
    CHECK(row["type"] == "part");
    CHECK_THAT(row["bounding_box"]["min"]["y"].get<double>(), WithinAbs(120., 1e-6));
    CHECK_THAT(row["bounding_box"]["max"]["y"].get<double>(), WithinAbs(130., 1e-6));
    CHECK_THAT(row["position"]["x"].get<double>(), WithinAbs(95., 1e-6));
    CHECK(volume_rows_json(object).size() == 2);
}

// ---- Cut objects ----

TEST_CASE("The pieces of one cut are kept together, and other objects are not", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& lower = two_part_object(model);
    ModelObject& upper = two_part_object(model);
    two_part_object(model);
    lower.cut_id.init();
    upper.cut_id.copy(lower.cut_id);
    CHECK(cut_siblings(model, 0) == std::vector<int>{0, 1});
    CHECK(cut_siblings(model, 1) == std::vector<int>{0, 1});
    CHECK(cut_siblings(model, 2).empty());
    CHECK(cut_refusal(1, {0, 1}, "its solid parts cannot be deleted").find("invalidate_cut_info") != std::string::npos);
}

// ---- split_object ----

TEST_CASE("A split to objects needs more than one piece", "[PartEdits][orcamcp]")
{
    Model model;
    CHECK_FALSE(splits_to_several_objects(one_part_object(model, its_make_cube(10., 10., 10.))));
    CHECK(splits_to_several_objects(one_part_object(model, mesh_fixtures::separate_cubes(2))));
    CHECK(splits_to_several_objects(two_part_object(model)));

    // One solid part and a modifier split into one object: the modifier is not a piece.
    ModelObject& with_modifier = two_part_object(model);
    with_modifier.volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    CHECK_FALSE(splits_to_several_objects(with_modifier));
    CHECK(mentions(split_refusal(3, with_modifier, SplitTarget::objects, std::nullopt, false), "one"));
}

TEST_CASE("A split to objects leaves the object's modifiers, negative and support volumes behind", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model);
    add_part(object, cube_at(2., Vec3d(1, 1, 1)), ModelVolumeType::PARAMETER_MODIFIER);
    add_part(object, cube_at(2., Vec3d(1, 1, 1)), ModelVolumeType::NEGATIVE_VOLUME);
    add_part(object, cube_at(2., Vec3d(1, 1, 1)), ModelVolumeType::SUPPORT_ENFORCER);
    CHECK(volumes_dropped_by_split_to_objects(object) == std::vector<int>{2, 3, 4});
}

TEST_CASE("A split to parts splits the named volume, which needs several shells", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& shells = one_part_object(model, mesh_fixtures::separate_cubes(3));
    CHECK(split_volume_of(shells, std::nullopt) == 0);
    CHECK_FALSE(split_refusal(0, shells, SplitTarget::parts, std::nullopt, false));

    // A multi-part object names the part, as the object list splits the selected part.
    ModelObject& pair = two_part_object(model);
    CHECK_FALSE(split_volume_of(pair, std::nullopt));
    CHECK(mentions(split_refusal(1, pair, SplitTarget::parts, std::nullopt, false), "volume_id"));
    // Each of its parts is one shell: nothing to split.
    CHECK(mentions(split_refusal(1, pair, SplitTarget::parts, 1, false), "one shell"));

    ModelObject& cube = one_part_object(model, its_make_cube(10., 10., 10.));
    CHECK(mentions(split_refusal(2, cube, SplitTarget::parts, std::nullopt, false), "one shell"));
}

TEST_CASE("A split takes each argument only for the target it applies to", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& shells = one_part_object(model, mesh_fixtures::separate_cubes(2));
    // keep_height answers the floating-pieces prompt of a split to objects only.
    CHECK(mentions(split_refusal(0, shells, SplitTarget::parts, std::nullopt, /*keep_height_given=*/true), "keep_height"));
    // A split to objects splits the whole object, as the object list's does.
    CHECK(mentions(split_refusal(0, shells, SplitTarget::objects, 0, false), "volume_id"));
    CHECK_FALSE(split_refusal(0, shells, SplitTarget::objects, std::nullopt, true));
}

// ---- set_volume_type ----

TEST_CASE("The last solid part keeps its type, and a text volume never becomes a support volume", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& pair = two_part_object(model);
    CHECK_FALSE(volume_type_change_refusal(0, pair, 1, ModelVolumeType::PARAMETER_MODIFIER));
    pair.volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    CHECK(mentions(volume_type_change_refusal(0, pair, 0, ModelVolumeType::NEGATIVE_VOLUME), "last solid part"));
    // Back to a part is always allowed, and the same type changes nothing (not a refusal).
    CHECK_FALSE(volume_type_change_refusal(0, pair, 1, ModelVolumeType::MODEL_PART));
    CHECK_FALSE(volume_type_change_refusal(0, pair, 0, ModelVolumeType::MODEL_PART));

    pair.volumes[1]->text_configuration.emplace();
    CHECK(mentions(volume_type_change_refusal(0, pair, 1, ModelVolumeType::SUPPORT_BLOCKER), "text"));
    CHECK(mentions(volume_type_change_refusal(0, pair, 1, ModelVolumeType::SUPPORT_ENFORCER), "text"));
    CHECK_FALSE(volume_type_change_refusal(0, pair, 1, ModelVolumeType::NEGATIVE_VOLUME));
}

// ---- delete_object with volume_id ----

TEST_CASE("The last solid part is not deleted, and a cut object's parts wait for invalidate_cut_info", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& pair = two_part_object(model);
    CHECK_FALSE(volume_delete_refusal(0, pair, 1, {}));
    pair.volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    CHECK(mentions(volume_delete_refusal(0, pair, 0, {}), "delete_object without volume_id"));
    CHECK_FALSE(volume_delete_refusal(0, pair, 1, {}));

    ModelObject& cut = two_part_object(model);
    cut.cut_id.init();
    CHECK(mentions(volume_delete_refusal(1, cut, 1, {1, 2}), "invalidate_cut_info"));
    // A modifier of a cut object is deleted as any other.
    cut.volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    CHECK_FALSE(volume_delete_refusal(1, cut, 1, {1, 2}));
}

TEST_CASE("Deleting down to one volume moves that volume's settings to the object", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& pair = two_part_object(model);
    pair.volumes[0]->config.set_key_value("wall_loops", new ConfigOptionInt(5));
    pair.volumes[1]->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));
    CHECK(settings_moving_to_object(pair, 1) == std::vector<std::string>{"wall_loops"});
    CHECK(settings_moving_to_object(pair, 0) == std::vector<std::string>{"sparse_infill_density"});
    add_part(pair, cube_at(2., Vec3d(1, 1, 1)), ModelVolumeType::PARAMETER_MODIFIER);
    CHECK(settings_moving_to_object(pair, 1).empty()); // two volumes stay
}

// ---- Transforms of one volume ----

TEST_CASE("A part of a one-part object or of a cut is not transformed on its own", "[PartEdits][orcamcp]")
{
    Model model;
    CHECK(mentions(volume_transform_refusal(0, one_part_object(model, its_make_cube(10., 10., 10.)), 0, {}), "omit volume_id"));
    ModelObject& pair = two_part_object(model);
    CHECK_FALSE(volume_transform_refusal(1, pair, 1, {}));
    CHECK(mentions(volume_transform_refusal(1, pair, 1, {1, 2}), "invalidate_cut_info"));
    pair.volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    CHECK_FALSE(volume_transform_refusal(1, pair, 1, {1, 2})); // a modifier of a cut object moves
}

TEST_CASE("A part moves along the plate's axes whatever the instance's rotation, and its sibling stays", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object      = two_part_object(model, Vec3d(100, 100, 0), 90.);
    const BoundingBoxf3 part = volume_world_box(object, 1);
    const BoundingBoxf3 other = volume_world_box(object, 0);
    translate_volume_in_plate_frame(object, 1, Vec3d(5., 0., 0.));
    check_vec(volume_world_box(object, 1).min, part.min + Vec3d(5., 0., 0.));
    check_vec(volume_world_box(object, 0).min, other.min);
}

TEST_CASE("A part turns about the plate's axis, about its own centre", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model, Vec3d(100, 100, 0), 90.);
    // A 10 x 10 x 20 part: turned 90 degrees about the plate's X, it lies 20 long along the plate's Y.
    object.volumes[1]->set_mesh(its_make_cube(10., 10., 20.));
    object.volumes[1]->set_offset(Vec3d(20., 0., 0.));
    const Vec3d centre = volume_world_box(object, 1).center();
    transform_volume_in_plate_frame(object, 1, Geometry::rotation_transform(Vec3d(Geometry::deg2rad(90.), 0., 0.)));
    const BoundingBoxf3 after = volume_world_box(object, 1);
    check_vec(after.center(), centre);
    CHECK_THAT(after.size().y(), WithinAbs(20., 1e-6));
    CHECK_THAT(after.size().z(), WithinAbs(10., 1e-6));
    check_vec(volume_world_box(object, 0).min, Vec3d(90., 100., 0.)); // the other part: turned instance, x -10..0
}

TEST_CASE("A part scales along the plate's axes about its own centre", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model, Vec3d(100, 100, 0), 90.);
    const Vec3d  centre = volume_world_box(object, 1).center();
    transform_volume_in_plate_frame(object, 1, Geometry::scale_transform(Vec3d(2., 1., 1.)));
    const BoundingBoxf3 after = volume_world_box(object, 1);
    check_vec(after.center(), centre);
    CHECK_THAT(after.size().x(), WithinAbs(20., 1e-6)); // the plate's X, the object's Y
    CHECK_THAT(after.size().y(), WithinAbs(10., 1e-6));
}

TEST_CASE("Every copy of the object shows a part's move in its own frame", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model);
    ModelInstance* turned = object.add_instance();
    turned->set_offset(Vec3d(200, 100, 0));
    turned->set_rotation(Vec3d(0., 0., Geometry::deg2rad(90.)));
    const BoundingBoxf3 on_turned = volume_world_box(object, 1, 1);
    translate_volume_in_plate_frame(object, 1, Vec3d(5., 0., 0.)); // the plate's X on instance 0
    // Instance 1 is turned 90 degrees: the object's X is its plate Y.
    check_vec(volume_world_box(object, 1, 1).min, on_turned.min + Vec3d(0., 5., 0.));
}

TEST_CASE("Moving the lowest part up drops the object back onto the bed, and a modifier's move does not", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model);
    object.volumes[1]->set_offset(Vec3d(0., 0., 5.)); // part 1 stands at z 5..15
    std::vector<double> before = instances_lowest_z(object);
    translate_volume_in_plate_frame(object, 0, Vec3d(0., 0., 10.)); // part 0 to z 10..20
    const std::vector<double> dropped = drop_after_volume_change(object, before, VolumeChange::move);
    CHECK_THAT(dropped[0], WithinAbs(5., 1e-6));
    CHECK_THAT(volume_world_box(object, 1).min.z(), WithinAbs(0., 1e-6));
    CHECK_THAT(volume_world_box(object, 0).min.z(), WithinAbs(5., 1e-6));

    // Modifiers are not what stands on the bed.
    ModelObject& with_modifier = two_part_object(model);
    with_modifier.volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    before = instances_lowest_z(with_modifier);
    translate_volume_in_plate_frame(with_modifier, 1, Vec3d(0., 0., 30.));
    CHECK_THAT(drop_after_volume_change(with_modifier, before, VolumeChange::move)[0], WithinAbs(0., 1e-6));
    CHECK_THAT(volume_world_box(with_modifier, 1).min.z(), WithinAbs(30., 1e-6));
}

TEST_CASE("A sinking object stays sinking when a part moves, and auto_drop off keeps an object up", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& sinking = two_part_object(model, Vec3d(100, 100, -3));
    std::vector<double> before = instances_lowest_z(sinking);
    translate_volume_in_plate_frame(sinking, 0, Vec3d(0., 0., 1.));
    CHECK_THAT(drop_after_volume_change(sinking, before, VolumeChange::move)[0], WithinAbs(0., 1e-6));
    CHECK_THAT(instances_lowest_z(sinking)[0], WithinAbs(-3., 1e-6));

    ModelObject& held = two_part_object(model);
    held.instances[0]->auto_drop = false;
    before = instances_lowest_z(held);
    translate_volume_in_plate_frame(held, 0, Vec3d(0., 0., 4.));
    translate_volume_in_plate_frame(held, 1, Vec3d(0., 0., 4.));
    CHECK_THAT(drop_after_volume_change(held, before, VolumeChange::move)[0], WithinAbs(0., 1e-6));
    CHECK_THAT(instances_lowest_z(held)[0], WithinAbs(4., 1e-6));
}

TEST_CASE("A part turned off the bed comes back down, as the GUI drops a resting object", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model);
    object.volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    object.volumes[0]->set_mesh(its_make_cube(10., 10., 20.)); // the only part, z 0..20
    const std::vector<double> before = instances_lowest_z(object);
    transform_volume_in_plate_frame(object, 0, Geometry::rotation_transform(Vec3d(Geometry::deg2rad(90.), 0., 0.)));
    // Lying down about its centre it spans z 5..15, then drops by 5.
    CHECK_THAT(drop_after_volume_change(object, before, VolumeChange::reshape)[0], WithinAbs(5., 1e-6));
    CHECK_THAT(volume_world_box(object, 0).min.z(), WithinAbs(0., 1e-6));
}

// ---- Settings on objects and parts ----

TEST_CASE("An object takes object and region settings, a part region settings only", "[PartEdits][orcamcp]")
{
    CHECK_FALSE(setting_key_refusal("wall_loops", SettingsHolder::object));
    CHECK_FALSE(setting_key_refusal("layer_height", SettingsHolder::object));
    CHECK_FALSE(setting_key_refusal("enable_support", SettingsHolder::object));
    CHECK_FALSE(setting_key_refusal("extruder", SettingsHolder::object));
    CHECK_FALSE(setting_key_refusal("wall_loops", SettingsHolder::part));
    CHECK_FALSE(setting_key_refusal("sparse_infill_density", SettingsHolder::part));

    CHECK(mentions(setting_key_refusal("layer_height", SettingsHolder::part), "omit volume_id"));
    CHECK(mentions(setting_key_refusal("extruder", SettingsHolder::part), "set_object_filament"));
    // Printer, filament and whole-print settings are never read per object or part.
    for (const char* key : {"nozzle_diameter", "nozzle_temperature", "skirt_loops"}) {
        INFO(key);
        CHECK(mentions(setting_key_refusal(key, SettingsHolder::object), "apply_config"));
        CHECK(mentions(setting_key_refusal(key, SettingsHolder::part), "apply_config"));
    }
}

TEST_CASE("A negative volume, a blocker and an enforcer take no settings", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model);
    CHECK_FALSE(volume_settings_refusal(0, 1, *object.volumes[1]));
    object.volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    CHECK_FALSE(volume_settings_refusal(0, 1, *object.volumes[1]));
    for (ModelVolumeType type : {ModelVolumeType::NEGATIVE_VOLUME, ModelVolumeType::SUPPORT_BLOCKER, ModelVolumeType::SUPPORT_ENFORCER}) {
        object.volumes[1]->set_type(type);
        CHECK(mentions(volume_settings_refusal(0, 1, *object.volumes[1]), "parts and modifiers"));
    }
}

// ---- assemble_objects and merge_parts ----

TEST_CASE("Assembling takes two or more objects, none of them part of a cut", "[PartEdits][orcamcp]")
{
    Model model;
    two_part_object(model);
    two_part_object(model);
    ModelObject& cut = two_part_object(model);
    CHECK_FALSE(assemble_refusal(model, {0, 1}));
    CHECK(mentions(assemble_refusal(model, {0}), "two or more"));
    CHECK(mentions(assemble_refusal(model, {0, 0}), "twice"));
    CHECK(mentions(assemble_refusal(model, {0, 7}), "7"));
    cut.cut_id.init();
    CHECK(mentions(assemble_refusal(model, {0, 2}), "invalidate_cut_info"));
}

TEST_CASE("Merging parts needs several volumes or several shells", "[PartEdits][orcamcp]")
{
    Model model;
    CHECK(mentions(merge_parts_refusal(0, one_part_object(model, its_make_cube(10., 10., 10.))), "one part"));
    CHECK_FALSE(merge_parts_refusal(1, one_part_object(model, mesh_fixtures::separate_cubes(2))));
    CHECK_FALSE(merge_parts_refusal(2, two_part_object(model)));
}

// ---- rename_object ----

TEST_CASE("A name is refused empty or with a character the object list refuses", "[PartEdits][orcamcp]")
{
    CHECK_FALSE(name_refusal("Bracket left"));
    CHECK(mentions(name_refusal(""), "empty"));
    for (const char* name : {"a/b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "a\\b"}) {
        INFO(name);
        CHECK(mentions(name_refusal(name), "not allowed"));
    }
}

// ---- add_volume's primitive: the object list moves the instance's transform into the volumes ----

// Adding a primitive part or modifier moves instance 0's rotation and scale into every volume
// (ObjectList::apply_object_instance_transfrom_to_all_volumes). Upstream then reset instance 0 alone and
// moved every instance by instance 0's offset: on 2026-09-28 a second copy of a 1.5x object jumped from
// (128, 112) to (256, 256), off the bed, and grew to 2.25x.
TEST_CASE("Moving the instance's transform into the volumes keeps every copy of the object where it was", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model, Vec3d(128, 144, 0), 30.);
    object.instances[0]->set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    ModelInstance* second = object.add_instance();
    second->set_offset(Vec3d(128, 80, 0));
    second->set_rotation(Vec3d(0., 0., Geometry::deg2rad(120.)));
    second->set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    std::vector<BoundingBoxf3> before;
    for (std::size_t i = 0; i < object.instances.size(); ++i)
        before.push_back(object.instance_bounding_box(i));

    Slic3r::GUI::bake_instance_transform_into_volumes(object, /*need_update_assemble_matrix=*/false);

    for (std::size_t i = 0; i < object.instances.size(); ++i) {
        INFO("instance " << i);
        check_vec(object.instance_bounding_box(i).min, before[i].min, 1e-4);
        check_vec(object.instance_bounding_box(i).max, before[i].max, 1e-4);
    }
    // Instance 0 keeps only its offset: its rotation and scale are in the volumes now.
    CHECK(object.instances[0]->get_matrix_no_offset().matrix().isApprox(Transform3d::Identity().matrix(), 1e-9));
    check_vec(object.instances[0]->get_offset(), Vec3d(128, 144, 0));
}

// The list keeps, per object index, which volume each of its rows stands for. Deleting an object left its
// map under its index, so the next object took it over: after deleting a cut object whose connectors were
// hidden, the list's Change Type on the object behind it retyped another volume, or none.
TEST_CASE("Deleting an object from the list drops its row map and moves the later objects' maps down", "[PartEdits][orcamcp]")
{
    std::map<int, std::map<int, int>> maps = {{0, {{0, 0}, {1, 2}}}, {1, {{0, 0}, {1, 1}}}, {3, {{0, 1}}}};
    Slic3r::GUI::erase_object_from_volume_maps(maps, 0);
    CHECK(maps == std::map<int, std::map<int, int>>{{0, {{0, 0}, {1, 1}}}, {2, {{0, 1}}}});
    Slic3r::GUI::erase_object_from_volume_maps(maps, 1); // an object with no map: the later ones still move
    CHECK(maps == std::map<int, std::map<int, int>>{{0, {{0, 0}, {1, 1}}}, {1, {{0, 1}}}});
}

TEST_CASE("Moving an object in the list moves its row map with it", "[PartEdits][orcamcp]")
{
    std::map<int, std::map<int, int>> maps = {{0, {{0, 0}}}, {1, {{0, 1}}}, {2, {{0, 2}}}};
    Slic3r::GUI::move_object_in_volume_maps(maps, 0, 2);
    CHECK(maps == std::map<int, std::map<int, int>>{{0, {{0, 1}}}, {1, {{0, 2}}}, {2, {{0, 0}}}});
    Slic3r::GUI::move_object_in_volume_maps(maps, 2, 0);
    CHECK(maps == std::map<int, std::map<int, int>>{{0, {{0, 0}}}, {1, {{0, 1}}}, {2, {{0, 2}}}});
}

// The Assemble view places each copy by its assemble transform; the bake gave every copy's instance its
// share of the change but only instance 0's assemble transform, so the view applied instance 0's rotation
// and scale twice to the others.
TEST_CASE("Moving the instance's transform into the volumes keeps every copy where the Assemble view shows it", "[PartEdits][orcamcp]")
{
    Model        model;
    ModelObject& object = two_part_object(model, Vec3d(128, 144, 0), 30.);
    object.instances[0]->set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    ModelInstance* second = object.add_instance();
    second->set_offset(Vec3d(128, 80, 0));
    second->set_rotation(Vec3d(0., 0., Geometry::deg2rad(120.)));
    second->set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    for (ModelInstance* instance : object.instances)
        instance->set_assemble_transformation(instance->get_transformation());
    const auto assembled_box = [&object](std::size_t i) {
        BoundingBoxf3 box;
        for (const ModelVolume* volume : object.volumes)
            box.merge(volume->mesh().transformed_bounding_box(object.instances[i]->get_assemble_transformation().get_matrix() *
                                                               volume->get_matrix()));
        return box;
    };
    std::vector<BoundingBoxf3> before;
    for (std::size_t i = 0; i < object.instances.size(); ++i)
        before.push_back(assembled_box(i));

    Slic3r::GUI::bake_instance_transform_into_volumes(object, /*need_update_assemble_matrix=*/true);

    for (std::size_t i = 0; i < object.instances.size(); ++i) {
        INFO("instance " << i);
        check_vec(assembled_box(i).min, before[i].min, 1e-4);
        check_vec(assembled_box(i).max, before[i].max, 1e-4);
    }
}
