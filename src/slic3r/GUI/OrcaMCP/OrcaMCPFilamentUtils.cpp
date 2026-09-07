// src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.cpp
#include "OrcaMCPFilamentUtils.hpp"
#include "OrcaMCPConfigKeys.hpp"
#include "OrcaMCPPresetConfigUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"
#include <cmath>

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

    // Domain rules (component range/physical-only, gradient needing exactly two
    // components, minimum printer filament count, ...) are enforced by
    // Sidebar::apply_mixed_filament -- keep it the single source of truth for those.
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
    ModelVolume* vol = nullptr;
    if (volume_id >= 0) {
        if (volume_id >= int(obj->volumes.size())) { error = "Invalid volume_id"; return false; }
        vol = obj->volumes[volume_id];
        if (!vol->is_model_part() && !vol->is_modifier()) {
            error = "Only model parts and modifiers accept a filament";
            return false;
        }
    }

    // Everything is validated -- only now do we touch the undo stack.
    plater->take_snapshot(_u8L("Change Filaments"));
    if (vol)
        vol->config.set("extruder", slot);
    else
        obj->config.set("extruder", slot);

    wxGetApp().obj_list()->changed_object(object_id);
    wxGetApp().obj_list()->update_filament_colors();
    plater->update();
    return true;
}

nlohmann::json describe_flush_volumes()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    const size_t extruders = size_t(pb->get_printer_extruder_count());
    const size_t n = pb->num_physical_filaments();
    const auto* mat = pb->project_config.option<ConfigOptionFloats>("flush_volumes_matrix");
    nlohmann::json matrices = nlohmann::json::array();
    for (size_t e = 0; e < extruders; ++e) {
        std::vector<double> block = mat ? get_flush_volumes_matrix(mat->values, e, extruders) : std::vector<double>();
        const size_t side = size_t(std::sqrt(double(block.size())) + 0.001);
        nlohmann::json rows = nlohmann::json::array();
        for (size_t r = 0; r < side; ++r)
            rows.push_back(std::vector<double>(block.begin() + r * side, block.begin() + (r + 1) * side));
        matrices.push_back({{"extruder", int(e)}, {"matrix", rows}});
    }
    const auto* mult = pb->project_config.option<ConfigOptionFloats>("flush_multiplier");
    return {{"extruder_count", int(extruders)}, {"filament_count", int(n)},
            {"flush_multiplier", mult ? nlohmann::json(mult->values) : nlohmann::json::array()},
            {"matrices", matrices}};
}

bool set_flush_volumes(const nlohmann::json& matrix, int extruder, std::string& error)
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    const size_t extruders = size_t(pb->get_printer_extruder_count());
    if (extruder < 0 || size_t(extruder) >= extruders) { error = "extruder out of range"; return false; }
    auto* mat = pb->project_config.option<ConfigOptionFloats>("flush_volumes_matrix", true);
    if (!mat || extruders == 0 || mat->values.size() % extruders != 0) { error = "flush_volumes_matrix is not configured"; return false; }
    const size_t side = size_t(std::sqrt(double(mat->values.size() / extruders)) + 0.001);
    if (!matrix.is_array() || matrix.size() != side) { error = "matrix must be " + std::to_string(side) + "x" + std::to_string(side); return false; }
    std::vector<double> block;
    for (const auto& row : matrix) {
        if (!row.is_array() || row.size() != side) { error = "matrix rows must have " + std::to_string(side) + " entries"; return false; }
        for (const auto& v : row) {
            if (!v.is_number()) { error = "matrix entries must be numbers"; return false; }
            block.push_back(v.get<double>());
        }
    }
    set_flush_volumes_matrix(mat->values, block, size_t(extruder), extruders);
    OrcaMCPPresetConfigUtils::RefreshAfterProjectConfigChange();
    return true;
}

void auto_calc_flush_volumes()
{
    // filament_idx < 0 and extruder_id < 0 (the defaults) already mean "every filament, every
    // extruder" inside Sidebar::auto_calc_flushing_volumes -- no need to loop here ourselves.
    // Mixed/virtual filament slots are skipped internally (auto_calc_flushing_volumes_internal).
    wxGetApp().sidebar().auto_calc_flushing_volumes();
    OrcaMCPPresetConfigUtils::RefreshAfterProjectConfigChange();
}

nlohmann::json describe_toolchanger_config()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    const DynamicPrintConfig& printer_cfg = pb->printers.get_edited_preset().config;
    const DynamicPrintConfig& print_cfg   = pb->prints.get_edited_preset().config;
    const DynamicPrintConfig& project_cfg = pb->project_config;

    auto emit = [](const DynamicPrintConfig& cfg, const std::set<std::string>& keys) {
        nlohmann::json out = nlohmann::json::object();
        for (const auto& key : keys) {
            if (cfg.has(key)) out[key] = cfg.opt_serialize(key);
        }
        return out;
    };

    return {
        {"printer", emit(printer_cfg, toolchanger_keys)},
        {"print", emit(print_cfg, toolchanger_keys)},
        {"project", emit(project_cfg, project_keys)},
        {"extruder_count", int(pb->get_printer_extruder_count())}
    };
}

}}} // namespace Slic3r::GUI::OrcaMCP
