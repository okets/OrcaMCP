// src/slic3r/GUI/OrcaMCP/OrcaMCPPartEdits.hpp
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

// The decisions of the part and mesh-edit tools (split_object, add_volume, set_volume_type, the
// volume_id forms of the transform, delete, rename and settings tools, assemble_objects, merge_parts,
// invalidate_cut_info), apart from the app, so they are tested without it
// (tests/slic3rutils/test_part_edits.cpp). Each refusal is what the object list's own action refuses,
// or would ask about, said before anything changes, with what to send instead. The tools themselves
// (OrcaMCPPartTools.cpp, and the extended tools in OrcaMCPServer.cpp) run the object list's actions.

namespace Slic3r {
class Model;
class ModelObject;
class ModelVolume;
enum class ModelVolumeType : int;
namespace GUI { namespace OrcaMCP {

// ---- Volumes ----

// The type names the part tools take, as get_object_info reports them: "part", "negative_volume",
// "modifier", "support_blocker", "support_enforcer".
const std::vector<std::string>& volume_type_names();
std::optional<ModelVolumeType> volume_type_from_name(const std::string& name);

// What is wrong with `volume_id` for object `object_id`, or nothing: it must be one of its volumes, as
// get_object_info lists them.
std::optional<std::string> volume_id_error(int object_id, const ModelObject& object, int volume_id);

// How many of the object's volumes are model parts (solid parts, cut connectors included).
std::size_t model_part_count(const ModelObject& object);

// The world box of volume `volume_idx` on instance `instance_idx`, in plate millimetres.
BoundingBoxf3 volume_world_box(const ModelObject& object, std::size_t volume_idx, std::size_t instance_idx = 0);

// One volume as the part tools report it: {volume_id, name, type, own_filament (null: the object's),
// effective_filament, bounding_box {min, max} and position (its centre), in plate mm on instance 0}.
nlohmann::json volume_row_json(const ModelObject& object, std::size_t volume_idx);
// Every volume of the object, in order.
nlohmann::json volume_rows_json(const ModelObject& object);

// ---- Cut objects ----

// The objects cut from the same object as `object_id` (those sharing its cut id), itself included,
// ascending; empty for an object that is not part of a cut. The object list keeps them together: a
// cut object's solid parts and connectors cannot be moved, deleted or assembled until
// invalidate_cut_info breaks that correspondence.
std::vector<int> cut_siblings(const Model& model, int object_id);

// "Object 3 is part of a cut (with objects 3 and 4): <what> ... call invalidate_cut_info ...".
std::string cut_refusal(int object_id, const std::vector<int>& siblings, const std::string& what);

// ---- split_object ----

enum class SplitTarget { objects, parts };

// Whether a split to objects makes more than one object: one per model part of a multi-part object,
// one per shell of a one-part object (ModelObject::split).
bool splits_to_several_objects(const ModelObject& object);

// The volumes a split to objects leaves behind: everything but model parts -- modifiers, negative
// volumes, support blockers and enforcers (ModelObject::split splits model parts only).
std::vector<int> volumes_dropped_by_split_to_objects(const ModelObject& object);

// The volume a split to parts splits: `volume_id`, or the only volume of a one-volume object.
// Nothing when a multi-volume object names none.
std::optional<int> split_volume_of(const ModelObject& object, std::optional<int> volume_id);

// Why split_object must not split object `object_id` so, or nothing.
std::optional<std::string> split_refusal(int object_id, const ModelObject& object, SplitTarget target,
                                         std::optional<int> volume_id, bool keep_height_given);

// ---- set_volume_type ----

// Why volume `volume_id` cannot become `to`, or nothing (the same type is a change of nothing, not a
// refusal): the object's last solid part stays a part, and a text or SVG volume never becomes a
// support blocker or enforcer (ObjectList::set_volume_type).
std::optional<std::string> volume_type_change_refusal(int object_id, const ModelObject& object, int volume_id, ModelVolumeType to);

// ---- delete_object with volume_id ----

// Why volume `volume_id` cannot be deleted, or nothing: the last solid part (delete the object), and a
// cut object's solid part or connector (ObjectList::del_subobject_from_object).
std::optional<std::string> volume_delete_refusal(int object_id, const ModelObject& object, int volume_id,
                                                 const std::vector<int>& cut_siblings);

// The settings that move from the remaining volume to the object when deleting `volume_id` leaves one
// volume (ObjectList::del_subobject_from_object applies its config to the object's).
std::vector<std::string> settings_moving_to_object(const ModelObject& object, int volume_id);

// ---- Transforms of one volume ----

// Why volume `volume_id` cannot be moved, rotated, scaled or mirrored, or nothing: a one-volume object
// has no part rows to transform (transform the object), and a cut object's solid parts and connectors
// cannot be manipulated (ObjectList::part_selection_changed).
std::optional<std::string> volume_transform_refusal(int object_id, const ModelObject& object, int volume_id,
                                                    const std::vector<int>& cut_siblings);

// `world_transform` -- a rotation, scale or mirror in plate axes -- applied to volume `volume_idx`
// about the centre of its world box on instance 0: the object-list selection's own formula for one
// volume in world coordinates (Selection::transform_volume_relative). The volume is shared by every
// instance, so each copy of the object shows the change in its own frame, as in the GUI.
void transform_volume_in_plate_frame(ModelObject& object, std::size_t volume_idx, const Transform3d& world_transform);

// `displacement` (plate mm, on instance 0) applied to volume `volume_idx` (Selection::translate).
void translate_volume_in_plate_frame(ModelObject& object, std::size_t volume_idx, const Vec3d& displacement);

// Each instance's lowest point, from its model parts, as the GUI's drop reads it.
std::vector<double> instances_lowest_z(const ModelObject& object);

// Which drop the GUI does after a volume changed: GLCanvas3D::do_move after a move (a floating
// instance comes down, a sinking one stays), do_rotate / do_scale after a rotation, scale or mirror
// (should_drop_to_bed, with the lowest point before).
enum class VolumeChange { move, reshape };

// The GUI's drop after a volume changed, for every instance with auto_drop; how far each instance
// went down (0 for one that stayed).
std::vector<double> drop_after_volume_change(ModelObject& object, const std::vector<double>& lowest_before, VolumeChange change);

// ---- Settings on objects and parts ----

enum class SettingsHolder { object, part };

// Why `key` cannot be set on an object (its settings tab: object and region settings, and its
// filament, extruder) or a part (region settings only), or nothing. Any other key is stored but never
// read per object or part, so it is refused, with where it belongs.
std::optional<std::string> setting_key_refusal(const std::string& key, SettingsHolder holder);

// Why volume `volume_id` takes no settings, or nothing: the slicer reads settings from parts and
// modifiers only (PrintApply), so a negative volume's, a blocker's or an enforcer's would be inert.
std::optional<std::string> volume_settings_refusal(int object_id, int volume_id, const ModelVolume& volume);

// ---- assemble_objects and merge_parts ----

// Why the objects cannot be assembled into one, or nothing: two or more distinct objects, none of them
// part of a cut (ObjectList::can_merge_to_multipart_object).
std::optional<std::string> assemble_refusal(const Model& model, const std::vector<int>& object_ids);

// Why object `object_id`'s parts cannot be merged into one, or nothing: it needs more than one volume,
// or one volume of several shells (ObjectList::can_mesh_boolean).
std::optional<std::string> merge_parts_refusal(int object_id, const ModelObject& object);

// ---- rename_object ----

// Why `name` cannot be an object's or a part's name, or nothing: empty, or with a character the object
// list refuses (Plater::has_illegal_filename_characters).
std::optional<std::string> name_refusal(const std::string& name);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
