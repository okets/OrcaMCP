// src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentSlots.hpp
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "OrcaMCPSliceProgress.hpp"

// The filament slots' decisions apart from the app, so they are tested without it
// (tests/slic3rutils/test_filament_slots.cpp): add_filament_slot and delete_filament_slot (the
// sidebar's "+" button, a slot's Delete and Merge with), and which slot's preset apply_config edits
// (the slot's Edit). The tools run the sidebar's own code (Sidebar::add_custom_filament,
// delete_filament, change_filament) in OrcaMCPFilamentTools.cpp.

namespace Slic3r { class DynamicPrintConfig; }

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// What the slots and the Filament settings are, as the tools read them from the app.
struct FilamentSlotsState
{
    std::vector<bool>        is_mixed;     // one per slot, slot 1 first; physical slots first, mixed ones last
    std::vector<std::string> slot_presets; // each slot's filament preset (PresetBundle::filament_presets)
    // The sidebar offers adding and deleting slots (Sidebar::should_show_SEMM_buttons): a single-extruder
    // multi-material printer, or a Bambu Lab one. Otherwise each extruder holds one filament.
    bool   multi_material = false;
    size_t extruders      = 1;
    bool   gcode_preview  = false; // the plate shows a G-code file (load_model of a .gcode)
    // The one filament preset the Filament settings edit, and its unsaved changes.
    std::string              edited_preset;
    bool                     edited_dirty = false;
    std::vector<std::string> dirty_keys;

    size_t slots() const { return is_mixed.size(); }
    size_t physical_slots() const;
};

// Why add_filament_slot must not add a slot now, or nothing.
std::optional<std::string> add_slot_refusal(const FilamentSlotsState& state, const PipelineState& pipeline, bool ui_job_running);

// A delete_filament_slot call: `slot` (1-based) goes; with `merge_into` (1-based) its objects move there,
// as the slot's Merge with does, else to slot 1, as its Delete does.
struct DeleteSlotRequest
{
    int                slot = 0;
    std::optional<int> merge_into;
    bool               allow_breaking_mix = false;
    // A mixed slot (1-based) made of the slot being deleted, which the delete breaks
    // (PresetBundle::mixed_filaments_using), with its components for the message.
    std::optional<int>        breaks_mix;
    std::vector<unsigned int> mix_components;
};

// Why delete_filament_slot must not delete `request.slot` now, or nothing.
std::optional<std::string> delete_slot_refusal(const FilamentSlotsState& state, const DeleteSlotRequest& request,
                                               const PipelineState& pipeline, bool ui_job_running);

// Whether deleting slot index `index` (0-based) makes the app re-select the Filament settings' preset
// (Sidebar::delete_filament: the first slot, or the only slot using the edited preset), which asks what to
// do with unsaved changes.
bool delete_reselects_edited_preset(const FilamentSlotsState& state, std::size_t index);

// [{from, to}] for the slots (1-based) whose number an add or a delete moves: an added physical slot goes
// before the mixed ones, which move up one; a deleted slot's later slots move down one.
nlohmann::json renumbered_after_add(const FilamentSlotsState& before);
nlohmann::json renumbered_after_delete(std::size_t slots_before, int deleted_slot);

// [{object_id, filaments_before, filaments_after}] for every object that printed with the deleted slot
// `deleted_slot` (1-based): its effective filaments before the delete, and after, numbered as they are
// then. An object on other slots only is renumbered, not changed: `renumbered` says how.
nlohmann::json objects_changed(const std::vector<std::vector<int>>& before, const std::vector<std::vector<int>>& after, int deleted_slot);

// What a slot change's answer says about undo when it renumbered slots or moved objects: the change is no
// undo step (the undo history holds the objects, not the slots), so an undo restores the objects' slot
// numbers from before it while the slots stay as they are now, and objects print on the wrong slots.
// Nothing when nothing was renumbered or moved.
std::optional<std::string> slot_change_undo_warning(const nlohmann::json& renumbered, const nlohmann::json& objects_changed);

// Which slot's preset apply_config's filament settings go to, as the slot's Edit points the Filament
// settings at it.
struct SlotEdit
{
    std::size_t      index = 0; // 0-based
    std::string      preset;
    bool             select_needed = false; // the Filament settings edit another preset now
    std::vector<int> slots;                 // every slot (1-based) using that preset: the change reaches them all
};

// The plan for editing slot `slot` (1-based), or why not: out of range, a mixed slot (no preset of its own),
// unsaved changes to another preset (switching would ask to discard them), or a preset other slots share
// unless `include_sharing_slots`.
std::optional<std::string> slot_edit_refusal(const FilamentSlotsState& state, int slot, bool include_sharing_slots, SlotEdit& plan);

// The slot the Filament settings edit (Sidebar's editing_filament, -1 none) after apply_config pointed them
// at slot `index`, when they edited `previous` before: none when they edited none (the settings window is
// closed), else `index`, as the slot's Edit leaves it -- they show its preset now, and one editing another
// slot would write that preset into it on the next refresh (Sidebar::update_presets).
int editing_slot_after_pointing(int previous, std::size_t index);

// Why filament settings with no filament_slot, or a filament select_preset with no slot, are refused: with
// several physical slots the call does not say which slot's filament it means. Nothing for one slot.
std::optional<std::string> slot_needed_refusal(const FilamentSlotsState& state, const std::string& argument);

// The slots (1-based) whose preset is `preset`.
std::vector<int> slots_using(const FilamentSlotsState& state, const std::string& preset);

// Why a setting that names a filament slot by its number ("extruder" and filament_number_settings) must not
// take `value`, or nothing: 0 is the default, else a slot the project has, and a physical one for support
// and the wipe tower (physical_only_filament_setting), which the slicer reads as they are. The app's lists
// offer nothing else (ConfigManipulation resets the print preset's). Nothing for any other key.
std::optional<std::string> filament_number_refusal(const FilamentSlotsState& state, const std::string& key, int value);

// Whether `key` names a filament slot by its number: "extruder" or one of filament_number_settings.
bool names_filament_slot(const std::string& key);

// The settings in `settings` that name a filament slot they may not (filament_number_refusal), in the
// config's key order, each with why: what set_object_config, set_object_layer_range and apply_config refuse.
std::vector<std::pair<std::string, std::string>> refused_filament_numbers(const FilamentSlotsState& state, const DynamicPrintConfig& settings);

// What such a setting takes, for a refusal's `expected`.
std::string filament_number_expected();

}}} // namespace Slic3r::GUI::OrcaMCP
