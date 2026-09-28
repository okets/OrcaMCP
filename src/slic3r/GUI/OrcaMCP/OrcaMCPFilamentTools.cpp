#include <cctype>
#include <regex>
// src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPFilamentUtils.hpp"
#include "OrcaMCPPresetConfigUtils.hpp"
#include "OrcaMCPColorRecipe.hpp"
#include "OrcaMCPFilamentModel.hpp"
#include "OrcaMCPFilamentSlots.hpp"
#include "OrcaMCPUiJob.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"
#include "libslic3r/FilamentMixer.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

nlohmann::json with_filaments(nlohmann::json result)
{
    result["filaments"] = describe_filaments()["filaments"];
    result["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
    return result;
}

// One decimal place, e.g. "54.0" -- so an out-of-gamut message never disagrees with the
// "delta_e" field by up to 0.5 the way rounding to an int would.
std::string format_one_decimal(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return std::string(buf);
}

PipelineState current_pipeline()
{
    Plater& plater = *wxGetApp().plater();
    return pipeline_state(plater, plater.get_partplate_list().get_plate_count());
}

bool ui_job_running() { return !wxGetApp().plater()->get_ui_job_worker().is_idle(); }

// Every object's effective filaments (painted facets, parts and modifiers, layer ranges), by object_id.
std::vector<std::vector<int>> objects_filaments()
{
    std::vector<std::vector<int>> filaments;
    for (const ModelObject* object : wxGetApp().plater()->model().objects)
        filaments.push_back(effective_object_filaments(*object));
    return filaments;
}

// What the plates were sliced with changed for every plate: none keeps its result.
void after_slot_count_change()
{
    wxGetApp().sidebar().update_filaments_counter();
    wxGetApp().plater()->get_partplate_list().invalid_all_slice_result();
}

// add_filament_slot, on the main thread: the sidebar's "+" button, with the colour and preset given.
nlohmann::json add_filament_slot(const std::optional<std::string>& color, const std::string& preset)
{
    McpDialogSuppressionGuard   guard;
    const FilamentSlotsState    before = filament_slots_state();
    if (const auto refusal = add_slot_refusal(before, current_pipeline(), ui_job_running()))
        return error_response(*refusal);
    if (!preset.empty())
        if (const std::string error = OrcaMCPPresetConfigUtils::FilamentSlotPresetError(preset); !error.empty())
            return error_response(error);

    // The "+" button takes the sidebar's next colour; so does a call that names none.
    const wxColour colour = color ? wxColour(from_u8(*color)) : Plater::get_next_color_for_filament();
    wxGetApp().sidebar().add_custom_filament(colour, preset);
    const FilamentSlotsState after = filament_slots_state();
    if (after.slots() != before.slots() + 1)
        return guard.report(error_response("The app did not add a filament slot."));
    after_slot_count_change();

    const size_t slot = before.physical_slots() + 1; // a physical slot goes before the mixed ones
    nlohmann::json answer = {{"status", "success"},
                             {"slot", int(slot)},
                             {"preset", after.slot_presets[slot - 1]},
                             {"color", colour.GetAsString(wxC2S_HTML_SYNTAX).ToStdString()},
                             {"renumbered", renumbered_after_add(before)}};
    return guard.report(with_filaments(std::move(answer)));
}

// delete_filament_slot, on the main thread: a slot's Delete, or with merge_into its Merge with.
nlohmann::json delete_filament_slot(DeleteSlotRequest request)
{
    McpDialogSuppressionGuard guard;
    const FilamentSlotsState  before = filament_slots_state();
    PresetBundle&             bundle = *wxGetApp().preset_bundle;
    if (request.slot >= 1 && size_t(request.slot) <= before.slots()) {
        // A mix made of the slot breaks when it goes: the one it is merged into first, which the app asks about.
        const size_t              from  = size_t(request.slot - 1);
        const std::vector<size_t> mixes = bundle.mixed_filaments_using(from);
        if (!mixes.empty()) {
            size_t mix = mixes.front();
            if (request.merge_into && *request.merge_into >= 1 && bundle.merge_breaks_mixed_filament(from, size_t(*request.merge_into - 1)))
                mix = size_t(*request.merge_into - 1);
            request.breaks_mix = int(mix + 1);
            if (const auto* components = bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components"))
                request.mix_components = parse_mixed_components(components->get_at(mix));
        }
    }
    if (const auto refusal = delete_slot_refusal(before, request, current_pipeline(), ui_job_running()))
        return error_response(*refusal);

    const std::vector<std::vector<int>> filaments_before = objects_filaments();
    Sidebar&                            sidebar          = wxGetApp().sidebar();
    if (request.merge_into) {
        if (request.breaks_mix && request.allow_breaking_mix)
            guard.answer_prompt(MCP_PROMPT_MERGE_INTO_MIX, wxID_OK, "allow_breaking_mix was true");
        sidebar.change_filament(size_t(request.slot - 1), size_t(*request.merge_into - 1));
    } else
        sidebar.delete_filament(size_t(request.slot - 1), -1);
    if (filament_slots_state().slots() != before.slots() - 1)
        return guard.report(error_response("The app did not delete slot " + std::to_string(request.slot) + "."));
    after_slot_count_change();

    nlohmann::json answer = {{"status", "success"},
                             {"deleted_slot", request.slot},
                             {"renumbered", renumbered_after_delete(before.slots(), request.slot)},
                             {"objects_changed", objects_changed(filaments_before, objects_filaments(), request.slot)}};
    if (request.merge_into) {
        answer["merged_into"]     = *request.merge_into;
        // The slot it merged into, as numbered now.
        answer["merged_into_now"] = *request.merge_into > request.slot ? *request.merge_into - 1 : *request.merge_into;
    }
    return guard.report(with_filaments(std::move(answer)));
}

} // namespace

void OrcaMCPServer::register_filament_tools()
{
    register_tool({
        "get_filaments",
        ToolCategory::FilamentsColour,
        "Filament slots, mixed slots, extruders",
        "List all filament slots (physical and mixed/virtual), extruder count, and filament-to-extruder map.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](const nlohmann::json&) -> nlohmann::json {
            return run_on_main_thread([]() -> nlohmann::json {
                nlohmann::json r = describe_filaments();
                r["status"] = "success";
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });

    register_tool({
        "add_filament_slot",
        ToolCategory::FilamentsColour,
        "Add a physical filament slot",
        "Add a physical filament slot, as the sidebar's + button does: after the last physical slot (mixed slots move up "
        "one, and so do objects on them), with the preset the + button gives it (the last slot's) unless preset names another, and the "
        "sidebar's next colour unless color gives one. Only a printer that changes filaments on one extruder, or a Bambu Lab "
        "printer, takes more slots: a printer with one filament per extruder has as many slots as extruders. Answers slot "
        "(the new slot's number), preset, color, renumbered ({from, to} for moved mixed slots) and filaments. Every plate "
        "loses its slice; no undo step, as in the app (undo does not bring a slot back or take one away). Then give it "
        "objects with set_object_filament or paint_object, or another preset with select_preset {type: filament, slot}. "
        "Refused while slicing or while a job runs.",
        {
            {"type", "object"},
            {"properties", {
                {"color", {{"type", "string"}, {"description", "The slot's colour on the plate, #RRGGBB. Default: the sidebar's next colour."}}},
                {"preset", {{"type", "string"}, {"description", "A filament preset for the slot (get_presets type filament). Default: the last slot's, as the + button gives it."}}}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::optional<std::string> color;
            if (params.contains("color")) {
                if (!params["color"].is_string() || !is_hex_color(params["color"].get<std::string>()))
                    return error_response("color must be \"#RRGGBB\"");
                color = params["color"].get<std::string>();
            }
            std::string preset;
            if (params.contains("preset")) {
                if (!params["preset"].is_string() || params["preset"].get<std::string>().empty())
                    return error_response("preset must be a filament preset's name; leave it out for the last physical slot's");
                preset = params["preset"].get<std::string>();
            }
            return run_on_main_thread([color, preset]() { return add_filament_slot(color, preset); });
        }
    });

    register_tool({
        "delete_filament_slot",
        ToolCategory::FilamentsColour,
        "Delete a filament slot, or merge it",
        "Delete a filament slot, physical or mixed, as its Delete in the sidebar does: its objects, parts and painted facets "
        "move to slot 1, and every later slot moves down one (objects and paint with it). With merge_into, as its Merge with "
        "does: its objects and paint move to that slot instead. Answers deleted_slot, renumbered ({from, to}), objects_changed "
        "(each object that printed with the slot: its filaments before, and after, as numbered then), merged_into / "
        "merged_into_now, and filaments. Every plate "
        "loses its slice; no undo step, as in the app. Refused: the last physical slot; a physical slot of a printer with "
        "one filament per extruder; a slot a mixed slot is made of, which breaks the mix, unless allow_breaking_mix; deleting slot 1 "
        "(or the only slot using the Filament settings' preset) while those settings have unsaved changes, which the app "
        "would re-select (save_preset or reset_preset first); while slicing or while a job runs. delete_mixed_filament "
        "deletes a mixed slot too.",
        {
            {"type", "object"},
            {"properties", {
                {"slot", {{"type", "integer"}, {"description", "The slot to delete, 1-based"}}},
                {"merge_into", {{"type", "integer"}, {"description", "Move its objects and paint to this slot (1-based, numbered as before the delete) instead of slot 1"}}},
                {"allow_breaking_mix", {{"type", "boolean"}, {"description", "Delete or merge a slot a mixed slot is made of anyway; the mix loses that component. Default false."}}}
            }},
            {"required", {"slot"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            DeleteSlotRequest request;
            if (!parse_integer_param(params["slot"], request.slot))
                return error_response("slot must be an integer, 1-based");
            if (params.contains("merge_into")) {
                int merge_into = 0;
                if (!parse_integer_param(params["merge_into"], merge_into))
                    return error_response("merge_into must be an integer, 1-based; leave it out to move the slot's objects to slot 1");
                request.merge_into = merge_into;
            }
            if (params.contains("allow_breaking_mix") && !parse_boolean_param(params["allow_breaking_mix"], request.allow_breaking_mix))
                return error_response("allow_breaking_mix must be a boolean");
            return run_on_main_thread([request]() { return delete_filament_slot(request); });
        }
    });

    register_tool({
        "set_mixed_filament",
        ToolCategory::FilamentsColour,
        "Create or edit a mixed filament slot",
        "Create or edit a mixed (virtual) filament slot that alternates two or three physical "
        "filaments by layer ratio. Requires a multi-filament printer profile.",
        {
            {"type", "object"},
            {"properties", {
                {"components", {
                    {"type", "array"},
                    {"items", {{"type", "integer"}}},
                    {"description", "2-3 physical filament slots, 1-based"}
                }},
                {"ratios", {
                    {"type", "array"},
                    {"items", {{"type", "integer"}}},
                    {"description", "Percent per component, must sum to 100"}
                }},
                {"slot", {
                    {"type", "integer"},
                    {"description", "Existing mixed slot to edit (1-based). Omit to create."}
                }},
                {"gradient", {
                    {"type", "boolean"},
                    {"description", "Z gradient between the two components"}
                }},
                {"gradient_direction", {
                    {"type", "string"},
                    {"enum", {"a_to_b", "b_to_a"}}
                }},
                {"per_part_gradient", {{"type", "boolean"}}}
            }},
            {"required", {"components", "ratios"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            MixedFilamentResult req;
            std::string err;
            if (!mixed_result_from_params(params, req, err))
                return {{"status", "error"}, {"message", err}};
            // Omitted means "create a new slot"; an explicit slot addresses an existing one. A
            // caller that passes 0 or a negative slot means the second and gets the first, so say so.
            const int slot = params.value("slot", -1);
            if (params.contains("slot") && slot < 1)
                return nlohmann::json{{"status", "error"},
                                      {"message", "slot must be 1-based; omit it to create a new mixed slot"}};

            return run_on_main_thread([req, slot]() -> nlohmann::json {
                std::string error;
                int edit_idx = -1;
                if (slot > 0) {
                    edit_idx = slot_to_config_index(slot, error);
                    if (edit_idx < 0) return nlohmann::json{{"status", "error"}, {"message", error}};
                    if (!wxGetApp().preset_bundle->is_mixed_filament(edit_idx))
                        return nlohmann::json{{"status", "error"}, {"message", "slot is not a mixed filament"}};
                }
                // The keys Sidebar::apply_mixed_filament writes a slot's recipe to.
                const WrittenValues written(wxGetApp().preset_bundle->project_config,
                                            {"filament_is_mixed", "filament_colour", "filament_multi_colour",
                                             "filament_mixed_components", "filament_mixed_sublayer_ratios",
                                             "filament_mixed_gradient", "filament_mixed_gradient_range",
                                             "filament_mixed_gradient_curve", "filament_mixed_gradient_per_part"});
                int idx = wxGetApp().sidebar().apply_mixed_filament(req, edit_idx, error);
                if (idx < 0) return nlohmann::json{{"status", "error"}, {"message", error}};
                // A filament slot changed for every plate: none keeps a result made without it. A slot
                // given the recipe it already had changed nothing, and every plate keeps its slice.
                const bool changed = edit_idx < 0 || written.changed_in(wxGetApp().preset_bundle->project_config);
                if (changed)
                    wxGetApp().plater()->get_partplate_list().invalid_all_slice_result();
                return with_filaments({{"status", "success"}, {"slot", idx + 1}, {"changed", changed}});
            });
        }
    });

    register_tool({
        "delete_mixed_filament",
        ToolCategory::FilamentsColour,
        "Delete a mixed filament slot",
        "Delete a mixed (virtual) filament slot.",
        {
            {"type", "object"},
            {"properties", {
                {"slot", {{"type", "integer"}, {"description", "Mixed slot, 1-based"}}}
            }},
            {"required", {"slot"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const int slot = params["slot"];
            return run_on_main_thread([slot]() -> nlohmann::json {
                std::string error;
                int idx = slot_to_config_index(slot, error);
                if (idx < 0) return nlohmann::json{{"status", "error"}, {"message", error}};

                auto mixed = wxGetApp().plater()->mixed_filament_config_indices();
                auto it = std::find(mixed.begin(), mixed.end(), size_t(idx));
                if (it == mixed.end())
                    return nlohmann::json{{"status", "error"}, {"message", "slot is not a mixed filament"}};

                const size_t slots_before = wxGetApp().preset_bundle->filament_presets.size();
                wxGetApp().sidebar().delete_mixed_filament_at(size_t(it - mixed.begin()));
                // The sidebar returns without a word when it deletes nothing: say so rather than succeed.
                if (wxGetApp().preset_bundle->filament_presets.size() != slots_before - 1)
                    return nlohmann::json{{"status", "error"}, {"message", "The app did not delete mixed slot " + std::to_string(slot) + "."}};
                // A filament slot changed for every plate: none keeps a result made with it.
                wxGetApp().plater()->get_partplate_list().invalid_all_slice_result();
                return with_filaments({{"status", "success"}});
            });
        }
    });

    // Written after an agent set 45 objects to slot 3, read extruder_id == 3 back on every one,
    // and reported success while every object still printed in slot 1: each had modifiers pinned
    // to slot 1, a volume's own slot beats the object's, and no read tool showed the modifiers.
    // So the whole-object form now clears the overrides, and the response says what happened.
    register_tool({
        "set_object_filament",
        ToolCategory::FilamentsColour,
        "Assign a filament to an object or volume",
        "Assign a filament slot (physical or mixed) to a whole object, or to one volume of it "
        "(volume_id). A volume's own slot beats the object's, so the whole-object form also clears "
        "the own slot of every part and, unless include_modifiers=false, of every modifier -- "
        "otherwise the change is invisible and the plate keeps its prime tower. Read the response, "
        "not just status: effective_filaments is every slot the object still prints with (volumes, "
        "painted facets, layer ranges); cleared_overrides lists each volume that lost its own slot "
        "and what it held; other_slots lists slots its volumes still force. The object is on one "
        "filament only when effective_filaments has one entry. get_object_info's `volumes` shows the "
        "same per volume; get_scene_info's filaments_used shows it per object. changed: false when "
        "the call set what was already there: no undo step, and the plates keep their slice. To colour "
        "only part of a surface, paint it: paint_object mode color.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}}},
                {"filament", {{"type", "integer"}, {"description", "Filament slot, 1-based"}}},
                {"volume_id", {
                    {"type", "integer"},
                    {"minimum", -1},
                    {"description", "Volume index within the object (0-based, as get_object_info's `volumes` "
                                    "lists them; parts and modifiers only). Omit, or pass -1, for the whole object."}
                }},
                {"include_modifiers", {
                    {"type", "boolean"},
                    {"description", "Whole-object form only. true (default): modifiers lose their own slot too, "
                                    "so the object prints in one filament. false: keep them, as the GUI's object "
                                    "row does -- a modifier pinned to another slot is how a two-colour inlay is "
                                    "made; other_slots then lists what they still force."}
                }}
            }},
            {"required", {"object_id", "filament"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const int object_id = params["object_id"];
            const int filament  = params["filament"];
            const int volume_id = params.value("volume_id", -1);
            bool include_modifiers = true;
            if (params.contains("include_modifiers") && !parse_boolean_param(params["include_modifiers"], include_modifiers))
                return nlohmann::json{{"status", "error"}, {"message", "include_modifiers must be a boolean"}};

            return run_on_main_thread([object_id, filament, volume_id, include_modifiers]() -> nlohmann::json {
                std::string        error;
                FilamentAssignment done;
                if (!set_object_filament(object_id, volume_id, filament, include_modifiers, done, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                nlohmann::json cleared = nlohmann::json::array();
                for (const ClearedOverride& c : done.cleared)
                    cleared.push_back({{"volume_id", c.volume_id}, {"name", c.name}, {"type", c.type}, {"was_filament", c.was_filament}});

                nlohmann::json r = {{"status", "success"}, {"object_id", object_id}, {"filament", filament}};
                r["volume_id"]           = volume_id < 0 ? nlohmann::json(nullptr) : nlohmann::json(volume_id);
                r["cleared_overrides"]   = cleared;
                r["effective_filaments"] = done.effective_filaments;
                r["other_slots"]         = done.other_slots;
                r["single_filament"]     = done.effective_filaments.size() == 1;
                r["changed"]             = done.changed;
                r["active_warnings"]     = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });

    // set_filament_color - the plate's per-slot colour. apply_config edits the filament *preset*,
    // which is not what the sidebar swatches, the 3D volumes and the render palette show; those read
    // project_config's filament_colour. This writes it through the shared WriteProjectFilamentColor
    // and then announces it the way the sidebar's colour picker does (EVT_FILAMENT_COLOR_CHANGED),
    // so every consumer refreshes. Found when "paint it white" had no white slot to paint with.
    register_tool({
        "set_filament_color",
        ToolCategory::FilamentsColour,
        "Set a slot's color as the plate shows it",
        "Set the colour of a filament slot as the plate shows it (sidebar swatch, 3D view, flush "
        "calculation). #RRGGBB or #RRGGBBAA. This is the project's per-slot colour, not the preset's. It is "
        "saved for the selected printer, so switching to another printer and back brings it back. A "
        "gradient slot becomes this one flat colour, and the response then has flattened: true.",
        {
            {"type", "object"},
            {"properties", {
                {"slot", {{"type", "integer"}, {"minimum", 1}, {"description", "Filament slot, 1-based"}}},
                {"color", {{"type", "string"}, {"description", "#RRGGBB or #RRGGBBAA"}}}
            }},
            {"required", {"slot", "color"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            if (!params.contains("slot") || !params["slot"].is_number_integer())
                return nlohmann::json{{"status", "error"}, {"message", "slot must be an integer"}};
            if (!params.contains("color") || !params["color"].is_string())
                return nlohmann::json{{"status", "error"}, {"message", "color must be a string like #FFFFFF"}};
            const int   slot  = params["slot"];
            std::string color = params["color"].get<std::string>();
            static const std::regex hex(R"(^#[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?$)");
            if (!std::regex_match(color, hex))
                return nlohmann::json{{"status", "error"}, {"message", "color must be #RRGGBB or #RRGGBBAA"}};
            for (char& c : color) c = char(std::toupper(static_cast<unsigned char>(c)));
            return run_on_main_thread([slot, color]() -> nlohmann::json {
                const DynamicPrintConfig& project_config = wxGetApp().preset_bundle->project_config;
                const auto* head = project_config.option<ConfigOptionStrings>("filament_colour");
                if (head == nullptr || slot < 1 || size_t(slot) > head->values.size())
                    return nlohmann::json{{"status", "error"},
                                          {"message", "slot out of range; the project has " + std::to_string(head ? head->values.size() : 0) + " filament slot(s)"}};
                const size_t      idx = size_t(slot - 1);
                const std::string was = head->values[idx];

                // The shared write: all three colour keys (a gradient slot becomes this one flat
                // colour, and the response says so), the plater's refresh, the project-dirty flag,
                // the mixed swatches, the per-printer snapshot and the background process.
                bool        flattened = false;
                std::string error;
                if (!OrcaMCPPresetConfigUtils::WriteProjectFilamentColor(idx, color, flattened, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};
                OrcaMCPPresetConfigUtils::NotifyFilamentColorChanged(idx);

                nlohmann::json r = {{"status", "success"}, {"slot", slot}, {"color", color}, {"previous_color", was}};
                if (flattened)
                    r["flattened"] = true;
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });

    register_tool({
        "get_flush_volumes",
        ToolCategory::FilamentsColour,
        "Per-extruder flush-volume matrices",
        "Get the per-extruder flush-volume matrices (mL to purge switching from filament X to Y) "
        "and the flush multiplier used to scale them.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](const nlohmann::json&) -> nlohmann::json {
            return run_on_main_thread([]() -> nlohmann::json {
                nlohmann::json r = describe_flush_volumes();
                r["status"] = "success";
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });

    register_tool({
        "set_flush_volumes",
        ToolCategory::FilamentsColour,
        "Overwrite one extruder's flush matrix",
        "Overwrite one extruder's full flush-volume matrix (NxN, N = physical filament count) "
        "and, optionally, that extruder's flush multiplier.",
        {
            {"type", "object"},
            {"properties", {
                {"matrix", {
                    {"type", "array"},
                    {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                    {"description", "NxN matrix, row = from-filament, column = to-filament, mL"}
                }},
                {"extruder", {{"type", "integer"}, {"description", "0-based extruder, default 0"}}},
                {"flush_multiplier", {{"type", "number"}, {"description", "Optional scale factor for this extruder"}}}
            }},
            {"required", {"matrix"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const nlohmann::json matrix = params.at("matrix");
            const int extruder = params.value("extruder", 0);
            std::optional<double> flush_multiplier;
            if (params.contains("flush_multiplier")) {
                double m = 0.0;
                if (!parse_double_param(params["flush_multiplier"], m))
                    return nlohmann::json{{"status", "error"}, {"message", "flush_multiplier must be a finite number"}};
                flush_multiplier = m;
            }

            return run_on_main_thread([matrix, extruder, flush_multiplier]() -> nlohmann::json {
                std::string error;
                if (!set_flush_volumes(matrix, extruder, flush_multiplier, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                nlohmann::json r = {{"status", "success"}};
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });

    register_tool({
        "auto_calc_flush_volumes",
        ToolCategory::FilamentsColour,
        "Recalculate flush volumes from colors",
        "Automatically recalculate flush-volume matrices for every physical filament and "
        "extruder from filament color/type compatibility.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](const nlohmann::json&) -> nlohmann::json {
            return run_on_main_thread([]() -> nlohmann::json {
                auto_calc_flush_volumes();
                nlohmann::json r = {{"status", "success"}};
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });

    register_tool({
        "get_toolchanger_config",
        ToolCategory::FilamentsColour,
        "Toolchange and multi-extruder settings",
        "Get toolchanger / multi-extruder settings (retraction on toolchange, prime tower, "
        "filament map, ...) from the printer preset, print preset, and project config.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](const nlohmann::json&) -> nlohmann::json {
            return run_on_main_thread([]() -> nlohmann::json {
                nlohmann::json r = describe_toolchanger_config();
                r["status"] = "success";
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });

    register_tool({
        "suggest_color_mix",
        ToolCategory::FilamentsColour,
        "Closest filament mix for a target color",
        "Suggest the closest achievable 2-3 component filament mix for a target color, from "
        "the printer's loaded physical filaments. Optionally create the mixed slot.",
        {
            {"type", "object"},
            {"properties", {
                {"target_color", {{"type", "string"}, {"description", "Target color, \"#RRGGBB\""}}},
                {"material_type", {
                    {"type", "string"},
                    {"description", "Restrict components to this filament type (e.g. \"PLA\"). Default: type of filament slot 1."}
                }},
                {"create", {
                    {"type", "boolean"},
                    {"description", "When true, also create the suggested mix as a new mixed filament slot, as set_mixed_filament does. Default false."}
                }}
            }},
            {"required", {"target_color"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const std::string target_color = params.at("target_color").get<std::string>();
            const std::string material_type = params.value("material_type", std::string());
            const bool create = params.value("create", false);

            // color_decompose_hex_to_rgb only needs six digits after the '#' and ignores whatever
            // follows, so "#B17C38ff" and "#B17C38 (bronze)" would both quietly become #B17C38.
            if (!is_hex_color(target_color))
                return nlohmann::json{{"status", "error"},
                                      {"message", "target_color must be exactly \"#RRGGBB\", got \"" + target_color + "\""}};

            return run_on_main_thread([target_color, material_type, create]() -> nlohmann::json {
                ColorDecomposeRgb target;
                if (!color_decompose_hex_to_rgb(target_color, target))
                    return nlohmann::json{{"status", "error"}, {"message", "target_color must be \"#RRGGBB\""}};

                const auto physicals = physical_filaments_for_recipe();
                if (physicals.size() < 2)
                    return nlohmann::json{{"status", "error"}, {"message", "need at least two physical filament slots"}};

                std::string effective_material_type = material_type;
                if (effective_material_type.empty())
                    effective_material_type = physicals.front().type; // default = type of filament slot 1

                const ColorDecomposeRecipeResult result =
                    recommend_from_physical_filaments(target, physicals, effective_material_type);
                const ColorMixRecipe recipe = color_mix_recipe_from_result(result);
                if (!recipe.valid)
                    return nlohmann::json{{"status", "error"}, {"message", "no mixable recipe found for target_color"}};

                // An exact match is the useful answer "load that slot, no mix required" -- not a
                // failure. It used to be returned as status: error, so an agent walking a set of
                // target colours had to match on the message text to carry on. status: error is
                // now reserved for calls that could not be answered at all.
                const std::string predicted_color =
                    recipe.exact_match ? recipe.hexes.front() : gui_mix_color(recipe.hexes, recipe.ratios);
                const double delta_e =
                    recipe.exact_match ? 0.0 : color_delta_e_hex(target_color, predicted_color);

                nlohmann::json out = {
                    {"status", "success"},
                    {"target_color", target_color},
                    {"recipe", {
                        {"components", recipe.components},
                        {"ratios", recipe.ratios},
                        {"predicted_color", predicted_color},
                        // Always false: predicted_color comes from gui_mix_color, never from the
                        // measured recipe table, so what is reported is exactly what the GUI shows
                        // for the same slot. get_color_palette's entries can say true.
                        {"measured", false}
                    }},
                    {"delta_e", delta_e},
                    {"exact_match", recipe.exact_match},
                    // A mixed slot averages layers instead of mixing pigment, so whole regions of
                    // the colour wheel are simply unreachable -- the session that prompted this got
                    // a confident-looking recipe for pure red at delta_e 54. Say which side of the
                    // threshold this landed on rather than letting the number speak for itself.
                    {"gamut", gamut_label(delta_e)},
                    {"gamut_delta_e_threshold", kGamutDeltaEThreshold},
                    {"slot", nullptr}
                };
                if (recipe.exact_match)
                    out["message"] = "target_color already matches physical filament " +
                                     std::to_string(recipe.components.front()) +
                                     " (" + recipe.hexes.front() + "); no mix needed";
                if (delta_e >= kGamutDeltaEThreshold)
                    out["message"] = "The closest mix is delta_e " + format_one_decimal(delta_e) +
                                     " from target_color, past the delta_e " +
                                     format_one_decimal(kGamutDeltaEThreshold) +
                                     " gamut threshold. A mixed slot alternates layers, so it averages "
                                     "its components' colours rather than mixing them like pigment; this "
                                     "colour is not reachable from the loaded filaments. Load a filament "
                                     "closer to it instead.";

                if (create) {
                    // An exact match has nothing to create: apply_mixed_filament needs 2-3
                    // components, and the slot the caller wants is already loaded.
                    if (recipe.exact_match) {
                        out["slot"] = recipe.components.front();
                        out["created"] = false;
                    } else {
                        // Route through the same params-shaped entry point set_mixed_filament uses,
                        // rather than hand-building a MixedFilamentResult, so there is one place
                        // (mixed_result_from_params) that turns {components, ratios} into a request.
                        const nlohmann::json synthetic_params = {{"components", recipe.components},
                                                                 {"ratios", recipe.ratios}};
                        MixedFilamentResult req;
                        std::string error;
                        if (!mixed_result_from_params(synthetic_params, req, error))
                            return nlohmann::json{{"status", "error"}, {"message", error}};
                        const int idx = wxGetApp().sidebar().apply_mixed_filament(req, -1, error);
                        if (idx < 0)
                            return nlohmann::json{{"status", "error"}, {"message", error}};
                        out["slot"] = idx + 1;
                        out["created"] = true;
                    }
                    out["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                }
                return out;
            });
        }
    });

    register_tool({
        "get_color_palette",
        ToolCategory::FilamentsColour,
        "Colors reachable by mixing loaded slots",
        "Enumerate an achievable palette of filament mixes (pairs, and optionally triples) from "
        "the printer's loaded physical filaments -- a shortlist to choose from before painting.",
        {
            {"type", "object"},
            {"properties", {
                {"max_count", {{"type", "integer"}, {"description", "Max entries to return, default 12, cap 48"}}},
                {"max_components", {
                    {"type", "integer"},
                    {"enum", {2, 3}},
                    {"description", "2 for pairs only, 3 to also include triples. Default 2."}
                }},
                {"material_type", {{"type", "string"}, {"description", "Restrict to this filament type (e.g. \"PLA\")"}}}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int max_count = params.value("max_count", 12);
            max_count = std::clamp(max_count, 1, 48);
            const int max_components = params.value("max_components", 2) >= 3 ? 3 : 2;
            const std::string material_type = params.value("material_type", std::string());

            return run_on_main_thread([max_count, max_components, material_type]() -> nlohmann::json {
                nlohmann::json enumerated = enumerate_mix_palette(max_count, max_components, material_type);
                nlohmann::json r = {
                    {"status", "success"},
                    {"palette", enumerated["entries"]},
                    {"unreachable_hues", enumerated["unreachable_hues"]},
                    {"gamut_delta_e_threshold", kGamutDeltaEThreshold}
                };
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });
}
