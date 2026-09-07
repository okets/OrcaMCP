// src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPFilamentUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

nlohmann::json with_filaments(nlohmann::json result)
{
    result["filaments"] = describe_filaments()["filaments"];
    result["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
    return result;
}

// Overwrites one extruder's flush multiplier. Kept separate from set_flush_volumes() (which
// only owns the matrix) since the multiplier is an independent, optional part of the request.
bool apply_flush_multiplier(int extruder, double value, std::string& error)
{
    auto* mult = wxGetApp().preset_bundle->project_config.option<Slic3r::ConfigOptionFloats>("flush_multiplier", true);
    if (!mult || mult->values.empty()) { error = "flush_multiplier is not configured"; return false; }
    if (size_t(extruder) >= mult->values.size()) { error = "extruder out of range for flush_multiplier"; return false; }
    mult->values[size_t(extruder)] = value;
    return true;
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
            const int slot = params.value("slot", -1);

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
                    {"description", "Part index within the object; omit for the whole object"}
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
            const bool has_multiplier = params.contains("flush_multiplier");
            const double multiplier = has_multiplier ? params["flush_multiplier"].get<double>() : 0.0;

            return run_on_main_thread([matrix, extruder, has_multiplier, multiplier]() -> nlohmann::json {
                std::string error;
                if (!set_flush_volumes(matrix, extruder, error))
                    return nlohmann::json{{"status", "error"}, {"message", error}};
                if (has_multiplier && !apply_flush_multiplier(extruder, multiplier, error))
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
}
