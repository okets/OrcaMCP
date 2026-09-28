// src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentSlots.cpp
#include "OrcaMCPFilamentSlots.hpp"
#include "OrcaMCPUiJob.hpp"

#include <algorithm>

#include "libslic3r/libslic3r.h" // MAXIMUM_EXTRUDER_NUMBER

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

using json = nlohmann::json;

std::string slot_range(std::size_t slots)
{
    return slots == 1 ? "the project has 1 filament slot, slot 1" :
                        "the project has " + std::to_string(slots) + " filament slots, 1 to " + std::to_string(slots);
}

// "slots 1 and 3", "slot 2", "slots 1, 2 and 4"
std::string named_slots(const std::vector<int>& slots)
{
    std::string text = slots.size() == 1 ? "slot " : "slots ";
    for (std::size_t i = 0; i < slots.size(); ++i)
        text += (i == 0 ? "" : i + 1 == slots.size() ? " and " : ", ") + std::to_string(slots[i]);
    return text;
}

std::string key_summary(const std::vector<std::string>& keys)
{
    std::string text;
    const std::size_t shown = std::min<std::size_t>(keys.size(), 6);
    for (std::size_t i = 0; i < shown; ++i)
        text += (i == 0 ? "" : ", ") + keys[i];
    if (keys.size() > shown)
        text += " and " + std::to_string(keys.size() - shown) + " more";
    return text;
}

// The unsaved changes the Filament settings hold, for a refusal that would otherwise discard them.
std::string unsaved_changes(const FilamentSlotsState& state)
{
    return "the Filament settings hold unsaved changes to '" + state.edited_preset + "'" +
           (state.dirty_keys.empty() ? std::string() : " (" + key_summary(state.dirty_keys) + ")") +
           ": save_preset {type: filament} keeps them (in that preset, for every slot using it), reset_preset {type: filament} "
           "drops them";
}

// Why a printer's slots cannot be added or deleted: they follow its extruders.
std::string fixed_slots(const FilamentSlotsState& state)
{
    if (state.extruders > 1)
        return "The selected printer has " + std::to_string(state.extruders) +
               " extruders, each holding one filament: its filament slots follow its extruders, so none is added or deleted";
    return "The selected printer prints from one extruder with one filament: its filament slots follow its extruders. A "
           "printer that changes filaments on one extruder has single_extruder_multi_material on (apply_config, type "
           "printer), which gives it slots to add and delete";
}

std::string busy(const PipelineState& pipeline, bool ui_job_running, const std::string& tool, const std::string& what)
{
    if (pipeline_busy(pipeline) != PipelineBusy::idle)
        return pipeline_busy_text(pipeline) + ", so " + what + ": call wait_for_slice, then " + tool + " again";
    if (ui_job_running)
        return ui_job_busy_message(tool);
    return std::string();
}

const std::string GCODE_PREVIEW =
    "The plate shows a G-code file, a preview whose filaments are fixed: the app would close it and start a new project. "
    "Call new_project, or load_project a project, first";

} // namespace

std::size_t FilamentSlotsState::physical_slots() const
{
    return std::size_t(std::count(is_mixed.begin(), is_mixed.end(), false));
}

std::optional<std::string> add_slot_refusal(const FilamentSlotsState& state, const PipelineState& pipeline, bool ui_job_running)
{
    if (const std::string why = busy(pipeline, ui_job_running, "add_filament_slot", "no slot was added"); !why.empty())
        return why;
    if (state.gcode_preview)
        return GCODE_PREVIEW;
    if (!state.multi_material)
        return fixed_slots(state);
    if (state.slots() >= MAXIMUM_EXTRUDER_NUMBER)
        return "The project has " + std::to_string(state.slots()) + " filament slots, the most the app takes";
    return std::nullopt;
}

bool delete_reselects_edited_preset(const FilamentSlotsState& state, std::size_t index)
{
    if (index >= state.slots() || state.is_mixed[index])
        return false;
    if (index == 0)
        return true;
    return state.slot_presets[index] == state.edited_preset &&
           std::count(state.slot_presets.begin(), state.slot_presets.end(), state.edited_preset) == 1;
}

std::optional<std::string> delete_slot_refusal(const FilamentSlotsState& state, const DeleteSlotRequest& request,
                                               const PipelineState& pipeline, bool ui_job_running)
{
    const std::size_t slots = state.slots();
    if (request.slot < 1 || std::size_t(request.slot) > slots)
        return "slot " + std::to_string(request.slot) + " is out of range: " + slot_range(slots);
    if (request.merge_into) {
        if (*request.merge_into < 1 || std::size_t(*request.merge_into) > slots)
            return "merge_into " + std::to_string(*request.merge_into) + " is out of range: " + slot_range(slots);
        if (*request.merge_into == request.slot)
            return "merge_into is the slot being deleted: name another slot, or leave merge_into out to move its objects to slot 1";
    }
    if (const std::string why = busy(pipeline, ui_job_running, "delete_filament_slot", "nothing was deleted"); !why.empty())
        return why;
    if (state.gcode_preview)
        return GCODE_PREVIEW;

    const std::size_t index    = std::size_t(request.slot - 1);
    const bool        physical = !state.is_mixed[index];
    if (physical && !state.multi_material)
        return fixed_slots(state);
    if (physical && state.physical_slots() <= 1)
        return "Slot " + std::to_string(request.slot) + " is the project's only physical filament slot: a project keeps at least one";
    if (request.breaks_mix && !request.allow_breaking_mix) {
        std::vector<int> components(request.mix_components.begin(), request.mix_components.end());
        return "Slot " + std::to_string(request.slot) + " is one of the filaments mixed slot " + std::to_string(*request.breaks_mix) +
               " is made of (" + named_slots(components) + "): " + (request.merge_into ? "merging" : "deleting") +
               " it breaks the mix. Pass allow_breaking_mix: true to go on anyway, or delete_mixed_filament {slot: " +
               std::to_string(*request.breaks_mix) + "} first";
    }
    if (state.edited_dirty && delete_reselects_edited_preset(state, index))
        return "Deleting slot " + std::to_string(request.slot) + " makes the app re-select the Filament settings' preset, and " +
               unsaved_changes(state) + ". Nothing was deleted";
    return std::nullopt;
}

json renumbered_after_add(const FilamentSlotsState& before)
{
    json moves = json::array();
    for (std::size_t i = 0; i < before.slots(); ++i)
        if (before.is_mixed[i])
            moves.push_back({{"from", int(i + 1)}, {"to", int(i + 2)}});
    return moves;
}

json renumbered_after_delete(std::size_t slots_before, int deleted_slot)
{
    json moves = json::array();
    for (int slot = deleted_slot + 1; slot <= int(slots_before); ++slot)
        moves.push_back({{"from", slot}, {"to", slot - 1}});
    return moves;
}

json objects_changed(const std::vector<std::vector<int>>& before, const std::vector<std::vector<int>>& after)
{
    json changed = json::array();
    for (std::size_t i = 0; i < before.size() && i < after.size(); ++i)
        if (before[i] != after[i])
            changed.push_back({{"object_id", int(i)}, {"filaments_before", before[i]}, {"filaments_after", after[i]}});
    return changed;
}

std::vector<int> slots_using(const FilamentSlotsState& state, const std::string& preset)
{
    std::vector<int> slots;
    for (std::size_t i = 0; i < state.slots(); ++i)
        if (!state.is_mixed[i] && state.slot_presets[i] == preset)
            slots.push_back(int(i + 1));
    return slots;
}

std::optional<std::string> slot_edit_refusal(const FilamentSlotsState& state, int slot, bool include_sharing_slots, SlotEdit& plan)
{
    if (slot < 1 || std::size_t(slot) > state.slots())
        return "filament_slot " + std::to_string(slot) + " is out of range: " + slot_range(state.slots());
    plan.index = std::size_t(slot - 1);
    if (state.is_mixed[plan.index])
        return "Slot " + std::to_string(slot) + " is a mixed filament, made of other slots, with no preset of its own: "
               "set_mixed_filament changes it, and apply_config with filament_slot changes one of the slots it is made of";
    plan.preset        = state.slot_presets[plan.index];
    plan.slots         = slots_using(state, plan.preset);
    plan.select_needed = plan.preset != state.edited_preset;
    if (plan.select_needed && state.edited_dirty)
        return "Changing slot " + std::to_string(slot) + "'s filament settings points the Filament settings at its preset '" +
               plan.preset + "', and " + unsaved_changes(state) + ". Nothing was changed";
    if (plan.slots.size() > 1 && !include_sharing_slots) {
        std::vector<int> others;
        for (int other : plan.slots)
            if (other != slot)
                others.push_back(other);
        return "Slot " + std::to_string(slot) + "'s preset '" + plan.preset + "' is " + named_slots(others) +
               "'s too: a change to it reaches every slot using it. Pass include_sharing_slots: true to change them all, or give "
               "slot " + std::to_string(slot) + " a preset of its own first: clone_preset {type: filament, source_name: '" + plan.preset +
               "', new_name: ...}, then select_preset {type: filament, name: <the new name>, slot: " + std::to_string(slot) + "}";
    }
    return std::nullopt;
}

std::optional<std::string> slot_needed_refusal(const FilamentSlotsState& state, const std::string& argument)
{
    if (state.physical_slots() <= 1)
        return std::nullopt;
    const std::vector<int> editing = slots_using(state, state.edited_preset);
    return "The project has " + std::to_string(state.physical_slots()) + " filament slots, and the Filament settings edit one preset, '" +
           state.edited_preset + "'" + (editing.empty() ? ", which no slot uses" : ", " + named_slots(editing) + "'s") +
           ": pass " + argument + " (1 to " + std::to_string(state.physical_slots()) + ") to say which slot's filament you mean";
}

}}} // namespace Slic3r::GUI::OrcaMCP
