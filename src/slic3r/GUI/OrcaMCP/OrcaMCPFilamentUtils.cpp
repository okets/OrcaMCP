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
#include "libslic3r/ColorDecomposeRecipe.hpp"
#include "libslic3r/Model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

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

bool set_flush_volumes(const nlohmann::json& matrix, int extruder, std::optional<double> flush_multiplier, std::string& error)
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

    // Validate the (optional) multiplier write too, before mutating anything, so a bad
    // multiplier never leaves the matrix half-written.
    ConfigOptionFloats* mult = nullptr;
    if (flush_multiplier.has_value()) {
        mult = pb->project_config.option<ConfigOptionFloats>("flush_multiplier", true);
        if (!mult || mult->values.empty()) { error = "flush_multiplier is not configured"; return false; }
        if (size_t(extruder) >= mult->values.size()) { error = "extruder out of range for flush_multiplier"; return false; }
    }

    set_flush_volumes_matrix(mat->values, block, size_t(extruder), extruders);
    if (mult) mult->values[size_t(extruder)] = *flush_multiplier;

    // Single refresh at the end, after both writes -- not after the matrix alone.
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

std::vector<ColorDecomposePhysicalFilament> physical_filaments_for_recipe()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    const DynamicPrintConfig& proj = pb->project_config;
    // get_filament_type() is non-const and needs an lvalue -- hold a local copy.
    DynamicPrintConfig full = pb->full_config();

    std::vector<ColorDecomposePhysicalFilament> out;
    // physical_filament_config_indices() gives the actual (possibly non-contiguous) config
    // indices of non-mixed slots -- num_physical_filaments() is only a count, not a bound.
    for (size_t idx : pb->physical_filament_config_indices()) {
        std::string displayed_type;
        ColorDecomposePhysicalFilament f;
        f.color_hex = opt_str_at(proj, "filament_colour", idx);
        f.name = idx < pb->filament_presets.size() ? pb->filament_presets[idx] : std::string();
        f.type = full.get_filament_type(displayed_type, int(idx));
        f.is_mixed = false;
        f.filament_index = static_cast<unsigned int>(idx + 1); // 1-based slot
        out.push_back(std::move(f));
    }
    return out;
}

MixColorPrediction predicted_mix_color(const std::vector<std::string>& hexes, const std::vector<int>& ratios)
{
    const std::string measured = lookup_measured_blend_color(hexes, ratios);
    if (!measured.empty())
        return {measured, true};
    return {blend_color_multi(hexes, ratios), false};
}

double color_delta_e_hex(const std::string& hex_a, const std::string& hex_b)
{
    ColorDecomposeRgb a, b;
    if (!color_decompose_hex_to_rgb(hex_a, a) || !color_decompose_hex_to_rgb(hex_b, b))
        return std::numeric_limits<double>::max();
    return color_decompose_delta_e(a, b);
}

namespace {

// Hue angle in degrees [0, 360) from an "#RRGGBB" string; 0 (red) for unparsable/gray input.
// Used only to order the palette -- matching MixedFilamentDialog's visual grouping is not a
// goal, so a plain HSV hue (no Lab) is enough.
double hue_degrees(const std::string& hex)
{
    ColorDecomposeRgb rgb;
    if (!color_decompose_hex_to_rgb(hex, rgb))
        return 0.0;
    const double r = rgb.r / 255.0, g = rgb.g / 255.0, b = rgb.b / 255.0;
    const double max_c = std::max({r, g, b}), min_c = std::min({r, g, b});
    const double delta = max_c - min_c;
    if (delta < 1e-9)
        return 0.0;
    double h;
    if (max_c == r)      h = std::fmod((g - b) / delta, 6.0);
    else if (max_c == g) h = (b - r) / delta + 2.0;
    else                 h = (r - g) / delta + 4.0;
    h *= 60.0;
    return h < 0.0 ? h + 360.0 : h;
}

// Same-type grouping rule as MixedFilamentDialog::rebuild_recommendation_items: only
// same-type combos are recommended, and support filaments (type ending "-S") are excluded.
std::map<std::string, std::vector<ColorDecomposePhysicalFilament>> group_by_type(
    const std::vector<ColorDecomposePhysicalFilament>& filaments, const std::string& material_type)
{
    std::map<std::string, std::vector<ColorDecomposePhysicalFilament>> groups;
    for (const auto& f : filaments) {
        if (f.type.size() >= 2 && f.type.compare(f.type.size() - 2, 2, "-S") == 0)
            continue;
        if (!material_type.empty() && f.type != material_type)
            continue;
        groups[f.type].push_back(f);
    }
    return groups;
}

struct PaletteCandidate {
    std::vector<unsigned int> components;
    std::vector<int>          ratios;
    std::string               predicted_color;
    bool                       measured{false};
};

void add_candidate_if_novel(std::vector<PaletteCandidate>& accepted,
                            const std::vector<ColorDecomposePhysicalFilament>& physicals,
                            std::vector<unsigned int> components,
                            std::vector<int> ratios)
{
    std::vector<std::string> hexes;
    hexes.reserve(components.size());
    for (unsigned int idx : components) {
        auto it = std::find_if(physicals.begin(), physicals.end(),
            [idx](const ColorDecomposePhysicalFilament& f) { return f.filament_index == idx; });
        hexes.push_back(it != physicals.end() ? it->color_hex : std::string("#808080"));
    }

    const MixColorPrediction prediction = predicted_mix_color(hexes, ratios);
    constexpr double kMinDeltaE = 5.0;

    for (const auto& f : physicals)
        if (color_delta_e_hex(prediction.hex, f.color_hex) < kMinDeltaE)
            return;
    for (const auto& c : accepted)
        if (color_delta_e_hex(prediction.hex, c.predicted_color) < kMinDeltaE)
            return;

    accepted.push_back({std::move(components), std::move(ratios), prediction.hex, prediction.measured});
}

} // namespace

nlohmann::json enumerate_mix_palette(int max_count, int max_components, const std::string& material_type)
{
    const auto physicals = physical_filaments_for_recipe();
    const auto groups = group_by_type(physicals, material_type);

    std::vector<PaletteCandidate> accepted;
    static const std::vector<std::vector<int>> kPairRatios  = {{70, 30}, {50, 50}, {30, 70}};

    for (const auto& [type, group] : groups) {
        for (size_t i = 0; i < group.size(); ++i) {
            for (size_t j = i + 1; j < group.size(); ++j) {
                for (const auto& ratios : kPairRatios)
                    add_candidate_if_novel(accepted, physicals,
                        {group[i].filament_index, group[j].filament_index}, ratios);

                if (max_components < 3)
                    continue;
                for (size_t k = j + 1; k < group.size(); ++k) {
                    const unsigned int idx[3] = {group[i].filament_index, group[j].filament_index, group[k].filament_index};
                    // Each component takes the dominant (50%) role in turn, the other two
                    // splitting 25/25 -- same rotation as the GUI's triple recommendations.
                    for (int dominant = 0; dominant < 3; ++dominant) {
                        std::vector<unsigned int> components = {
                            idx[(dominant + 1) % 3], idx[(dominant + 2) % 3], idx[dominant]
                        };
                        add_candidate_if_novel(accepted, physicals, std::move(components), {25, 25, 50});
                    }
                }
            }
        }
    }

    std::sort(accepted.begin(), accepted.end(), [](const PaletteCandidate& a, const PaletteCandidate& b) {
        return hue_degrees(a.predicted_color) < hue_degrees(b.predicted_color);
    });
    if (int(accepted.size()) > max_count)
        accepted.resize(size_t(max_count));

    nlohmann::json palette = nlohmann::json::array();
    for (const auto& c : accepted) {
        palette.push_back({
            {"components", c.components},
            {"ratios", c.ratios},
            {"predicted_color", c.predicted_color},
            {"measured", c.measured}
        });
    }
    return palette;
}

}}} // namespace Slic3r::GUI::OrcaMCP
