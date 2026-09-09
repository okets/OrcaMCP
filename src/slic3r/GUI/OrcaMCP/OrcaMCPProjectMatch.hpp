// src/slic3r/GUI/OrcaMCP/OrcaMCPProjectMatch.hpp
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "slic3r/Utils/FlashforgeApi.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// "Match project to printer": make the project's filament slots say what the printer's material
// station actually holds. A project left pointing at the wrong material blocks send_to_printer (the
// family check rejects the mapping) and paints the plate preview in the wrong colour, and until now
// the only cure was editing four combo boxes by hand.
//
// Station slot N is matched to project filament slot N. The station's own auto-mapping at send time
// is free to pair any tool with any same-family slot; this is the other question -- "what is in the
// machine right now" -- and there the slot numbering is the answer.
//
// Everything above `plan_project_match` is pure: no wx, no preset bundle, no printer. That is what
// tests/slic3rutils/test_project_match.cpp exercises.

// ── The pure matching core ───────────────────────────────────────────────────────────────────────

// One filament preset as the matcher sees it.
struct MatchCandidate
{
    std::string name;            // "Flashforge PETG Pro @FF C5P"
    std::string filament_type;   // the preset's own filament_type, e.g. "PETG"
    std::string vendor;          // filament_vendor, "" when the preset carries none
    // Its compatible_printers names the active printer preset (or the system preset it inherits
    // from) outright, rather than being compatible through a broad condition. This is what the
    // "@FF C5P" naming convention encodes; see choose_filament_preset for why it is derived rather
    // than parsed out of the name.
    bool model_specific{false};
};

// One project filament slot as it stands now.
struct ProjectSlot
{
    int         slot{0};   // 1-based
    std::string preset;
    std::string type;      // the slot preset's filament_type
    std::string color;     // "#RRGGBB"
    bool        is_mixed{false};
    // filament_colour_type "0": the slot shows a gradient/multi-colour spool. The printer reports
    // one flat colour per slot, so matching replaces it -- and says so rather than quietly
    // discarding the second colour.
    bool color_is_gradient{false};
};

// One material-station slot, reduced to what matching needs.
struct StationSlot
{
    int         slot_id{0};
    bool        has_filament{false};
    std::string material;   // the printer's materialName, e.g. "PETG"
    std::string color;      // the printer's materialColor, e.g. "#B17C38"
};

// What one slot would become. `preset_after`/`color_after` always hold the value the slot ends on,
// equal to the "before" when nothing changes, so a caller never has to guess.
struct SlotPlan
{
    int         slot{0};
    std::string material;        // what the printer reports for this slot
    std::string color;           // the printer's colour, normalized, "" when it reports none
    std::string preset_before;
    std::string preset_after;
    std::string type_before;     // the project slot's own filament_type, for the console's one-liner
    std::string color_before;
    std::string color_after;
    bool        preset_changes{false};
    bool        color_changes{false};
    // The printer and the project do not say the same thing about this slot. Not the same as
    // `changes()`: a disagreement nothing compatible can fix still disagrees, and the console still
    // has to say so rather than quietly showing a project that will be refused at send time.
    bool        disagrees{false};
    // False when the printer's material could not be turned into a preset choice (no material name
    // reported, or nothing compatible of that family). `reason` says which.
    bool        matched{true};
    std::string reason;

    bool changes() const { return preset_changes || color_changes; }
};

// "#RRGGBB" upper case, or "" when `raw` is not a colour this can make sense of. Accepts a leading
// "#" or "0x" or neither, 3/6/8 hex digits (an alpha byte is dropped: filament_colour is compared
// and written as RGB).
std::string normalize_hex_color(const std::string& raw);

// The preset to put in a slot the printer reports as `material`, or "" when nothing fits.
//
// Preference, in order:
//   0. a same-vendor profile built for this exact printer model  ("Flashforge PETG Pro @FF C5P")
//   1. any other compatible same-vendor profile of that material ("Flashforge PETG Pro @FF AD5X"
//      would never be compatible, but a vendor's generic-machine profile is)
//   2. any *other* profile built for this exact printer model -- the bundle ships model-specific
//      profiles with no filament_vendor at all ("Generic BVOH @FF C5P"), and one of those is tuned
//      for this machine where an alphabetically-earlier stranger is not
//   3. a vendor-neutral "Generic <MATERIAL>" profile
//   4. anything else compatible of that material family -- better than leaving the slot on a
//      material the machine is not holding, which is the bug this whole feature exists to fix.
//
// The model tag ("FF C5P") is a naming convention with no machine-readable source: no printer preset
// field carries it, and the vendor bundle does not declare it. So tier 0 is derived structurally
// instead -- `model_specific` is "this preset's compatible_printers names this printer" -- which is
// exactly what the tag means and works for every vendor rather than only Flashforge's.
//
// Within a tier: an exact filament_type match before a merely same-family one (so an ABS slot takes
// an ABS profile rather than the ASA one that normalizes to the same family), then a preset whose
// name begins with the material once the vendor prefix and the "@tag" suffix are stripped ("PETG
// Pro" over "HS PETG"), then alphabetically so the choice is stable.
std::string choose_filament_preset(const std::string&                 material,
                                   const std::vector<MatchCandidate>& candidates,
                                   const std::string&                 printer_vendor);

// The plan for every requested slot, in station order. `requested_slots` empty means "every loaded
// slot"; a slot named explicitly is reported even when it is empty or has nothing to change, so a
// caller who asked about it gets an answer rather than silence.
//
// A slot the printer reports as empty is never touched: the station, not the project, is the thing
// that knows a spool is missing, and clearing the project would throw away a deliberate choice.
// A loaded slot whose project preset is already of the right material keeps that preset -- this
// fixes disagreement, it does not homogenize a project onto one profile per material.
std::vector<SlotPlan> plan_project_match(const std::vector<StationSlot>&    station,
                                         const std::vector<ProjectSlot>&    project,
                                         const std::vector<MatchCandidate>& candidates,
                                         const std::string&                 printer_vendor,
                                         const std::vector<int>&            requested_slots);

// One line naming what disagrees, for the console's suggestion. "" when nothing does.
std::string describe_plan_summary(const std::vector<SlotPlan>& plan);

// The per-slot JSON both front doors report: the contract's
// {slot, material, color, preset_before, preset_after, color_before, color_after, changed, reason},
// plus `type_before` (the project's own material, which the console names in its suggestion) and
// `matched`. `changed` is "was changed", or "would change" under dry_run.
nlohmann::json slot_plan_json(const SlotPlan& plan);

// ── The GUI side (main thread) ───────────────────────────────────────────────────────────────────

// Reads the project and the preset bundle, plans against `station`, and -- unless `dry_run` --
// applies it: the preset through the same OrcaMCPPresetConfigUtils::SelectFilamentSlotPreset the
// sidebar combo and `select_preset {slot}` go through, the colour into project_config's
// filament_colour. Holds a McpDialogSuppressionGuard around the apply, because a preset switch can
// raise a dialog and neither front door may ever open one.
//
// Returns the whole response body both front doors return:
//   {"status": "success"|"partial"|"error", "dry_run": bool, "slots": [...], "changed_count": N,
//    "filaments": [...], "info_messages": [...]}
// It never throws and never returns an empty object.
nlohmann::json match_project_to_printer(const std::vector<FlashforgeApi::MaterialSlot>& station,
                                        const std::vector<int>&                        requested_slots,
                                        bool                                           dry_run);

// The console page's suggestion, computed from a status snapshot (the shape `status_to_json` builds,
// wrapped in {"printer": ...}). Returns null when the snapshot has no material station, or when the
// project already agrees with it -- the page shows the suggestion exactly while this is non-null.
//   {"summary": "...", "slots": [ ...the changing SlotPlans... ]}
// Main thread.
nlohmann::json project_match_suggestion(const nlohmann::json& snapshot);

// The material-station slots out of that same snapshot shape. Any thread; pure.
std::vector<FlashforgeApi::MaterialSlot> material_slots_from_json(const nlohmann::json& snapshot);

}}} // namespace Slic3r::GUI::OrcaMCP
