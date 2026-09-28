// src/slic3r/GUI/OrcaMCP/OrcaMCPLayerGcode.hpp
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/CustomGCode.hpp"
#include "OrcaMCPFilamentSlots.hpp"

// G-code at a layer -- a pause, a filament change, custom G-code or the printer's template -- as the
// Preview's layer slider adds, edits and deletes it (IMSlider::render_add_menu / render_edit_menu,
// add_code_as_tick, add_custom_gcode, delete_tick; TickCodeInfo::add_tick). The slider works only on the
// plate the Preview shows and posts its change when the preview canvas next draws, so add_layer_gcode and
// delete_layer_gcode edit the same data, the plate's CustomGCode::Info (Model::plates_custom_gcodes), by
// the slider's rules, and then run what the slider's change runs (Plater::on_layer_gcodes_changed). No wx:
// the tests drive it with plain values (tests/slic3rutils/test_layer_gcode.cpp).

namespace Slic3r {
class Print;
namespace GUI { namespace OrcaMCP {

// What add_layer_gcode's `type` takes, as the slider's menu names them.
enum class LayerGcodeKind { pause, filament_change, custom, template_gcode };

std::optional<LayerGcodeKind> layer_gcode_kind_named(const std::string& name);
// The name a stored item's type is reported by: pause, filament_change, custom, template, and
// color_change or unknown for what an older project may hold (the slider offers neither).
std::string layer_gcode_type_name(CustomGCode::Type type);

// What decides what the slider offers on a plate (Preview::update_layers_slider,
// update_layers_slider_mode, IMSlider::SetDrawMode, SetModeAndOnlyExtruder).
struct LayerGcodeRules
{
    FilamentSlotsState       slots;            // the project's filament slots, which the menu lists
    std::vector<std::string> filament_colors;  // one per slot (filament_colour): a filament change's colour
    std::vector<int>         plate_filaments;  // PartPlate::get_extruders_without_support, 1-based: what a pause records
    // How many filaments the plate's objects print, as the slicer counts them (Print::object_extruders, which the
    // Preview's slider decides by): known from the plate's Print only while it is current and up to date.
    std::optional<std::size_t> object_filaments;
    bool                       spiral_vase          = false; // the plate's own vase mode, else the print preset's
    bool                       by_object            = false; // the plate prints one object after another
    bool                       template_gcode_empty = true;  // the printer's template_custom_gcode
};

// The slider's mode, which it writes into the plate's Info with every change: MultiAsSingle when the
// project has several filaments and the plate prints with one or more, SingleExtruder otherwise.
CustomGCode::Mode slider_mode(const LayerGcodeRules& rules);
// The filament a pause, template or custom G-code records (IMSlider's max(1, m_only_extruder)).
int recorded_filament(const LayerGcodeRules& rules);

// The layer (0-based) a stored item at `print_z` is shown at: the first layer at or above it, within the
// slider's tolerance (IMSlider::get_tick_from_value), or nullopt past the last layer.
std::optional<std::size_t> layer_of(const std::vector<double>& layer_zs, double print_z);

// An add_layer_gcode call.
struct LayerGcodeRequest
{
    LayerGcodeKind kind     = LayerGcodeKind::pause;
    int            filament = 0;  // filament_change: 1-based slot
    std::string    gcode;         // custom
};

// How the slicer takes a filament change recorded in `mode` on this plate (CustomGCode::tool_changes_off, the rule the
// Preview's slider shows them by), or nullopt when only the filaments its objects print could tell and they are not known.
std::optional<CustomGCode::ToolChangesOff> filament_changes_off(const LayerGcodeRules& rules, CustomGCode::Mode mode);

// Why add_layer_gcode refuses `request` on this plate whatever the layer, or nullopt: a plate printed by object, and the
// kind's own rules (a template the printer lacks, custom G-code's length, a filament change the slider does not offer).
// A filament change on a plate whose objects' filaments are not known yet is refused only once they are.
std::optional<std::string> layer_gcode_request_refusal(const LayerGcodeRequest& request, const LayerGcodeRules& rules);

// What an add or a delete did: the item it wrote or removed, and one it replaced.
struct LayerGcodeChange
{
    bool                             changed = false;
    CustomGCode::Item                item{};
    std::optional<CustomGCode::Item> replaced;
};

// Adds the request at layer `layer` (0-based) of `layer_zs` to `info`, or edits what is there where the
// slider's Edit would (a custom G-code's text, a filament change's filament). Returns why not, before
// changing anything: a by-object plate; a layer that holds another kind (the slider offers only Delete
// there); a template the printer has none of; empty or too long custom G-code; a filament change on a
// project of one filament, on a plate printed with several, in spiral vase, or to a slot the project
// lacks. A request for what is already there changes nothing.
std::optional<std::string> add_layer_gcode(CustomGCode::Info& info, const std::vector<double>& layer_zs, std::size_t layer,
                                           const LayerGcodeRequest& request, const LayerGcodeRules& rules, LayerGcodeChange& change);

// Removes the item at layer `layer`, as the slider's Delete does, or says there is none.
std::optional<std::string> delete_layer_gcode(CustomGCode::Info& info, const std::vector<double>& layer_zs, std::size_t layer,
                                              LayerGcodeChange& change);

// Which slice a plate's layers were read from: each object's slicing and support steps, with the stamp each
// took when it was last done. A step done again takes a new stamp, so the same stamps are the same layers;
// "the steps are done", which is all the Print tells otherwise, holds for another slice's layers too.
struct SliceLayersStamp
{
    std::vector<std::array<std::size_t, 3>> objects; // {model object id, slicing step's stamp, support step's stamp}
    bool operator==(const SliceLayersStamp& other) const { return objects == other.objects; }
    bool operator!=(const SliceLayersStamp& other) const { return !(*this == other); }
};

// The stamp of `print`'s layers, or nullopt while an object's layers or support are not done.
std::optional<SliceLayersStamp> slice_layers_stamp(const Print& print);

// One item as get_scene_info's layer_gcodes and the tools' answers give it: {layer (1-based, null when
// the plate's layers are not known), z_mm, type, filament (a filament change), gcode (custom)}. With the plate's
// rules, a filament change also says whether the slicer takes it: active (true, false or null when not known) and,
// when false, inactive_reason. The Preview's slider hides one that is not.
nlohmann::json layer_gcode_json(const CustomGCode::Item& item, const std::vector<double>* layer_zs, const LayerGcodeRules* rules = nullptr,
                                CustomGCode::Mode mode = CustomGCode::MultiAsSingle);
nlohmann::json layer_gcodes_json(const CustomGCode::Info& info, const std::vector<double>* layer_zs, const LayerGcodeRules* rules = nullptr);

}}} // namespace Slic3r::GUI::OrcaMCP
