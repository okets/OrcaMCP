// src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.cpp
#include "OrcaMCPFilamentUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

static std::string opt_str_at(const DynamicPrintConfig& cfg, const char* key, size_t idx)
{
    const auto* opt = cfg.option<ConfigOptionStrings>(key);
    return (opt && idx < opt->values.size()) ? opt->values[idx] : std::string();
}
static bool opt_bool_at(const DynamicPrintConfig& cfg, const char* key, size_t idx)
{
    const auto* opt = cfg.option<ConfigOptionBools>(key);
    return opt && idx < opt->values.size() && opt->values[idx];
}

nlohmann::json describe_filaments()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    const DynamicPrintConfig& proj = pb->project_config;
    // get_filament_type() is non-const and needs an lvalue -- hold a local copy.
    DynamicPrintConfig full = pb->full_config();

    nlohmann::json list = nlohmann::json::array();
    const size_t n = pb->filament_presets.size();
    for (size_t i = 0; i < n; ++i) {
        std::string displayed_type;
        std::string type = full.get_filament_type(displayed_type, int(i));

        nlohmann::json f = {
            {"slot", int(i + 1)},
            {"preset", pb->filament_presets[i]},
            {"type", type},
            {"color", opt_str_at(proj, "filament_colour", i)},
            {"is_mixed", pb->is_mixed_filament(i)}
        };
        if (pb->is_mixed_filament(i)) {
            const auto components = parse_mixed_components(opt_str_at(proj, "filament_mixed_components", i));
            f["components"] = components;
            f["ratios"]     = parse_mixed_ratios(opt_str_at(proj, "filament_mixed_sublayer_ratios", i), components.size());
            f["gradient"]   = opt_bool_at(proj, "filament_mixed_gradient", i);
            const std::string range = opt_str_at(proj, "filament_mixed_gradient_range", i);
            // The gradient range always describes the first component's start/end ratio,
            // i.e. exactly two values, regardless of how many components the mix has.
            f["gradient_range"] = range.empty() ? nlohmann::json(nullptr) : nlohmann::json(parse_mixed_ratios(range, 2));
            f["per_part_gradient"] = opt_bool_at(proj, "filament_mixed_gradient_per_part", i);
        }
        list.push_back(f);
    }

    const auto* fmap = proj.option<ConfigOptionInts>("filament_map");
    nlohmann::json out = {
        {"extruder_count", pb->get_printer_extruder_count()},
        {"physical_count", int(pb->num_physical_filaments())},
        {"mixed_sublayer_enabled", full.opt_bool("enable_mixed_color_sublayer")},
        {"filament_map_mode", proj.opt_serialize("filament_map_mode")},
        {"filament_map", fmap ? nlohmann::json(fmap->values) : nlohmann::json::array()},
        {"filaments", list}
    };
    return out;
}

bool mixed_result_from_params(const nlohmann::json& params, MixedFilamentResult& out, std::string& error)
{
    if (!params.contains("components") || !params["components"].is_array() ||
        params["components"].size() < 2 || params["components"].size() > 3) {
        error = "components must be an array of 2 or 3 physical filament slots (1-based)";
        return false;
    }
    if (!params.contains("ratios") || !params["ratios"].is_array() ||
        params["ratios"].size() != params["components"].size()) {
        error = "ratios must be an array of the same length as components, in percent";
        return false;
    }
    for (const auto& c : params["components"]) {
        if (!c.is_number_integer()) { error = "components must be integers"; return false; }
    }
    int sum = 0;
    for (const auto& r : params["ratios"]) {
        if (!r.is_number_integer()) { error = "ratios must be integers (percent)"; return false; }
        sum += r.get<int>();
    }
    if (sum != 100) { error = "ratios must sum to 100"; return false; }

    out.components.clear();
    out.ratios.clear();
    for (const auto& c : params["components"]) out.components.push_back(c.get<unsigned int>());
    for (const auto& r : params["ratios"])     out.ratios.push_back(r.get<int>());

    out.gradient_enabled   = params.value("gradient", false);
    out.gradient_direction = params.value("gradient_direction", std::string("a_to_b")) == "b_to_a" ? 1 : 0;
    out.per_part_gradient  = params.value("per_part_gradient", false);

    if (out.gradient_enabled && out.components.size() != 2) {
        error = "gradient requires exactly two components";
        return false;
    }
    return true;
}

int slot_to_config_index(int slot, std::string& error)
{
    const int n = int(wxGetApp().preset_bundle->filament_presets.size());
    if (slot < 1 || slot > n) {
        error = "slot out of range 1.." + std::to_string(n);
        return -1;
    }
    return slot - 1;
}

bool set_object_filament(int object_id, int volume_id, int slot, std::string& error)
{
    Plater* plater = wxGetApp().plater();
    Model& model = plater->model();
    if (object_id < 0 || object_id >= int(model.objects.size())) { error = "Invalid object_id"; return false; }
    if (slot_to_config_index(slot, error) < 0) return false;

    ModelObject* obj = model.objects[object_id];
    plater->take_snapshot(_u8L("Change Filaments"));
    if (volume_id < 0) {
        obj->config.set("extruder", slot);
    } else {
        if (volume_id >= int(obj->volumes.size())) { error = "Invalid volume_id"; return false; }
        ModelVolume* vol = obj->volumes[volume_id];
        if (!vol->is_model_part() && !vol->is_modifier()) {
            error = "Only model parts and modifiers accept a filament";
            return false;
        }
        vol->config.set("extruder", slot);
    }
    wxGetApp().obj_list()->changed_object(object_id);
    wxGetApp().obj_list()->update_filament_colors();
    plater->update();
    return true;
}

}}} // namespace Slic3r::GUI::OrcaMCP
