// src/slic3r/GUI/OrcaMCP/OrcaMCPProjectMatch.cpp
#include "OrcaMCPProjectMatch.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPFilamentUtils.hpp"
#include "OrcaMCPPresetConfigUtils.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/PrintHostDialogs.hpp"   // flashforge_normalize_material

#include <algorithm>
#include <cctype>
#include <tuple>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// Letters and digits only, upper case: the shape in which two material names are comparable no
// matter how the printer, the preset author and the config spell them ("PETG-CF" / "petg cf").
std::string alnum_upper(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text)
        if (std::isalnum(ch))
            out.push_back(static_cast<char>(std::toupper(ch)));
    return out;
}

bool same_material_family(const std::string& a, const std::string& b)
{
    const std::string fa = flashforge_normalize_material(a);
    return !fa.empty() && fa == flashforge_normalize_material(b);
}

// The preset name with its vendor prefix and its "@tag" suffix taken off: "Flashforge PETG Pro @FF
// C5P" -> "PETG Pro". What is left is the part that says which filament this is.
std::string preset_core_name(const std::string& name, const std::string& vendor)
{
    std::string core = name;
    if (!vendor.empty() && boost::istarts_with(core, vendor + " "))
        core = core.substr(vendor.size() + 1);
    const size_t at = core.rfind(" @");
    if (at != std::string::npos)
        core = core.substr(0, at);
    return core;
}

// "Slot 1", "Slots 1 and 3", "Slots 1, 2 and 4".
std::string join_slot_numbers(const std::vector<int>& slots)
{
    const std::string head = slots.size() == 1 ? "Slot " : "Slots ";
    std::string       list;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i > 0)
            list += (i + 1 == slots.size()) ? " and " : ", ";
        list += std::to_string(slots[i]);
    }
    return head + list;
}

// "PETG #B17C38", or just "PETG" / just "#B17C38" when only one of them is known.
std::string material_and_color(const std::string& material, const std::string& color)
{
    if (material.empty())
        return color;
    return color.empty() ? material : material + " " + color;
}

// ── The GUI-side readers (main thread) ──────────────────────────────────────────────────────────

std::string config_string_at(const DynamicPrintConfig& config, const char* key, size_t idx = 0)
{
    const auto* opt = config.option<ConfigOptionStrings>(key);
    return (opt != nullptr && idx < opt->values.size()) ? opt->values[idx] : std::string();
}

// Every filament preset the sidebar combo would offer for the selected printer, as the matcher sees
// them. Compatibility and visibility are exactly the combo's own rules, so the matcher can never
// pick something the user could not have picked by hand.
std::vector<MatchCandidate> gather_candidates(const PresetBundle& bundle)
{
    const Preset&     printer          = bundle.printers.get_edited_preset();
    const std::string printer_name     = printer.name;
    const std::string printer_inherits = printer.inherits();

    std::vector<MatchCandidate> candidates;
    for (const Preset& preset : bundle.filaments.get_presets()) {
        if (!preset.is_compatible || !preset.is_visible || preset.is_default)
            continue;

        MatchCandidate candidate;
        candidate.name          = preset.name;
        candidate.filament_type = config_string_at(preset.config, "filament_type");
        candidate.vendor        = config_string_at(preset.config, "filament_vendor");

        const auto* compatible = preset.config.option<ConfigOptionStrings>("compatible_printers");
        if (compatible != nullptr) {
            const auto& names = compatible->values;
            candidate.model_specific =
                std::find(names.begin(), names.end(), printer_name) != names.end() ||
                (!printer_inherits.empty() && std::find(names.begin(), names.end(), printer_inherits) != names.end());
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

std::vector<ProjectSlot> gather_project_slots(PresetBundle& bundle)
{
    const DynamicPrintConfig& project = bundle.project_config;

    std::vector<ProjectSlot> slots;
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
        ProjectSlot slot;
        slot.slot     = int(i + 1);
        slot.preset   = bundle.filament_presets[i];
        slot.color    = config_string_at(project, "filament_colour", i);
        slot.is_mixed = bundle.is_mixed_filament(i);
        slot.color_is_gradient = config_string_at(project, "filament_colour_type", i) == "0";
        // The slot's own preset, not full_config(): the two agree for a physical slot, and this
        // costs nothing on the console's poll cadence.
        if (const Preset* preset = bundle.filaments.find_preset(slot.preset, false))
            slot.type = config_string_at(preset->config, "filament_type");
        slots.push_back(std::move(slot));
    }
    return slots;
}

std::string printer_vendor_name(const PresetBundle& bundle)
{
    const PresetWithVendorProfile printer =
        bundle.printers.get_preset_with_vendor_profile(bundle.printers.get_edited_preset());
    return printer.vendor != nullptr ? printer.vendor->name : std::string();
}

} // namespace

// ── The pure matching core ──────────────────────────────────────────────────────────────────────

std::string normalize_hex_color(const std::string& raw)
{
    std::string digits = boost::trim_copy(raw);
    if (boost::istarts_with(digits, "0x"))
        digits = digits.substr(2);
    else if (!digits.empty() && digits.front() == '#')
        digits.erase(digits.begin());

    for (char& ch : digits) {
        if (!std::isxdigit(static_cast<unsigned char>(ch)))
            return {};
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }

    if (digits.size() == 3)  // #abc -> #AABBCC
        digits = {digits[0], digits[0], digits[1], digits[1], digits[2], digits[2]};
    if (digits.size() == 8)  // an alpha byte: filament_colour is compared and written as RGB
        digits.resize(6);
    if (digits.size() != 6)
        return {};
    return "#" + digits;
}

std::string choose_filament_preset(const std::string&                 material,
                                   const std::vector<MatchCandidate>& candidates,
                                   const std::string&                 printer_vendor)
{
    if (flashforge_normalize_material(material).empty())
        return {};

    const std::string material_key = alnum_upper(material);

    // A vendor-neutral generic profile *of exactly this material*. Compared normalized, like the
    // family check: a printer that says "PET-G" or "PLA+" must still find "Generic PETG @System"
    // rather than skipping the generic tier because the spelling differs.
    const auto is_generic_profile = [&material_key](const MatchCandidate& candidate) {
        return boost::istarts_with(candidate.name, "Generic ") &&
               alnum_upper(preset_core_name(candidate.name, candidate.vendor)) == material_key;
    };

    // Lower sorts first, so the first element of the best key wins.
    using Rank = std::tuple<int, int, int, std::string>;
    const MatchCandidate* best      = nullptr;
    Rank                  best_rank;

    for (const MatchCandidate& candidate : candidates) {
        if (!same_material_family(candidate.filament_type, material))
            continue;

        const bool same_vendor = !printer_vendor.empty() && boost::iequals(candidate.vendor, printer_vendor);
        int        tier        = 4;
        if (candidate.model_specific && same_vendor)
            tier = 0;
        else if (same_vendor)
            tier = 1;
        else if (candidate.model_specific)
            tier = 2;   // built for this machine, whoever wrote it
        else if (is_generic_profile(candidate))
            tier = 3;

        const int  exact_type = alnum_upper(candidate.filament_type) == material_key ? 0 : 1;
        const bool leads      = boost::istarts_with(alnum_upper(preset_core_name(candidate.name, candidate.vendor)),
                                                    material_key);
        const Rank rank{tier, exact_type, leads ? 0 : 1, candidate.name};

        if (best == nullptr || rank < best_rank) {
            best      = &candidate;
            best_rank = rank;
        }
    }
    return best != nullptr ? best->name : std::string();
}

std::vector<SlotPlan> plan_project_match(const std::vector<StationSlot>&    station,
                                         const std::vector<ProjectSlot>&    project,
                                         const std::vector<MatchCandidate>& candidates,
                                         const std::string&                 printer_vendor,
                                         const std::vector<int>&            requested_slots)
{
    std::vector<SlotPlan> plans;

    for (const StationSlot& slot : station) {
        const bool requested = requested_slots.empty()
                                   ? slot.has_filament
                                   : std::find(requested_slots.begin(), requested_slots.end(), slot.slot_id) !=
                                         requested_slots.end();
        if (!requested)
            continue;

        SlotPlan plan;
        plan.slot     = slot.slot_id;
        plan.material = slot.material;
        plan.color    = normalize_hex_color(slot.color);

        const auto found = std::find_if(project.begin(), project.end(),
                                        [&slot](const ProjectSlot& p) { return p.slot == slot.slot_id; });
        if (found == project.end()) {
            // Reported when asked, but never a disagreement: a one-filament project on a machine
            // with four spools loaded is perfectly ordinary, and the console must not nag about
            // something this cannot fix anyway - it does not add filament slots.
            plan.matched = false;
            plan.reason  = "The project has no filament slot " + std::to_string(slot.slot_id) + ".";
            plans.push_back(std::move(plan));
            continue;
        }

        const ProjectSlot& current = *found;
        const std::string  project_color = normalize_hex_color(current.color);
        plan.preset_before = plan.preset_after = current.preset;
        plan.type_before                       = current.type;
        plan.color_before = plan.color_after   = project_color.empty() ? current.color : project_color;

        if (!slot.has_filament) {
            // The station, not the project, is what knows a spool is missing. Clearing the slot
            // would throw away a choice the user may well want back when they load it again.
            plan.reason = "The printer reports this slot as empty, so it was left unchanged.";
            plans.push_back(std::move(plan));
            continue;
        }

        if (current.is_mixed) {
            plan.matched   = false;
            plan.disagrees = true;
            plan.reason    = "Slot " + std::to_string(slot.slot_id) +
                          " is a mixed filament slot; change those with set_mixed_filament.";
            plans.push_back(std::move(plan));
            continue;
        }

        if (!plan.color.empty() && plan.color != project_color) {
            plan.color_after   = plan.color;
            plan.color_changes = true;
            plan.disagrees     = true;
        }

        const std::string family = flashforge_normalize_material(slot.material);
        if (family.empty()) {
            // A loaded slot the printer cannot name. The colour it does report is still worth
            // having, so it is applied and this says why the preset was not.
            plan.matched = false;
            plan.reason  = "The printer reports no material name for this slot, so its filament preset "
                          "was left unchanged.";
            if (plan.color_changes)
                plan.reason += " Its colour takes the printer's " + plan.color + ".";
        } else if (same_material_family(current.type, slot.material)) {
            // Already the right material. Swapping "PETG Transparent" for "PETG Pro" would be
            // homogenizing a deliberate choice, not fixing a disagreement.
            plan.reason = plan.color_changes
                              ? "The printer has " + material_and_color(slot.material, plan.color) +
                                    "; slot " + std::to_string(slot.slot_id) + " takes colour " + plan.color + "."
                              : "Slot " + std::to_string(slot.slot_id) + " already matches the printer's " +
                                    material_and_color(slot.material, plan.color) + ".";
        } else {
            plan.disagrees            = true;
            const std::string chosen  = choose_filament_preset(slot.material, candidates, printer_vendor);
            if (chosen.empty()) {
                plan.matched = false;
                plan.reason  = "No " + slot.material + " filament preset is compatible with the selected printer, "
                              "so slot " + std::to_string(slot.slot_id) + " was left on '" + current.preset + "'.";
                if (plan.color_changes)
                    plan.reason += " Its colour takes the printer's " + plan.color + ".";
            } else {
                plan.preset_after   = chosen;
                plan.preset_changes = chosen != current.preset;
                plan.reason         = "The printer has " + material_and_color(slot.material, plan.color) + "; slot " +
                              std::to_string(slot.slot_id) + " takes '" + chosen + "'" +
                              (plan.color_changes ? " and colour " + plan.color : "") + ".";
            }
        }

        // The station reports one flat colour per slot, so matching a gradient spool loses its
        // second colour. Said out loud, in the plan, so a dry run warns before anything is written.
        if (plan.color_changes && current.color_is_gradient)
            plan.reason += " Slot " + std::to_string(slot.slot_id) +
                           " showed a gradient, which one flat colour from the printer replaces.";

        plans.push_back(std::move(plan));
    }

    return plans;
}

std::string describe_plan_summary(const std::vector<SlotPlan>& plan)
{
    std::vector<int> slots;
    for (const SlotPlan& entry : plan)
        if (entry.disagrees)
            slots.push_back(entry.slot);
    if (slots.empty())
        return {};

    if (slots.size() == 1) {
        const auto entry = std::find_if(plan.begin(), plan.end(),
                                        [&slots](const SlotPlan& p) { return p.slot == slots.front(); });
        const std::string loaded  = material_and_color(entry->material, entry->color);
        const std::string project = material_and_color(entry->type_before, entry->color_before);
        return "Slot " + std::to_string(entry->slot) + " is loaded with " +
               (loaded.empty() ? "filament the printer cannot name" : loaded) + ", but the project has " +
               (project.empty() ? "nothing recorded" : project) + ".";
    }
    return join_slot_numbers(slots) + " are loaded with different filament than the project has.";
}

nlohmann::json slot_plan_json(const SlotPlan& plan)
{
    return {{"slot", plan.slot},
            {"material", plan.material},
            {"color", plan.color},
            {"preset_before", plan.preset_before},
            {"preset_after", plan.preset_after},
            {"type_before", plan.type_before},
            {"color_before", plan.color_before},
            {"color_after", plan.color_after},
            {"changed", plan.changes()},
            {"matched", plan.matched},
            {"reason", plan.reason}};
}

std::vector<FlashforgeApi::MaterialSlot> material_slots_from_json(const nlohmann::json& snapshot)
{
    std::vector<FlashforgeApi::MaterialSlot> slots;
    if (!snapshot.is_object())
        return slots;
    const auto printer = snapshot.find("printer");
    if (printer == snapshot.end() || !printer->is_object())
        return slots;
    const auto station = printer->find("material_station");
    if (station == printer->end() || !station->is_object())
        return slots;
    const auto list = station->find("slots");
    if (list == station->end() || !list->is_array())
        return slots;

    for (const auto& entry : *list) {
        if (!entry.is_object())
            continue;
        FlashforgeApi::MaterialSlot slot;
        slot.slot_id        = entry.value("slot_id", 0);
        slot.has_filament   = entry.value("has_filament", false);
        slot.material_name  = entry.value("material", std::string());
        slot.material_color = entry.value("color", std::string());
        slots.push_back(std::move(slot));
    }
    return slots;
}

// ── The GUI side (main thread) ──────────────────────────────────────────────────────────────────

namespace {

std::vector<StationSlot> to_station_slots(const std::vector<FlashforgeApi::MaterialSlot>& slots)
{
    std::vector<StationSlot> out;
    out.reserve(slots.size());
    for (const FlashforgeApi::MaterialSlot& slot : slots)
        out.push_back(StationSlot{slot.slot_id, slot.has_filament, slot.material_name, slot.material_color});
    return out;
}

nlohmann::json error_json(const std::string& message)
{
    return {{"status", "error"}, {"message", message}};
}

} // namespace

nlohmann::json match_project_to_printer(const std::vector<FlashforgeApi::MaterialSlot>& station,
                                        const std::vector<int>&                        requested_slots,
                                        bool                                           dry_run)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return error_json("Preset bundle not available");
    if (station.empty())
        return error_json("The printer reports no material station, so there is nothing to match against.");

    for (int slot : requested_slots) {
        const bool known = std::any_of(station.begin(), station.end(),
                                       [slot](const FlashforgeApi::MaterialSlot& s) { return s.slot_id == slot; });
        if (!known)
            return error_json("slot " + std::to_string(slot) + " is not a material-station slot on this printer");
    }

    const std::vector<ProjectSlot>    project    = gather_project_slots(*bundle);
    const std::vector<MatchCandidate> candidates = gather_candidates(*bundle);
    const std::string                 vendor     = printer_vendor_name(*bundle);

    std::vector<SlotPlan> plan =
        plan_project_match(to_station_slots(station), project, candidates, vendor, requested_slots);

    std::vector<std::string> info_messages;
    if (!dry_run) {
        // A preset switch can raise a dialog, and neither front door may ever open one: the MCP call
        // would hang on the GUI thread, and the console promised never to show a modal.
        McpDialogSuppressionGuard suppression;

        for (SlotPlan& entry : plan) {
            if (entry.preset_changes) {
                std::string error;
                // The same path select_preset {slot} and the sidebar combo take, so compatibility
                // validation and the sidebar refresh are shared rather than re-implemented.
                if (!OrcaMCPPresetConfigUtils::SelectFilamentSlotPreset(entry.slot, entry.preset_after, error)) {
                    entry.preset_after   = entry.preset_before;
                    entry.preset_changes = false;
                    entry.matched        = false;
                    entry.reason         = error;
                }
            }
            if (entry.color_changes) {
                // Addressed by project_config index. combos_filament() is the *physical* subset and
                // each combo carries its own index, so going through the combo list by position
                // would colour the wrong slot as soon as a mixed slot sits before this one.
                bool        flattened = false;
                std::string error;
                if (!OrcaMCPPresetConfigUtils::WriteProjectFilamentColor(size_t(entry.slot - 1),
                                                                         entry.color_after, flattened, error)) {
                    entry.color_after   = entry.color_before;
                    entry.color_changes = false;
                    entry.matched       = false;
                    entry.reason        = error;
                }
            }
        }
        info_messages = suppression.messages();
    }

    nlohmann::json slots_json = nlohmann::json::array();
    int            changed    = 0;
    bool           partial    = false;
    for (const SlotPlan& entry : plan) {
        slots_json.push_back(slot_plan_json(entry));
        if (entry.changes())
            ++changed;
        if (!entry.matched)
            partial = true;
    }

    nlohmann::json response = {{"status", partial ? "partial" : "success"},
                               {"dry_run", dry_run},
                               {"changed_count", changed},
                               {"slots", std::move(slots_json)},
                               {"filaments", describe_filaments()["filaments"]}};
    if (!info_messages.empty())
        response["info_messages"] = info_messages;
    return response;
}

nlohmann::json project_match_suggestion(const nlohmann::json& snapshot)
{
    const std::vector<FlashforgeApi::MaterialSlot> station = material_slots_from_json(snapshot);
    if (station.empty())
        return nullptr;

    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return nullptr;

    const std::vector<ProjectSlot> project = gather_project_slots(*bundle);
    const std::vector<StationSlot> slots   = to_station_slots(station);

    // The cheap question first, and asked through the same planner rather than a second copy of the
    // rule: `disagrees` is decided before any preset is chosen, so a plan built with no candidates
    // answers "is there anything to suggest?" exactly. Only then is the filament collection walked -
    // which matters on a console that polls once a second through a filament load.
    const auto disagreeing = [](const std::vector<SlotPlan>& p) {
        return std::any_of(p.begin(), p.end(), [](const SlotPlan& entry) { return entry.disagrees; });
    };
    if (!disagreeing(plan_project_match(slots, project, {}, std::string(), {})))
        return nullptr;

    const std::vector<SlotPlan> plan =
        plan_project_match(slots, project, gather_candidates(*bundle), printer_vendor_name(*bundle), {});

    nlohmann::json changing = nlohmann::json::array();
    for (const SlotPlan& entry : plan)
        if (entry.disagrees)
            changing.push_back(slot_plan_json(entry));
    if (changing.empty())
        return nullptr;

    return nlohmann::json{{"summary", describe_plan_summary(plan)}, {"slots", std::move(changing)}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
