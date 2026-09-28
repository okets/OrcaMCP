// src/slic3r/GUI/OrcaMCP/OrcaMCPLayerGcode.cpp
#include "OrcaMCPLayerGcode.hpp"

#include <algorithm>
#include <cmath>

#include "slic3r/GUI/IMSlider.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// The slider's custom G-code window holds 1023 characters (IMSlider::m_custom_gcode, 1024 with its end).
constexpr std::size_t k_max_custom_gcode = 1023;

std::string layer_words(std::size_t layer) { return "layer " + std::to_string(layer + 1); }

// The item of `info` shown at layer `layer`, or end().
std::vector<CustomGCode::Item>::iterator item_at(CustomGCode::Info& info, const std::vector<double>& layer_zs, std::size_t layer)
{
    return std::find_if(info.gcodes.begin(), info.gcodes.end(),
                        [&](const CustomGCode::Item& item) { return layer_of(layer_zs, item.print_z) == layer; });
}

// What the menu offers for a filament change on this plate (IMSlider::m_can_change_color and the menu's
// extruder_num > 1), or why not.
std::optional<std::string> filament_change_refusal(const LayerGcodeRules& rules, int filament)
{
    if (rules.slots.slots() < 2)
        return std::string("The project has one filament, so there is no filament to change to: add_filament_slot adds one");
    if (rules.spiral_vase)
        return std::string("The app offers no filament change at a layer in spiral vase mode");
    for (int used : rules.plate_filaments)
        if (used != rules.plate_filaments.front())
            return std::string("The app offers a filament change at a layer only on a plate printed with one filament, and this "
                               "one prints with several: paint or set_object_filament is how a plate changes filaments");
    if (filament < 1)
        return "filament " + std::to_string(filament) + " is not a slot: filaments are numbered from 1";
    if (const auto refusal = filament_number_refusal(rules.slots, "extruder", filament))
        return "filament" + refusal->substr(std::string("extruder").size());
    return std::nullopt;
}

std::optional<std::string> kind_refusal(const LayerGcodeRequest& request, const LayerGcodeRules& rules)
{
    switch (request.kind) {
    case LayerGcodeKind::pause: return std::nullopt;
    case LayerGcodeKind::template_gcode:
        if (rules.template_gcode_empty)
            return std::string("The printer has no template G-code (its template_custom_gcode is empty), so the app offers none");
        return std::nullopt;
    case LayerGcodeKind::custom:
        if (request.gcode.empty())
            return std::string("gcode is required for type custom, and must not be empty");
        if (request.gcode.size() > k_max_custom_gcode)
            return "gcode is " + std::to_string(request.gcode.size()) + " characters; the app's custom G-code takes at most " +
                   std::to_string(k_max_custom_gcode);
        return std::nullopt;
    case LayerGcodeKind::filament_change: return filament_change_refusal(rules, request.filament);
    }
    return std::nullopt;
}

CustomGCode::Type item_type(LayerGcodeKind kind)
{
    switch (kind) {
    case LayerGcodeKind::pause: return CustomGCode::PausePrint;
    case LayerGcodeKind::filament_change: return CustomGCode::ToolChange;
    case LayerGcodeKind::custom: return CustomGCode::Custom;
    case LayerGcodeKind::template_gcode: return CustomGCode::Template;
    }
    return CustomGCode::Unknown;
}

// The colour the slider gives an item it adds (TickCodeInfo::add_tick, get_color_for_tick): the filament's
// for a filament change or a template, none for a pause or custom G-code.
std::string item_color(CustomGCode::Type type, int filament, const LayerGcodeRules& rules)
{
    if (type != CustomGCode::ToolChange && type != CustomGCode::Template)
        return {};
    return filament >= 1 && std::size_t(filament) <= rules.filament_colors.size() ? rules.filament_colors[std::size_t(filament - 1)]
                                                                                   : std::string();
}

CustomGCode::Item new_item(double print_z, const LayerGcodeRequest& request, const LayerGcodeRules& rules)
{
    const CustomGCode::Type type     = item_type(request.kind);
    const int               filament = type == CustomGCode::ToolChange ? request.filament : recorded_filament(rules);
    return {print_z, type, filament, item_color(type, filament, rules), type == CustomGCode::Custom ? request.gcode : std::string()};
}

// What is at the layer is the same as the request asks for.
bool same_as(const CustomGCode::Item& item, const CustomGCode::Item& wanted)
{
    return item.type == wanted.type && item.extruder == wanted.extruder && item.extra == wanted.extra;
}

void sort_items(CustomGCode::Info& info)
{
    std::stable_sort(info.gcodes.begin(), info.gcodes.end(),
                     [](const CustomGCode::Item& a, const CustomGCode::Item& b) { return a.print_z < b.print_z; });
}

} // namespace

std::optional<LayerGcodeKind> layer_gcode_kind_named(const std::string& name)
{
    if (name == "pause")
        return LayerGcodeKind::pause;
    if (name == "filament_change")
        return LayerGcodeKind::filament_change;
    if (name == "custom")
        return LayerGcodeKind::custom;
    if (name == "template")
        return LayerGcodeKind::template_gcode;
    return std::nullopt;
}

std::string layer_gcode_type_name(CustomGCode::Type type)
{
    switch (type) {
    case CustomGCode::PausePrint: return "pause";
    case CustomGCode::ToolChange: return "filament_change";
    case CustomGCode::Custom: return "custom";
    case CustomGCode::Template: return "template";
    case CustomGCode::ColorChange: return "color_change";
    case CustomGCode::Unknown: break;
    }
    return "unknown";
}

CustomGCode::Mode slider_mode(const LayerGcodeRules& rules)
{
    return rules.slots.slots() > 1 && !rules.plate_filaments.empty() ? CustomGCode::MultiAsSingle : CustomGCode::SingleExtruder;
}

int recorded_filament(const LayerGcodeRules& rules)
{
    return slider_mode(rules) == CustomGCode::MultiAsSingle ? std::max(1, rules.plate_filaments.front()) : 1;
}

std::optional<std::size_t> layer_of(const std::vector<double>& layer_zs, double print_z)
{
    const auto it = std::lower_bound(layer_zs.begin(), layer_zs.end(), print_z - GUI::epsilon());
    if (it == layer_zs.end())
        return std::nullopt;
    return std::size_t(it - layer_zs.begin());
}

std::optional<std::string> add_layer_gcode(CustomGCode::Info& info, const std::vector<double>& layer_zs, std::size_t layer,
                                           const LayerGcodeRequest& request, const LayerGcodeRules& rules, LayerGcodeChange& change)
{
    change = {};
    if (layer >= layer_zs.size())
        return layer_words(layer) + " does not exist: the plate's G-code has layers 1.." + std::to_string(layer_zs.size());
    if (rules.by_object)
        return std::string("The plate prints one object after another (print sequence by object), and the app offers no G-code at "
                           "a layer then: set_plate_settings print_sequence \"by layer\" first");
    if (const auto refusal = kind_refusal(request, rules))
        return refusal;

    const CustomGCode::Item wanted = new_item(layer_zs[layer], request, rules);
    const auto              there  = item_at(info, layer_zs, layer);
    if (there != info.gcodes.end()) {
        // The slider's Edit changes a custom G-code's text and a filament change's filament; anything else
        // at the layer it offers only to delete.
        if (there->type != wanted.type || (wanted.type != CustomGCode::Custom && wanted.type != CustomGCode::ToolChange)) {
            if (there->type == wanted.type) {
                change.item = *there;
                return std::nullopt; // the same pause or template: nothing to do
            }
            return layer_words(layer) + " already has a " + layer_gcode_type_name(there->type) +
                   ", and the app offers only to delete it there: delete_layer_gcode it first";
        }
        change.item = *there;
        if (same_as(*there, wanted))
            return std::nullopt;
        change.replaced = *there;
        there->extruder = wanted.extruder;
        there->color    = wanted.color;
        there->extra    = wanted.extra;
        change.item     = *there;
        change.changed  = true;
    } else {
        info.gcodes.push_back(wanted);
        sort_items(info);
        change.item    = wanted;
        change.changed = true;
    }
    info.mode = slider_mode(rules);
    return std::nullopt;
}

std::optional<std::string> delete_layer_gcode(CustomGCode::Info& info, const std::vector<double>& layer_zs, std::size_t layer,
                                              LayerGcodeChange& change)
{
    change = {};
    if (layer >= layer_zs.size())
        return layer_words(layer) + " does not exist: the plate's G-code has layers 1.." + std::to_string(layer_zs.size());
    const auto there = item_at(info, layer_zs, layer);
    if (there == info.gcodes.end())
        return layer_words(layer) + " has no G-code of its own: get_scene_info's plates[].layer_gcodes lists the plate's";
    change.item    = *there;
    change.changed = true;
    info.gcodes.erase(there);
    return std::nullopt;
}

nlohmann::json layer_gcode_json(const CustomGCode::Item& item, const std::vector<double>* layer_zs)
{
    const std::optional<std::size_t> layer = layer_zs != nullptr ? layer_of(*layer_zs, item.print_z) : std::nullopt;
    nlohmann::json                   json  = {
        {"layer", layer ? nlohmann::json(*layer + 1) : nlohmann::json(nullptr)},
        {"z_mm", std::round(item.print_z * 1000.) / 1000.},
        {"type", layer_gcode_type_name(item.type)},
    };
    if (item.type == CustomGCode::ToolChange || item.type == CustomGCode::ColorChange)
        json["filament"] = item.extruder;
    if (item.type == CustomGCode::Custom)
        json["gcode"] = item.extra;
    return json;
}

nlohmann::json layer_gcodes_json(const CustomGCode::Info& info, const std::vector<double>* layer_zs)
{
    nlohmann::json list = nlohmann::json::array();
    for (const CustomGCode::Item& item : info.gcodes)
        list.push_back(layer_gcode_json(item, layer_zs));
    return list;
}

}}} // namespace Slic3r::GUI::OrcaMCP
