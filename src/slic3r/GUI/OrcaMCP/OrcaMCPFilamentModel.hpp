#ifndef slic3r_OrcaMCPFilamentModel_hpp_
#define slic3r_OrcaMCPFilamentModel_hpp_

// Which filament slot each piece of a ModelObject actually prints with, and how to make an
// object-level choice take effect everywhere. Pure libslic3r: nothing here touches wx, so it is
// unit-tested in tests/slic3rutils/test_filament_model.cpp. set_object_filament and the read
// tools (get_scene_info, get_object_info) call these instead of reading ModelObject::config,
// because a volume's own "extruder" beats the object's -- ModelVolume::extruder_id -- and a
// modifier pinned to another slot keeps a plate multi-filament however the object is set.

#include <string>
#include <vector>

#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// One volume's filament as the object list shows it (own) and as the slicer uses it (effective).
struct VolumeFilament
{
    int         volume_id = 0;          // index into ModelObject::volumes
    std::string name;
    std::string type;                   // "part", "modifier", "negative_volume", "support_blocker", "support_enforcer"
    int         own_filament = 0;       // the volume's own "extruder"; 0 when it inherits the object's
    int         effective_filament = 0; // ModelVolume::extruder_id(): what prints
};

// "part", "modifier", ... for the JSON the tools return.
std::string volume_type_name(ModelVolumeType type);

std::vector<VolumeFilament> describe_volume_filaments(const ModelObject& object);

// Every slot the object prints with: parts and modifiers (own or inherited), painted facets, and
// layer ranges that force a slot. Sorted, unique. The per-object half of the rule
// PartPlate::get_extruders uses to decide whether a plate needs a prime tower; the global
// support-filament settings it also consults need the print config and are not included.
std::vector<int> effective_object_filaments(const ModelObject& object);

// How many parts and modifiers carry their own "extruder" -- the number of places where the
// object's slot is not the whole story. 0 means extruder_id and filaments_used agree.
int volume_filament_override_count(const ModelObject& object);

struct ClearedOverride
{
    int         volume_id = 0;
    std::string name;
    std::string type;
    int         was_filament = 0;
};

// Erases the own "extruder" of every part and, when include_modifiers, of every modifier, so the
// object's slot takes effect. Returns what it erased, in volume order. Negative volumes and
// support blockers/enforcers print nothing and are never touched. Does not touch the undo stack.
std::vector<ClearedOverride> clear_volume_filament_overrides(ModelObject& object, bool include_modifiers);

// Slots that parts or modifiers still force which differ from object_filament -- what a
// caller who kept modifiers has left on other slots. Sorted, unique.
std::vector<int> other_volume_filaments(const ModelObject& object, int object_filament);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif // slic3r_OrcaMCPFilamentModel_hpp_
