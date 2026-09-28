// src/slic3r/GUI/OrcaMCP/OrcaMCPPlateSettings.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/PrintConfig.hpp"

// A plate's own settings -- its name, lock, bed type, print sequence, filament order for the first and
// the other layers, spiral vase -- as set_plate_settings takes them and get_scene_info reports them,
// and the global bed type apply_config sets. Each is what the plate settings dialog, the plate's name
// editor and its lock icon change (Plater::open_platesettings_dialog, Plater::select_plate_by_hover_id),
// and each refusal is what the dialog does not offer, said before anything changes. Apart from the app,
// so they are tested without it (tests/slic3rutils/test_plate_settings.cpp); the tool is
// OrcaMCPPlateTools.cpp.

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI {
class PartPlate;
namespace OrcaMCP {

// The dialog's Spiral vase: Same as Global, Enable, Disable.
enum class SpiralVase { global, on, off };

// The layer the dialog's "End" stands for (MAX_LAYER_VALUE in PlateSettingsDialog.cpp), and the first
// layer an other-layers order may start at: layer 1 is the first-layer order's.
constexpr int k_last_layer        = 2147483646;
constexpr int k_first_other_layer = 2;

// The longest name the plate's name editor takes.
constexpr std::size_t k_max_plate_name_length = 250;

// A plate's own settings. The defaults are the plate's when it has none of its own: the global bed type
// and print sequence, the automatic filament orders, the global spiral vase.
struct PlateSettings
{
    std::string                     name;
    bool                            locked         = false;
    BedType                         bed_type       = btDefault;                // btDefault: the global bed type
    PrintSequence                   print_sequence = PrintSequence::ByDefault; // ByDefault: the global one
    std::vector<int>                first_layer_order;                         // empty: automatic
    std::vector<LayerPrintSequence> other_layers_order;                        // empty: automatic
    SpiralVase                      spiral_vase = SpiralVase::global;
};

// The settings plate `plate` holds (its config, name and lock), as PartPlate's getters read them.
PlateSettings plate_settings_of(PartPlate& plate);

// plate_index as a call gives it: nothing when left out (the current plate), else a whole number 0 or
// more; given as anything else, `error` says so.
std::optional<int> read_plate_index(const nlohmann::json& params, std::string& error);
// What is wrong with `plate_index` among `plate_count` plates, or nothing.
std::optional<std::string> plate_index_error(int plate_index, int plate_count);

// What the call asks to change: only what it gives.
struct PlateSettingsRequest
{
    std::optional<std::string>                     name;
    std::optional<bool>                            locked;
    std::optional<BedType>                         bed_type;
    std::optional<PrintSequence>                   print_sequence;
    std::optional<std::vector<int>>                first_layer_order;
    std::optional<std::vector<LayerPrintSequence>> other_layers_order;
    std::optional<SpiralVase>                      spiral_vase;
};

// What the dialog offers on this printer and project.
struct PlateSettingsOffer
{
    int                  filament_count = 1;     // the project's filaments: each custom order lists each once
    bool                 mixed_filaments = false; // the dialog turns custom orders off while any is mixed
    bool                 plate_bed_type = false;  // the dialog offers a plate its own bed type (Bambu Lab printers only)
    std::vector<BedType> bed_types;               // the bed types the printer offers (the sidebar's list)
};

// The request the call's arguments make, or why they make none: a value the dialog does not offer, an
// order that does not list each filament once, overlapping or out-of-range layer ranges, a name too long.
std::optional<PlateSettingsRequest> read_plate_settings(const nlohmann::json& params, const PlateSettingsOffer& offer, std::string& error);

// `settings` with the request's changes.
PlateSettings with_request(PlateSettings settings, const PlateSettingsRequest& request);

// The arguments (as named) whose value differs between the two.
std::vector<std::string> plate_settings_differences(const PlateSettings& before, const PlateSettings& after);
// The arguments (as named) whose value the request changes; empty for a request that changes nothing.
std::vector<std::string> plate_settings_changes(const PlateSettings& now, const PlateSettingsRequest& request);

// Whether a change among `changes` reaches the slice (anything but the name and the lock).
bool changes_slicing(const std::vector<std::string>& changes);

// The settings in exactly the shape set_plate_settings takes them: sent back, they change nothing.
nlohmann::json plate_settings_json(const PlateSettings& settings);

// What applies on the plate: its own setting, else the global one (`global_bed_type`,
// `global_print_sequence`, `global_spiral_vase`).
nlohmann::json plate_effective_json(const PlateSettings& settings, BedType global_bed_type, PrintSequence global_print_sequence,
                                    bool global_spiral_vase);

// ---- Bed types ----

// A bed type as the config spells it ("Textured PEI Plate"), "global" for btDefault.
std::string bed_type_value(BedType bed_type);
// The bed type a config value names, or nothing (btDefault is not one: it is "global").
std::optional<BedType> bed_type_from_value(const std::string& value);
// "\"Cool Plate\", \"Textured PEI Plate\"": the offered values, for a refusal.
std::string listed_bed_types(const std::vector<BedType>& bed_types);

// Why apply_config cannot set the global bed type (curr_bed_type) to `value`, or nothing: a value that
// names no bed type, one the printer does not offer, or any change on a printer whose bed-type list the
// sidebar greys out (one bed type: neither Bambu Lab nor support_multi_bed_types). `parsed` is set.
std::optional<std::string> global_bed_type_refusal(const std::string& value, const std::vector<BedType>& offered, bool selectable,
                                                   BedType current, BedType& parsed);

// ---- Spiral vase ----

// The object settings the dialog's Enable gives every object on the plate (its Yes; its No cancels the
// vase), and so the object-wide settings a vase plate imposes: PartPlate::vase_mode_object_config.
const DynamicPrintConfig& vase_object_settings();
// The keys of those settings the object's config holds with the vase value.
std::vector<std::string> vase_settings_carried(const DynamicPrintConfig& object_config);
// The keys whose value differs between the two configs, or that one of them lacks.
std::vector<std::string> changed_config_keys(const DynamicPrintConfig& before, const DynamicPrintConfig& after);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
