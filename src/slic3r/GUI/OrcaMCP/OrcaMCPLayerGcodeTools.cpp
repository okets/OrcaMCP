// src/slic3r/GUI/OrcaMCP/OrcaMCPLayerGcodeTools.cpp
#include "OrcaMCPLayerGcodeTools.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPFilamentUtils.hpp"
#include "OrcaMCPInstanceBox.hpp"
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

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// The layers read last from each plate's G-code, with the slice they came from, by the plate's print index
// (PartPlateList never reuses one): an edit of the plate's layer G-code takes its G-code away, and a second edit
// before the next slice still needs them -- while the Print's layers are still that slice's.
struct LayersRead
{
    SliceLayersStamp    stamp;
    std::vector<double> zs;
};
std::map<int, LayersRead>& layers_read()
{
    static std::map<int, LayersRead> s_read;
    return s_read;
}

int print_index_of(PartPlate& plate)
{
    int print_index = -1;
    plate.get_print(nullptr, nullptr, &print_index);
    return print_index;
}

// Whether `plate`'s Print holds the settings and the model as they are now: it is the current plate, the one the
// slicing process applies them to, and no change waits for the background timer. Another plate's Print was last
// brought up to date when it was current.
bool plate_print_is_current(Plater& plater, PartPlate& plate)
{
    return &plate == plater.get_partplate_list().get_curr_plate() && !plater.is_background_process_update_scheduled() &&
           plate.fff_print() != nullptr;
}

} // namespace

std::optional<std::vector<double>> plate_layer_zs(PartPlate& plate)
{
    // Only from the current plate's Print, as the settings are now: another plate's keeps layers a settings change since
    // has made another slice's.
    if (!plate_print_is_current(*wxGetApp().plater(), plate))
        return std::nullopt;
    const std::optional<SliceLayersStamp> stamp = slice_layers_stamp(*plate.fff_print());
    if (!stamp)
        return std::nullopt;
    const int print_index = print_index_of(plate);
    // Read only from a finished result: a plate being sliced has none, and its moves are being written.
    const GCodeProcessorResult* result = plate.get_slice_result();
    if (plate.is_slice_result_valid() && result != nullptr && !result->moves.empty()) {
        std::vector<double> zs;
        for (const GcodeLayer& layer : gcode_layers(result->moves))
            zs.push_back(layer.z);
        layers_read()[print_index] = {*stamp, zs};
        return zs;
    }
    const auto read = layers_read().find(print_index);
    if (read == layers_read().end() || read->second.stamp != *stamp)
        return std::nullopt;
    return read->second.zs;
}

bool plate_layers_may_be_known(PartPlate& plate)
{
    return plate.is_slice_result_valid() || layers_read().count(print_index_of(plate)) != 0;
}

LayerGcodeRules layer_gcode_rules(Plater& plater, PartPlate& plate)
{
    const DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
    LayerGcodeRules          rules;
    rules.slots           = filament_slots_state();
    rules.filament_colors = plater.get_extruder_colors_from_plater_config();
    rules.plate_filaments = plate.get_extruders_without_support();
    // As the Preview's slider decides (Preview::update_layers_slider_mode): the Print's own count, and the plate's
    // own vase mode, else the print preset's.
    if (plate_print_is_current(plater, plate)) {
        std::vector<int>& filaments = rules.object_filaments.emplace();
        for (unsigned int filament : plate.fff_print()->object_extruders())
            filaments.push_back(int(filament) + 1);
    }
    rules.spiral_vase          = plate.get_spiral_vase_mode();
    rules.by_object            = plate.get_real_print_seq() == PrintSequence::ByObject;
    rules.template_gcode_empty = config.opt_string("template_custom_gcode").empty();
    return rules;
}

void add_objects_reach(PartPlate& plate, LayerGcodeRules& rules)
{
    const Model&              model  = wxGetApp().plater()->model();
    const DynamicPrintConfig  config = wxGetApp().preset_bundle->full_config();
    const BuildVolume         volume = plate.slicing_build_volume();
    for (std::size_t o = 0; o < model.objects.size(); ++o) {
        const ModelObject& object  = *model.objects[o];
        bool               printed = false;
        for (std::size_t i = 0; i < object.instances.size(); ++i)
            if (plate.slicer_prints_instance(int(o), int(i), volume)) {
                printed              = true;
                rules.objects_top_mm = std::max(rules.objects_top_mm.value_or(0.), instance_box(object, i).max.z());
            }
        if (printed)
            rules.largest_layer_mm = std::max(rules.largest_layer_mm, object_largest_layer_mm(object, config));
    }
    // Print::shrinkage_compensation: 100 / the first filament's percent when every filament has the same, else none.
    // The largest of them is the shortest the slicer can make the objects.
    if (const auto* shrinkage = config.option<ConfigOptionPercents>("filament_shrinkage_compensation_z"); shrinkage != nullptr)
        for (double percent : shrinkage->values)
            rules.z_shrinkage_percent = std::max(rules.z_shrinkage_percent, percent);
}

nlohmann::json plate_layer_gcodes_json(PartPlate& plate)
{
    Plater&      plater = *wxGetApp().plater();
    const Model& model  = plater.model();
    const auto   info   = model.plates_custom_gcodes.find(plate.get_index());
    if (info == model.plates_custom_gcodes.end())
        return nlohmann::json::array();
    const std::optional<std::vector<double>> zs    = plate_layer_zs(plate);
    LayerGcodeRules                          rules = layer_gcode_rules(plater, plate);
    // The objects' top tells a filament change the slicer reaches while the plate's layers are not known.
    const bool has_changes = std::any_of(info->second.gcodes.begin(), info->second.gcodes.end(),
                                         [](const CustomGCode::Item& item) { return item.type == CustomGCode::ToolChange; });
    if (!zs && has_changes)
        add_objects_reach(plate, rules);
    return layer_gcodes_json(info->second, zs ? &*zs : nullptr, &rules);
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

// Makes `plate_index` current for the call, as the Preview's slider edits the plate it shows, and applies a settings
// change the app has not taken in yet, so the plate's Print is as the settings are now. What that did goes into every
// answer the call gives: the plate made current, and whether its slice was invalidated (a Print brought up to date
// drops a slice made with other settings).
struct PlateSwitch
{
    int  previous_plate_index = -1;
    bool switched             = false;
    bool slice_invalidated    = false;
};
PlateSwitch make_plate_current_for_layers(Plater& plater, int plate_index, const McpDialogSuppressionGuard& guard)
{
    GUI::PartPlateList& plates = plater.get_partplate_list();
    PartPlate&          plate  = *plates.get_plate(plate_index);
    PlateSwitch         done{plates.get_curr_plate_index()};
    const bool          sliced = plate.is_slice_result_valid();
    done.switched              = done.previous_plate_index != plate_index;
    make_plate_current(plater, plate_index);
    apply_pending_settings(plater, guard);
    done.slice_invalidated = sliced && !plate.is_slice_result_valid();
    return done;
}

nlohmann::json with_plate_switch(nlohmann::json answer, const PlateSwitch& done)
{
    if (done.switched || done.slice_invalidated)
        answer["plate_made_current"] = {{"previous_plate_index", done.previous_plate_index},
                                        {"switched", done.switched},
                                        {"slice_invalidated", done.slice_invalidated}};
    return answer;
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

    // Everything that does not need the plate current is checked first, so a call refused for it changes nothing.
    PartPlate&   plate       = *plates.get_plate(plate_index);
    Model&       model       = plater->model();
    const auto   stored      = model.plates_custom_gcodes.find(plate_index);
    const bool   has_gcodes  = stored != model.plates_custom_gcodes.end() && !stored->second.gcodes.empty();
    const std::string no_layers = "plate_index " + std::to_string(plate_index) +
                                  " has no slice of its layers as the settings are now (never sliced, or a change since made its "
                                  "layers another slice's), which the Preview's layer slider needs: slice_all, then wait_for_slice, first";
    if (add) {
        if (const auto refusal = layer_gcode_request_refusal(call.request, layer_gcode_rules(*plater, plate)))
            return error_response(*refusal);
    } else if (!has_gcodes) {
        return error_response("plate_index " + std::to_string(plate_index) + " has no G-code at a layer to delete");
    }
    if (!plate_layers_may_be_known(plate))
        return error_response(no_layers);

    // The layer slider edits the plate the Preview shows, the current one, whose Print is brought up to the
    // settings as they are now: the layers are that slice's, or not known.
    McpDialogSuppressionGuard guard;
    const PlateSwitch                        done = make_plate_current_for_layers(*plater, plate_index, guard);
    const std::optional<std::vector<double>> zs   = plate_layer_zs(plate);
    if (!zs || zs->empty())
        return guard.report(with_plate_switch(error_response(no_layers), done));
    std::size_t layer = 0;
    if (const auto refusal = layer_index(call, *zs, layer))
        return guard.report(with_plate_switch(error_response(*refusal), done));

    CustomGCode::Info         info  = model.plates_custom_gcodes[plate_index];
    LayerGcodeChange          change;
    const LayerGcodeRules     rules   = layer_gcode_rules(*plater, plate);
    const auto                refusal = add ? add_layer_gcode(info, *zs, layer, call.request, rules, change)
                                            : delete_layer_gcode(info, *zs, layer, change);
    if (refusal)
        return guard.report(with_plate_switch(error_response(*refusal), done));
    if (change.changed) {
        model.plates_custom_gcodes[plate_index] = info;
        plater->on_layer_gcodes_changed(plate_index, change.item.type);
    }

    nlohmann::json answer = {{"status", "success"}, {"changed", change.changed}, {"plate_index", plate_index}};
    answer[add ? "added" : "deleted"] = layer_gcode_in(info, change.item, &*zs, &rules);
    if (change.replaced)
        answer["replaced"] = layer_gcode_json(*change.replaced, &*zs);
    answer["layer_gcodes"]       = layer_gcodes_json(info, &*zs, &rules);
    answer["slice_result_valid"] = plate.is_slice_result_valid();
    if (add && change.item.type == CustomGCode::PausePrint && wxGetApp().preset_bundle->full_config().opt_string("machine_pause_gcode").empty())
        answer["printer_gcode_empty"] = "the printer's pause G-code (machine_pause_gcode) is empty, so the pause writes nothing";
    if (change.changed)
        answer["undo"] = "not an undo step, as the Preview's layer slider's edits are not: delete_layer_gcode (or add_layer_gcode) changes it back";
    answer["active_warnings"] = get_active_warnings_json(plater);
    add_next_steps(answer, layer_gcode_next_steps(plate_index, change.changed));
    return guard.report(with_plate_switch(std::move(answer), done));
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
        "number (layer, as the slider numbers them) or height (z), on the current plate or plate_index, which must have been "
        "sliced as the settings are now. The slider's rules hold: nothing at a layer while the plate prints by object; a "
        "filament change only where the slicer takes one (several filaments, the plate's objects printing with one, not in "
        "spiral vase mode); the template only when the printer has one; custom G-code of 1 to 1023 characters. What needs no "
        "layer is checked first; then the plate is made current, as the slider edits the plate shown, and every answer says "
        "so (plate_made_current, with whether its slice was invalidated). On a layer that already has G-code, a custom "
        "G-code's text and a filament change's filament are changed (replaced says what was there); anything else there must "
        "be deleted first (delete_layer_gcode). The same again changes nothing. The plate loses its slice (slice it again: "
        "next_steps). Not an undo step, as the slider's edits are not. The answer lists the plate's layer_gcodes, as "
        "get_scene_info does.",
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
        "slider's Delete does. The layer is given by number (layer) or height (z), on the current plate or plate_index (made "
        "current once a plate with G-code at a layer is found, and plate_made_current says so); get_scene_info's "
        "plates[].layer_gcodes lists them. The plate loses its slice (slice it again: next_steps). Not an undo step, as the "
        "slider's edits are not.",
        layer_arguments(false),
        [](const nlohmann::json& params) -> nlohmann::json {
            LayerGcodeCall call;
            if (const auto error = read_call(params, /*add=*/false, call))
                return error_response(*error);
            return run_on_main_thread([call]() -> nlohmann::json { return layer_gcode_on_main_thread(call, /*add=*/false); });
        }});
}
