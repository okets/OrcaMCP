// src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPFilamentUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"

#include <algorithm>
#include <optional>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

nlohmann::json with_filaments(nlohmann::json result)
{
    result["filaments"] = describe_filaments()["filaments"];
    result["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
    return result;
}

} // namespace

void OrcaMCPServer::register_filament_tools()
{
    register_tool({
        "get_filaments",
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
        "set_mixed_filament",
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
                int idx = wxGetApp().sidebar().apply_mixed_filament(req, edit_idx, error);
                if (idx < 0) return nlohmann::json{{"status", "error"}, {"message", error}};
                return with_filaments({{"status", "success"}, {"slot", idx + 1}});
            });
        }
    });

    register_tool({
        "delete_mixed_filament",
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

                wxGetApp().sidebar().delete_mixed_filament_at(size_t(it - mixed.begin()));
                return with_filaments({{"status", "success"}});
            });
        }
    });

    register_tool({
        "set_object_filament",
        "Assign a filament slot (physical or mixed) to an object, or to one part/modifier of it.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}}},
                {"filament", {{"type", "integer"}, {"description", "Filament slot, 1-based"}}},
                {"volume_id", {
                    {"type", "integer"},
                    {"minimum", -1},
                    {"description", "Part index within the object (0-based); omit, or pass -1, for the whole object"}
                }}
            }},
            {"required", {"object_id", "filament"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const int object_id = params["object_id"];
            const int filament  = params["filament"];
            const int volume_id = params.value("volume_id", -1);

            return run_on_main_thread([object_id, filament, volume_id]() -> nlohmann::json {
                std::string error;
                if (!set_object_filament(object_id, volume_id, filament, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};

                nlohmann::json r = {{"status", "success"}, {"object_id", object_id}, {"filament", filament}};
                r["volume_id"] = volume_id < 0 ? nlohmann::json(nullptr) : nlohmann::json(volume_id);
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });

    register_tool({
        "get_flush_volumes",
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
            const std::optional<double> flush_multiplier = params.contains("flush_multiplier")
                ? std::optional<double>(params["flush_multiplier"].get<double>())
                : std::nullopt;

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
                    {"description", "When true, create the mixed slot via apply_mixed_filament. Default false."}
                }}
            }},
            {"required", {"target_color"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const std::string target_color = params.at("target_color").get<std::string>();
            const std::string material_type = params.value("material_type", std::string());
            const bool create = params.value("create", false);

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
                if (!result.valid)
                    return nlohmann::json{{"status", "error"}, {"message", "no mixable recipe found for target_color"}};
                if (result.components.size() < 2) {
                    const auto& c = result.components.front();
                    return nlohmann::json{{"status", "error"}, {"message",
                        "target_color already matches physical filament " + std::to_string(c.filament_index) +
                        " (" + c.color_hex + "); no mix needed"}};
                }

                std::vector<std::string> hexes;
                std::vector<int> ratios;
                std::vector<unsigned int> components;
                for (const auto& c : result.components) {
                    hexes.push_back(c.color_hex);
                    ratios.push_back(c.ratio);
                    components.push_back(c.filament_index);
                }

                // gui_mix_color (never the measured table) so predicted_color always matches the
                // color Sidebar::apply_mixed_filament will actually give the slot below when
                // create: true. This tool never consults the measured table, so "measured" is
                // always reported false here (get_color_palette is the one that can be true).
                const std::string predicted_color = gui_mix_color(hexes, ratios);
                const double delta_e = color_delta_e_hex(target_color, predicted_color);

                nlohmann::json out = {
                    {"status", "success"},
                    {"target_color", target_color},
                    {"recipe", {
                        {"components", components},
                        {"ratios", ratios},
                        {"predicted_color", predicted_color},
                        {"measured", false}
                    }},
                    {"delta_e", delta_e},
                    {"slot", nullptr}
                };

                if (create) {
                    // Route through the same params-shaped entry point set_mixed_filament uses,
                    // rather than hand-building a MixedFilamentResult, so there is one place
                    // (mixed_result_from_params) that turns {components, ratios} into a request.
                    const nlohmann::json synthetic_params = {{"components", components}, {"ratios", ratios}};
                    MixedFilamentResult req;
                    std::string error;
                    if (!mixed_result_from_params(synthetic_params, req, error))
                        return nlohmann::json{{"status", "error"}, {"message", error}};
                    const int idx = wxGetApp().sidebar().apply_mixed_filament(req, -1, error);
                    if (idx < 0)
                        return nlohmann::json{{"status", "error"}, {"message", error}};
                    out["slot"] = idx + 1;
                    out["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                }
                return out;
            });
        }
    });

    register_tool({
        "get_color_palette",
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
                nlohmann::json r = {
                    {"status", "success"},
                    {"palette", enumerate_mix_palette(max_count, max_components, material_type)}
                };
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });
}
