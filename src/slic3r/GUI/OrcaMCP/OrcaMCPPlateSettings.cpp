// src/slic3r/GUI/OrcaMCP/OrcaMCPPlateSettings.cpp
#include "OrcaMCPPlateSettings.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPartEdits.hpp"

#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <numeric>
#include <set>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

const std::vector<std::string>& bed_type_values() { return print_config_def.get("curr_bed_type")->enum_values; }

std::string print_sequence_value(PrintSequence sequence)
{
    switch (sequence) {
    case PrintSequence::ByLayer: return "by layer";
    case PrintSequence::ByObject: return "by object";
    default: break;
    }
    return "global";
}

std::string spiral_vase_value(SpiralVase vase)
{
    switch (vase) {
    case SpiralVase::on: return "on";
    case SpiralVase::off: return "off";
    case SpiralVase::global: break;
    }
    return "global";
}

// A text argument as one of `choices`, or `error`.
std::optional<std::string> read_choice(const nlohmann::json& params, const std::string& key, const std::vector<std::string>& choices,
                                       std::string& error)
{
    const std::optional<std::string> value = read_text(params, key, error);
    if (!value)
        return std::nullopt;
    if (std::find(choices.begin(), choices.end(), *value) == choices.end()) {
        std::string listed;
        for (const std::string& choice : choices)
            listed += (listed.empty() ? "\"" : ", \"") + choice + "\"";
        error = key + " must be one of " + listed + "; got \"" + *value + "\"";
        return std::nullopt;
    }
    return value;
}

std::string filament_order_hint(int filament_count)
{
    std::vector<int> example(std::size_t(std::max(filament_count, 1)));
    std::iota(example.begin(), example.end(), 1);
    if (example.size() > 1)
        std::swap(example[0], example[1]);
    return "each of filaments 1 to " + std::to_string(filament_count) + " exactly once, in printing order, e.g. " + nlohmann::json(example).dump();
}

// ---- A value's shape, read before what the dialog offers is asked ----

std::string order_hint_error(const std::string& what, int filament_count, const nlohmann::json& given)
{
    return what + " must list " + filament_order_hint(filament_count) + ", or be \"auto\"; got " + given.dump();
}

// A list of filament numbers, or nothing when `value` is not one (`error` names `what`).
std::optional<std::vector<int>> read_filament_numbers(const nlohmann::json& value, const std::string& what, int filament_count,
                                                      std::string& error)
{
    std::vector<int> numbers;
    if (value.is_array())
        for (const nlohmann::json& item : value) {
            int number = 0;
            if (!parse_integer_param(item, number)) {
                error = order_hint_error(what, filament_count, value);
                return std::nullopt;
            }
            numbers.push_back(number);
        }
    else {
        error = order_hint_error(what, filament_count, value);
        return std::nullopt;
    }
    return numbers;
}

bool is_auto(const nlohmann::json& value) { return value.is_string() && value.get<std::string>() == "auto"; }

// One range of the other layers as given: {from_layer, to_layer (null or left out: to the last layer), order}.
std::optional<LayerPrintSequence> read_layer_range(const nlohmann::json& range, std::size_t index, int filament_count, std::string& error)
{
    const std::string what = "other_layers_filament_order[" + std::to_string(index) + "]";
    int               from = 0;
    if (!range.is_object() || !range.contains("from_layer") || !parse_integer_param(range.at("from_layer"), from)) {
        error = what + " must be {\"from_layer\": 2 or more, \"to_layer\": that or more (null: to the last layer), \"order\": [...]}";
        return std::nullopt;
    }
    int to = k_last_layer;
    if (range.contains("to_layer") && !range.at("to_layer").is_null() && !parse_integer_param(range.at("to_layer"), to)) {
        error = what + ".to_layer must be a layer number, or null for the last layer; got " + range.at("to_layer").dump();
        return std::nullopt;
    }
    if (!range.contains("order")) {
        error = what + ".order is required: " + filament_order_hint(filament_count);
        return std::nullopt;
    }
    const auto order = read_filament_numbers(range.at("order"), what + ".order", filament_count, error);
    if (!order)
        return std::nullopt;
    return LayerPrintSequence{{from, to}, *order};
}

// In layer order, as the dialog keeps them (LayerSeqInfo::operator<).
std::vector<LayerPrintSequence> in_layer_order(std::vector<LayerPrintSequence> ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const LayerPrintSequence& a, const LayerPrintSequence& b) { return a.first < b.first; });
    return ranges;
}

std::optional<std::vector<LayerPrintSequence>> read_layer_ranges(const nlohmann::json& value, int filament_count, std::string& error)
{
    if (!value.is_array() || value.empty()) {
        error = "other_layers_filament_order must be \"auto\" or a list of {from_layer, to_layer, order}; got " + value.dump();
        return std::nullopt;
    }
    std::vector<LayerPrintSequence> ranges;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto range = read_layer_range(value[i], i, filament_count, error);
        if (!range)
            return std::nullopt;
        ranges.push_back(*range);
    }
    return in_layer_order(ranges);
}

// ---- What the dialog offers, asked only of a change ----

// What is wrong with a custom order, or nothing: every filament once.
std::optional<std::string> filament_order_error(const std::vector<int>& order, int filament_count, const std::string& what)
{
    std::vector<int> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    std::vector<int> every(std::size_t(std::max(filament_count, 0)));
    std::iota(every.begin(), every.end(), 1);
    if (sorted == every)
        return std::nullopt;
    return order_hint_error(what, filament_count, order);
}

std::string layer_range_text(const LayerPrintSequence& range)
{
    return std::to_string(range.first.first) + "-" + (range.first.second == k_last_layer ? std::string("the last layer") : std::to_string(range.first.second));
}

// What is wrong with ranges (in layer order), or nothing: each from layer 2, ending at or after its start,
// every filament once, no two sharing a layer.
std::optional<std::string> layer_ranges_error(const std::vector<LayerPrintSequence>& ranges, int filament_count)
{
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const std::string what = "other_layers_filament_order's range " + layer_range_text(ranges[i]);
        if (ranges[i].first.first < k_first_other_layer)
            return what + ": from_layer must be 2 or more: layer 1 is first_layer_filament_order's";
        if (ranges[i].first.second < ranges[i].first.first)
            return what + ": to_layer must be from_layer or more, or null for the last layer";
        if (const auto error = filament_order_error(ranges[i].second, filament_count, what + "'s order"))
            return error;
        if (i > 0 && ranges[i].first.first <= ranges[i - 1].first.second)
            return "other_layers_filament_order's layers " + layer_range_text(ranges[i - 1]) + " and " + layer_range_text(ranges[i]) +
                   " overlap: each layer takes one order, so give ranges that do not share a layer";
    }
    return std::nullopt;
}

const char* mixed_filaments_refusal()
{
    return "the project has mixed filaments, so a custom filament order does not take effect (the plate settings dialog turns it off "
           "then): send \"auto\"";
}

std::optional<std::vector<int>> read_first_layer_order(const nlohmann::json& params, const PlateSettingsOffer& offer, const PlateSettings& current,
                                                       std::string& error)
{
    const nlohmann::json& value = params.at("first_layer_filament_order");
    if (is_auto(value))
        return std::vector<int>();
    const auto order = read_filament_numbers(value, "first_layer_filament_order", offer.filament_count, error);
    if (!order || *order == current.first_layer_order)
        return order;
    if (offer.mixed_filaments)
        error = mixed_filaments_refusal();
    else if (const auto refusal = filament_order_error(*order, offer.filament_count, "first_layer_filament_order"))
        error = *refusal;
    return error.empty() ? order : std::nullopt;
}

std::optional<std::vector<LayerPrintSequence>> read_other_layers_order(const nlohmann::json& params, const PlateSettingsOffer& offer,
                                                                        const PlateSettings& current, std::string& error)
{
    const nlohmann::json& value = params.at("other_layers_filament_order");
    if (is_auto(value))
        return std::vector<LayerPrintSequence>();
    const auto ranges = read_layer_ranges(value, offer.filament_count, error);
    if (!ranges || *ranges == in_layer_order(current.other_layers_order))
        return ranges;
    if (offer.mixed_filaments)
        error = mixed_filaments_refusal();
    else if (const auto refusal = layer_ranges_error(*ranges, offer.filament_count))
        error = *refusal;
    return error.empty() ? ranges : std::nullopt;
}

std::optional<BedType> read_plate_bed_type(const nlohmann::json& params, const PlateSettingsOffer& offer, const PlateSettings& current,
                                           std::string& error)
{
    const std::optional<std::string> value = read_text(params, "bed_type", error);
    if (!value)
        return std::nullopt;
    const std::optional<BedType> bed_type = *value == "global" ? std::optional<BedType>(btDefault) : bed_type_from_value(*value);
    if (bed_type && *bed_type == current.bed_type)
        return bed_type;
    if (bed_type == btDefault)
        return bed_type;
    if (!offer.plate_bed_type) {
        error = "a plate's own bed type is offered for Bambu Lab printers only (the plate settings dialog greys it out on this "
                "printer), so this plate follows the global bed type: send bed_type \"global\", or set the global one with "
                "apply_config {\"settings\": [{\"type\": \"project\", \"key\": \"curr_bed_type\", \"value\": <a bed type>}]}";
        return std::nullopt;
    }
    if (!bed_type || std::find(offer.bed_types.begin(), offer.bed_types.end(), *bed_type) == offer.bed_types.end()) {
        error = "bed_type \"" + *value + "\" is not one this printer offers: \"global\", " + listed_bed_types(offer.bed_types);
        return std::nullopt;
    }
    return bed_type;
}

std::optional<std::string> read_plate_name(const nlohmann::json& params, const PlateSettings& current, std::string& error)
{
    const std::optional<std::string> name = read_text(params, "name", error);
    if (!name || *name == current.name)
        return name;
    // Characters, as the name editor counts them: UTF-8 bytes that do not continue a character.
    const std::size_t characters = std::size_t(std::count_if(name->begin(), name->end(), [](char c) { return (c & 0xC0) != 0x80; }));
    if (characters > k_max_plate_name_length) {
        error = "name must be at most " + std::to_string(k_max_plate_name_length) + " characters, as the plate's name editor takes; got " +
                std::to_string(characters);
        return std::nullopt;
    }
    return name;
}

nlohmann::json other_layers_json(const std::vector<LayerPrintSequence>& ranges)
{
    if (ranges.empty())
        return "auto";
    nlohmann::json out = nlohmann::json::array();
    for (const LayerPrintSequence& range : ranges)
        out.push_back({{"from_layer", range.first.first},
                       {"to_layer", range.first.second == k_last_layer ? nlohmann::json(nullptr) : nlohmann::json(range.first.second)},
                       {"order", range.second}});
    return out;
}

} // namespace

PlateSettings plate_settings_of(PartPlate& plate)
{
    PlateSettings settings;
    settings.name               = plate.get_plate_name();
    settings.locked             = plate.is_locked();
    settings.bed_type           = plate.get_bed_type(false);
    settings.print_sequence     = plate.get_print_seq();
    settings.first_layer_order  = plate.get_first_layer_print_sequence();
    settings.other_layers_order = plate.get_other_layers_print_sequence();
    if (plate.has_spiral_mode_config())
        settings.spiral_vase = plate.config()->opt_bool("spiral_mode") ? SpiralVase::on : SpiralVase::off;
    return settings;
}

std::optional<int> read_plate_index(const nlohmann::json& params, std::string& error)
{
    if (!params.contains("plate_index"))
        return std::nullopt;
    int plate_index = -1;
    if (!parse_integer_param(params.at("plate_index"), plate_index) || plate_index < 0) {
        error = "plate_index must be a whole number 0 or more, as get_scene_info's plate_index; got " + params.at("plate_index").dump() +
                ": omit it for the current plate";
        return std::nullopt;
    }
    return plate_index;
}

std::optional<std::string> plate_index_error(int plate_index, int plate_count)
{
    if (plate_index >= 0 && plate_index < plate_count)
        return std::nullopt;
    return "Invalid plate_index " + std::to_string(plate_index) + ": plates are 0 to " + std::to_string(plate_count - 1);
}

std::optional<PlateSettingsRequest> read_plate_settings(const nlohmann::json& params, const PlateSettingsOffer& offer,
                                                        const PlateSettings& current, std::string& error)
{
    PlateSettingsRequest request;
    request.name     = read_plate_name(params, current, error);
    request.locked   = read_flag(params, "locked", error);
    request.bed_type = read_plate_bed_type(params, offer, current, error);
    if (!error.empty())
        return std::nullopt;
    if (const auto sequence = read_choice(params, "print_sequence", {"global", "by layer", "by object"}, error))
        request.print_sequence = *sequence == "global" ? PrintSequence::ByDefault :
                                 *sequence == "by layer" ? PrintSequence::ByLayer :
                                                           PrintSequence::ByObject;
    if (const auto vase = read_choice(params, "spiral_vase", {"global", "on", "off"}, error))
        request.spiral_vase = *vase == "on" ? SpiralVase::on : *vase == "off" ? SpiralVase::off : SpiralVase::global;
    if (!error.empty())
        return std::nullopt;
    if (params.contains("first_layer_filament_order"))
        request.first_layer_order = read_first_layer_order(params, offer, current, error);
    if (error.empty() && params.contains("other_layers_filament_order"))
        request.other_layers_order = read_other_layers_order(params, offer, current, error);
    if (!error.empty())
        return std::nullopt;
    return request;
}

bool asks_anything(const PlateSettingsRequest& request)
{
    return request.name || request.locked || request.bed_type || request.print_sequence || request.first_layer_order ||
           request.other_layers_order || request.spiral_vase;
}

PlateSettings with_request(PlateSettings settings, const PlateSettingsRequest& request)
{
    if (request.name)
        settings.name = *request.name;
    if (request.locked)
        settings.locked = *request.locked;
    if (request.bed_type)
        settings.bed_type = *request.bed_type;
    if (request.print_sequence)
        settings.print_sequence = *request.print_sequence;
    if (request.first_layer_order)
        settings.first_layer_order = *request.first_layer_order;
    if (request.other_layers_order)
        settings.other_layers_order = *request.other_layers_order;
    if (request.spiral_vase)
        settings.spiral_vase = *request.spiral_vase;
    return settings;
}

std::vector<std::string> plate_settings_differences(const PlateSettings& before, const PlateSettings& after)
{
    std::vector<std::string> changes;
    if (after.name != before.name)
        changes.emplace_back("name");
    if (after.locked != before.locked)
        changes.emplace_back("locked");
    if (after.bed_type != before.bed_type)
        changes.emplace_back("bed_type");
    if (after.print_sequence != before.print_sequence)
        changes.emplace_back("print_sequence");
    if (after.first_layer_order != before.first_layer_order)
        changes.emplace_back("first_layer_filament_order");
    // The same ranges in another order are the same setting: the slicer reads each layer's range.
    if (in_layer_order(after.other_layers_order) != in_layer_order(before.other_layers_order))
        changes.emplace_back("other_layers_filament_order");
    if (after.spiral_vase != before.spiral_vase)
        changes.emplace_back("spiral_vase");
    return changes;
}

std::vector<std::string> plate_settings_changes(const PlateSettings& now, const PlateSettingsRequest& request)
{
    return plate_settings_differences(now, with_request(now, request));
}

bool changes_slicing(const std::vector<std::string>& changes)
{
    return std::any_of(changes.begin(), changes.end(), [](const std::string& change) { return change != "name" && change != "locked"; });
}

nlohmann::json plate_settings_json(const PlateSettings& settings)
{
    return {{"name", settings.name},
            {"locked", settings.locked},
            {"bed_type", bed_type_value(settings.bed_type)},
            {"print_sequence", print_sequence_value(settings.print_sequence)},
            {"first_layer_filament_order", settings.first_layer_order.empty() ? nlohmann::json("auto") : nlohmann::json(settings.first_layer_order)},
            {"other_layers_filament_order", other_layers_json(settings.other_layers_order)},
            {"spiral_vase", spiral_vase_value(settings.spiral_vase)}};
}

nlohmann::json plate_effective_json(const PlateSettings& settings, BedType global_bed_type, PrintSequence global_print_sequence,
                                    bool global_spiral_vase)
{
    const BedType       bed_type = settings.bed_type == btDefault ? global_bed_type : settings.bed_type;
    const PrintSequence sequence = settings.print_sequence == PrintSequence::ByDefault ? global_print_sequence : settings.print_sequence;
    const bool          vase     = settings.spiral_vase == SpiralVase::global ? global_spiral_vase : settings.spiral_vase == SpiralVase::on;
    return {{"bed_type", bed_type_value(bed_type)}, {"print_sequence", print_sequence_value(sequence)}, {"spiral_vase", vase}};
}

// ---- Bed types ----

std::string bed_type_value(BedType bed_type)
{
    const std::vector<std::string>& values = bed_type_values();
    if (bed_type <= btDefault || std::size_t(bed_type) > values.size())
        return "global";
    return values[std::size_t(bed_type) - 1];
}

std::optional<BedType> bed_type_from_value(const std::string& value)
{
    const std::vector<std::string>& values = bed_type_values();
    const auto                      it     = std::find(values.begin(), values.end(), value);
    if (it == values.end())
        return std::nullopt;
    return BedType(int(it - values.begin()) + 1);
}

std::string listed_bed_types(const std::vector<BedType>& bed_types)
{
    std::string listed;
    for (BedType bed_type : bed_types)
        listed += (listed.empty() ? "\"" : ", \"") + bed_type_value(bed_type) + "\"";
    return listed;
}

std::optional<std::string> global_bed_type_refusal(const std::string& value, const std::vector<BedType>& offered, bool selectable,
                                                   BedType current, BedType& parsed)
{
    const std::optional<BedType> bed_type = bed_type_from_value(value);
    if (!bed_type)
        return "curr_bed_type \"" + value + "\" names no bed type: one of " + listed_bed_types(offered);
    parsed = *bed_type;
    if (!selectable && *bed_type != current)
        return "this printer has one bed type, \"" + bed_type_value(current) + "\": the sidebar greys its bed-type list out (only Bambu "
               "Lab printers, and printers with support_multi_bed_types, offer several)";
    if (std::find(offered.begin(), offered.end(), *bed_type) == offered.end())
        return "curr_bed_type \"" + value + "\" is not one this printer offers: " + listed_bed_types(offered);
    return std::nullopt;
}

// ---- Spiral vase ----

const DynamicPrintConfig& vase_object_settings() { return PartPlate::vase_mode_object_config(); }

std::vector<std::string> vase_settings_carried(const DynamicPrintConfig& object_config)
{
    std::vector<std::string> carried;
    const DynamicPrintConfig& vase = vase_object_settings();
    for (const std::string& key : vase.keys())
        if (object_config.has(key) && object_config.opt_serialize(key) == vase.opt_serialize(key))
            carried.push_back(key);
    return carried;
}

std::vector<std::string> changed_config_keys(const DynamicPrintConfig& before, const DynamicPrintConfig& after)
{
    std::set<std::string> keys;
    for (const std::string& key : before.keys())
        keys.insert(key);
    for (const std::string& key : after.keys())
        keys.insert(key);
    std::vector<std::string> changed;
    for (const std::string& key : keys)
        if (!before.has(key) || !after.has(key) || before.opt_serialize(key) != after.opt_serialize(key))
            changed.push_back(key);
    return changed;
}

}}} // namespace Slic3r::GUI::OrcaMCP
