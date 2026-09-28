// src/slic3r/GUI/OrcaMCP/OrcaMCPLayerGcodeTools.cpp
#include "OrcaMCPLayerGcodeTools.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPFilamentUtils.hpp"
#include "OrcaMCPLayerGcode.hpp"
#include "OrcaMCPLayerPlan.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPServer.hpp"
#include "OrcaMCPSliceProgress.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <map>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

std::optional<std::vector<double>> plate_layer_zs(PartPlate& plate)
{
    // The layers read last, by the plate's print index (PartPlateList never reuses one): an edit of the
    // plate's layer G-code takes its G-code away, and a second edit before the next slice still needs them.
    static std::map<int, std::vector<double>> s_read;
    int print_index = -1;
    plate.get_print(nullptr, nullptr, &print_index);
    // Read only from a finished result: a plate being sliced has none, and its moves are being written.
    const GCodeProcessorResult* result = plate.get_slice_result();
    if (plate.is_slice_result_valid() && result != nullptr && !result->moves.empty()) {
        std::vector<double> zs;
        for (const GcodeLayer& layer : gcode_layers(result->moves))
            zs.push_back(layer.z);
        s_read[print_index] = zs;
        return zs;
    }
    const auto  read  = s_read.find(print_index);
    const Print* print = plate.fff_print();
    if (read == s_read.end() || print == nullptr || !print->is_step_done(posSlice) || !print->is_step_done(posSupportMaterial))
        return std::nullopt;
    return read->second;
}

nlohmann::json plate_layer_gcodes_json(PartPlate& plate)
{
    const Model& model = wxGetApp().plater()->model();
    const auto   info  = model.plates_custom_gcodes.find(plate.get_index());
    if (info == model.plates_custom_gcodes.end())
        return nlohmann::json::array();
    const std::optional<std::vector<double>> zs = plate_layer_zs(plate);
    return layer_gcodes_json(info->second, zs ? &*zs : nullptr);
}

}}} // namespace Slic3r::GUI::OrcaMCP

namespace {

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

// What an add_layer_gcode or delete_layer_gcode call asks for.
struct LayerGcodeCall
{
    std::optional<int>    plate_index;
    std::optional<int>    layer; // 1-based, as the slider numbers them
    std::optional<double> z;
    LayerGcodeRequest     request;
};

std::optional<std::string> read_call(const nlohmann::json& params, bool add, LayerGcodeCall& call)
{
    const auto read_int = [&params](const char* key, std::optional<int>& out) -> std::optional<std::string> {
        if (!params.contains(key))
            return std::nullopt;
        int value = 0;
        if (!parse_integer_param(params.at(key), value))
            return std::string(key) + " must be a whole number, got " + params.at(key).dump();
        out = value;
        return std::nullopt;
    };
    if (auto error = read_int("plate_index", call.plate_index))
        return error;
    if (auto error = read_int("layer", call.layer))
        return error;
    if (params.contains("z")) {
        double z = 0.;
        if (!parse_double_param(params.at("z"), z))
            return "z must be a number of mm, got " + params.at("z").dump();
        call.z = z;
    }
    if (call.layer.has_value() == call.z.has_value())
        return std::string("Pass the layer by its number (layer, 1-based, as the Preview's layer slider numbers them) or by its "
                           "height (z, mm), one of them");
    if (!add)
        return std::nullopt;

    const std::string type = params.value("type", std::string());
    const auto        kind = layer_gcode_kind_named(type);
    if (!kind)
        return "type must be pause, filament_change, custom or template, got \"" + type + "\"";
    call.request.kind = *kind;
    std::optional<int> filament;
    if (auto error = read_int("filament", filament))
        return error;
    if (filament.has_value() != (*kind == LayerGcodeKind::filament_change))
        return std::string(*kind == LayerGcodeKind::filament_change ? "filament is required with type filament_change: the slot to change to"
                                                                    : "filament goes with type filament_change only");
    call.request.filament = filament.value_or(0);
    if (params.contains("gcode") != (*kind == LayerGcodeKind::custom))
        return std::string(*kind == LayerGcodeKind::custom ? "gcode is required with type custom" : "gcode goes with type custom only");
    if (params.contains("gcode")) {
        if (!params.at("gcode").is_string())
            return std::string("gcode must be text");
        call.request.gcode = params.at("gcode").get<std::string>();
    }
    return std::nullopt;
}

// What decides what the Preview's layer slider offers on `plate`.
LayerGcodeRules rules_for(Plater& plater, PartPlate& plate)
{
    const DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
    LayerGcodeRules          rules;
    rules.slots                = filament_slots_state();
    rules.filament_colors      = plater.get_extruder_colors_from_plater_config();
    rules.plate_filaments      = plate.get_extruders_without_support();
    rules.spiral_vase          = config.opt_bool("spiral_mode");
    rules.by_object            = plate.get_real_print_seq() == PrintSequence::ByObject;
    rules.template_gcode_empty = config.opt_string("template_custom_gcode").empty();
    return rules;
}

// The layer (0-based) the call names, or why not.
std::optional<std::string> layer_index(const LayerGcodeCall& call, const std::vector<double>& zs, std::size_t& index)
{
    std::vector<GcodeLayer> layers;
    for (double z : zs)
        layers.push_back({0, 0, z});
    try {
        index = call.layer ? layer_by_number(layers, *call.layer) : layer_nearest_z(layers, *call.z).index;
    } catch (const std::exception& e) {
        return std::string(e.what());
    }
    return std::nullopt;
}

// Why no layer G-code may be changed on plate `plate_index` now, or nothing.
std::optional<std::string> state_refusal(Plater& plater, const std::string& tool)
{
    if (plater.only_gcode_mode())
        return std::string("The scene is a preview of a G-code file, which cannot be edited: new_project goes back to an editable scene");
    const PipelineState state = pipeline_state(plater, plater.get_partplate_list().get_plate_count());
    if (pipeline_busy(state) != PipelineBusy::idle)
        return pipeline_busy_text(state) + ", so the layer G-code was not changed: wait_for_slice (or cancel_slice) first";
    return edit_job_refusal(!plater.get_ui_job_worker().is_idle(), tool);
}

nlohmann::json layer_gcode_on_main_thread(const LayerGcodeCall& call, bool add)
{
    const std::string tool   = add ? "add_layer_gcode" : "delete_layer_gcode";
    Plater*           plater = wxGetApp().plater();
    GUI::PartPlateList& plates = plater->get_partplate_list();
    const int         plate_index = call.plate_index.value_or(plates.get_curr_plate_index());
    if (plate_index < 0 || plate_index >= plates.get_plate_count())
        return error_response("Invalid plate_index " + std::to_string(plate_index) + ": the project has plates 0.." +
                              std::to_string(plates.get_plate_count() - 1));
    if (const auto refusal = state_refusal(*plater, tool))
        return error_response(*refusal);
    PartPlate&                               plate = *plates.get_plate(plate_index);
    const std::optional<std::vector<double>> zs    = plate_layer_zs(plate);
    if (!zs || zs->empty())
        return error_response("plate_index " + std::to_string(plate_index) +
                              " has no sliced layers to put G-code at (never sliced, or an edit since changed its layers), as the Preview's "
                              "layer slider needs: slice_all, then wait_for_slice, first");
    std::size_t layer = 0;
    if (const auto refusal = layer_index(call, *zs, layer))
        return error_response(*refusal);

    McpDialogSuppressionGuard guard;
    Model&                    model = plater->model();
    CustomGCode::Info         info  = model.plates_custom_gcodes[plate_index];
    LayerGcodeChange          change;
    const LayerGcodeRules     rules   = rules_for(*plater, plate);
    const auto                refusal = add ? add_layer_gcode(info, *zs, layer, call.request, rules, change)
                                            : delete_layer_gcode(info, *zs, layer, change);
    if (refusal)
        return error_response(*refusal);
    if (change.changed) {
        model.plates_custom_gcodes[plate_index] = info;
        plater->on_layer_gcodes_changed(plate_index, change.item.type);
    }

    nlohmann::json answer = {{"status", "success"}, {"changed", change.changed}, {"plate_index", plate_index}};
    answer[add ? "added" : "deleted"] = layer_gcode_json(change.item, &*zs);
    if (change.replaced)
        answer["replaced"] = layer_gcode_json(*change.replaced, &*zs);
    answer["layer_gcodes"]       = layer_gcodes_json(info, &*zs);
    answer["slice_result_valid"] = plate.is_slice_result_valid();
    if (add && change.item.type == CustomGCode::PausePrint && wxGetApp().preset_bundle->full_config().opt_string("machine_pause_gcode").empty())
        answer["printer_gcode_empty"] = "the printer's pause G-code (machine_pause_gcode) is empty, so the pause writes nothing";
    if (change.changed)
        answer["undo"] = "not an undo step, as the Preview's layer slider's edits are not: delete_layer_gcode (or add_layer_gcode) changes it back";
    answer["active_warnings"] = get_active_warnings_json(plater);
    add_next_steps(answer, layer_gcode_next_steps(plate_index, change.changed));
    return guard.report(answer);
}

nlohmann::json layer_arguments(bool add)
{
    nlohmann::json properties = {
        {"plate_index", {{"type", "integer"}, {"minimum", 0}, {"description", "Plate index (0-based). Default: the current plate"}}},
        {"layer", {{"type", "integer"}, {"minimum", 1}, {"description", "The layer, as the Preview's layer slider numbers them (1 is the first)"}}},
        {"z", {{"type", "number"}, {"description", "Or the layer by height in mm: the printed layer nearest to it"}}},
    };
    if (add) {
        properties["type"] = {{"type", "string"},
                              {"enum", {"pause", "filament_change", "custom", "template"}},
                              {"description", "pause, filament_change (to `filament`), custom (the `gcode` text) or template (the printer's "
                                              "template G-code)"}};
        properties["filament"] = {{"type", "integer"}, {"minimum", 1}, {"description", "With filament_change: the filament slot to change to (1-based)"}};
        properties["gcode"]    = {{"type", "string"}, {"description", "With custom: the G-code to run at the start of the layer"}};
    }
    nlohmann::json schema = {{"type", "object"}, {"properties", properties}};
    if (add)
        schema["required"] = {"type"};
    return schema;
}

} // namespace

void Slic3r::GUI::OrcaMCPServer::register_layer_gcode_tools()
{
    register_tool({
        "add_layer_gcode",
        ToolCategory::Slicing,
        "Add a pause or G-code at a sliced layer",
        "Add G-code at the start of a sliced layer, as the Preview's layer slider's menu does: a pause, a filament change "
        "(filament: the slot to change to), custom G-code (gcode) or the printer's template G-code. The layer is given by its "
        "number (layer, as the slider numbers them) or height (z), on the current plate or plate_index, which must have "
        "been sliced. The slider's rules hold: nothing at a layer while the plate prints by object; a filament change only on "
        "a project of several filaments, a plate printed with one, and not in spiral vase; the template only when the "
        "printer has one; custom G-code of 1 to 1023 characters. On a layer that already has G-code, a custom G-code's text "
        "and a filament change's filament are changed (replaced says what was there); anything else there must be deleted "
        "first (delete_layer_gcode). The same again changes nothing. The plate loses its slice (slice it again: next_steps). "
        "Not an undo step, as the slider's edits are not. The answer lists the plate's layer_gcodes, as get_scene_info does.",
        layer_arguments(true),
        [](const nlohmann::json& params) -> nlohmann::json {
            LayerGcodeCall call;
            if (const auto error = read_call(params, /*add=*/true, call))
                return error_response(*error);
            return run_on_main_thread([call]() -> nlohmann::json { return layer_gcode_on_main_thread(call, /*add=*/true); });
        }});

    register_tool({
        "delete_layer_gcode",
        ToolCategory::Slicing,
        "Remove the pause or G-code at a layer",
        "Delete the G-code at a layer -- a pause, filament change, custom or template G-code -- as the Preview's layer "
        "slider's Delete does. The layer is given by number (layer) or height (z), on the current plate or plate_index; "
        "get_scene_info's plates[].layer_gcodes lists them. The plate loses its slice (slice it again: next_steps). Not an "
        "undo step, as the slider's edits are not.",
        layer_arguments(false),
        [](const nlohmann::json& params) -> nlohmann::json {
            LayerGcodeCall call;
            if (const auto error = read_call(params, /*add=*/false, call))
                return error_response(*error);
            return run_on_main_thread([call]() -> nlohmann::json { return layer_gcode_on_main_thread(call, /*add=*/false); });
        }});
}
