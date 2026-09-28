#ifndef slic3r_CustomGCode_hpp_
#define slic3r_CustomGCode_hpp_

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r {

class DynamicPrintConfig;

namespace CustomGCode {

enum Type
{
    ColorChange,
    PausePrint,
    ToolChange,
    Template,
    Custom,
    Unknown,
};

struct Item
{
    bool operator<(const Item& rhs) const { return this->print_z < rhs.print_z; }
    bool operator==(const Item& rhs) const
    {
        return (rhs.print_z   == this->print_z    ) &&
               (rhs.type      == this->type       ) &&
               (rhs.extruder  == this->extruder   ) &&
               (rhs.color     == this->color      ) &&
               (rhs.extra     == this->extra      );
    }
    bool operator!=(const Item& rhs) const { return ! (*this == rhs); }
    
    double      print_z;
    Type        type;
    int         extruder;   // Informative value for ColorChangeCode and ToolChangeCode
                            // "gcode" == ColorChangeCode   => M600 will be applied for "extruder" extruder
                            // "gcode" == ToolChangeCode    => for whole print tool will be switched to "extruder" extruder
    std::string color;      // if gcode is equal to PausePrintCode, 
                            // this field is used for save a short message shown on Printer display 
    std::string extra;      // this field is used for the extra data like :
                            // - G-code text for the Type::Custom 
                            // - message text for the Type::PausePrint
    void from_json(const nlohmann::json& j) {
        std::string type_str;
        j.at("type").get_to(type_str);
        std::map<std::string,Type> str2type = { {"ColorChange", ColorChange },
            {"PausePrint",PausePrint},
            {"ToolChange",ToolChange},
            {"Template",Template},
            {"Custom",Custom},
            {"Unknown",Unknown} };
        type = Unknown;
        if (str2type.find(type_str) != str2type.end())
            type = str2type[type_str];
        j.at("print_z").get_to(print_z);
        j.at("color").get_to(color);
        j.at("extruder").get_to(extruder);
        if(j.contains("extra"))
            j.at("extra").get_to(extra);
    }
};

enum Mode
{
    Undef,
    SingleExtruder,   // Single extruder printer preset is selected
    MultiAsSingle,    // Multiple extruder printer preset is selected, but 
                      // this mode works just for Single extruder print 
                      // (The same extruder is assigned to all ModelObjects and ModelVolumes).
    MultiExtruder     // Multiple extruder printer preset is selected
};

// string anlogue of custom_code_per_height mode
static constexpr char SingleExtruderMode[] = "SingleExtruder";
static constexpr char MultiAsSingleMode [] = "MultiAsSingle";
static constexpr char MultiExtruderMode [] = "MultiExtruder";

struct Info
{
    Mode mode = Undef;
    std::vector<Item> gcodes;

    bool operator==(const Info& rhs) const
    {
        return  (rhs.mode   == this->mode   ) &&
                (rhs.gcodes == this->gcodes );
    }
    bool operator!=(const Info& rhs) const { return !(*this == rhs); }

    void from_json(const nlohmann::json& j) {
        std::string mode_str;
        if (j.contains("mode"))
            j.at("mode").get_to(mode_str);
        if (mode_str == "SingleExtruder") mode = SingleExtruder;
        else if (mode_str == "MultiAsSingle") mode = MultiAsSingle;
        else if (mode_str == "MultiExtruder") mode = MultiExtruder;
        else mode = Undef;

        auto j_gcodes = j.at("gcodes");
        gcodes.reserve(j_gcodes.size());
        for (auto& jj : j_gcodes) {
            Item item;
            item.from_json(jj);
            gcodes.push_back(item);
        }
    }
};

// If loaded configuration has a "colorprint_heights" option (if it was imported from older Slicer), 
// and if CustomGCode::Info.gcodes is empty (there is no color print data available in a new format
// then CustomGCode::Info.gcodes should be updated considering this option.
//BBS
//extern void update_custom_gcode_per_print_z_from_config(Info& info, DynamicPrintConfig* config);

// If information for custom Gcode per print Z was imported from older Slicer, mode will be undefined.
// So, we should set CustomGCode::Info.mode should be updated considering code values from items.
extern void check_mode_for_custom_gcode_per_print_z(Info& info);

// Return pairs of <print_z, 1-based extruder ID> sorted by increasing print_z from custom_gcode_per_print_z.
// print_z corresponds to the first layer printed with the new extruder.
std::vector<std::pair<double, unsigned int>> custom_tool_changes(const Info& custom_gcode_per_print_z, size_t num_extruders);

// Orca: why the slicer does not take a plate's filament changes (its ToolChange items) as filament switches, or
// none when it does: only on a print by layer, out of spiral vase mode, on a printer of several filaments, whose
// objects all print with one filament, recorded in MultiAsSingle mode (ToolOrdering). Otherwise they write nothing:
// the slicer turns none into a color change (ToolOrdering::assign_custom_gcodes skips every filament change). One rule
// for the slicer, the filaments a plate lists (Print::extruders, PartPlate::get_extruders), the Preview's slider and MCP.
enum class ToolChangesOff
{
    none,
    by_object,         // the plate prints one object after another
    spiral_vase,       // the plate prints in spiral vase mode
    one_filament,      // the project has one filament: there is nothing to switch to
    several_filaments, // the plate's objects print with several filaments
    other_mode,        // recorded in another mode (an older project's)
    same_filament,     // one change only (tool_change_effects): it names the filament already printing there
};
ToolChangesOff tool_changes_off(Mode mode, size_t num_filaments, size_t object_filaments, bool by_layer, bool spiral_vase);
inline bool    tool_changes_apply(Mode mode, size_t num_filaments, size_t object_filaments, bool by_layer, bool spiral_vase)
{
    return tool_changes_off(mode, num_filaments, object_filaments, by_layer, spiral_vase) == ToolChangesOff::none;
}
// Whether the Preview's slider hides the plate's filament changes: by object, in vase mode, or on a plate that
// prints with several filaments, where they write nothing. It shows the rest -- on a project of one filament, or
// recorded in another mode, they write nothing either, as upstream shows them -- so it never hides one that prints.
inline bool tool_changes_hidden(ToolChangesOff off)
{
    return off == ToolChangesOff::by_object || off == ToolChangesOff::spiral_vase || off == ToolChangesOff::several_filaments;
}
// Orca: the filament a filament change naming `named` switches to, as the slicer takes it: a slot the printer lacks is
// filament 1 (custom_tool_changes), and 0 or less the objects' own (no override, ToolOrdering::collect_extruders).
int tool_change_target(int named, size_t num_filaments, int objects_own);
// What each of a plate's filament changes writes in the G-code, one entry per item of `info` (none for an item that is
// no filament change): none where it switches the filament, else why it writes nothing -- the plate's reason
// (tool_changes_off), or same_filament, a change to the filament already printing there: the objects' own below the
// first change, the previous change's above it, as ToolOrdering::collect_extruders takes them in height order (a slot
// the printer lacks is filament 1, and 0 the objects' own, as custom_tool_changes and the override read them).
// `object_filaments`: the filaments the plate's objects print, 1-based, as Print::object_extruders counts them.
std::vector<ToolChangesOff> tool_change_effects(const Info& info, size_t num_filaments, const std::vector<int>& object_filaments,
                                                bool by_layer, bool spiral_vase);

} // namespace CustomGCode

} // namespace Slic3r



#endif /* slic3r_CustomGCode_hpp_ */
