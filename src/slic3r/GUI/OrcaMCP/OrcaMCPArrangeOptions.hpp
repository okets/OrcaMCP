// src/slic3r/GUI/OrcaMCP/OrcaMCPArrangeOptions.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The arrange menu's options (GLCanvas3D::_render_arrange_menu) as arrange_objects and
// fill_bed_with_instances take and report them: spacing, auto-rotate, several materials on one plate,
// align to Y, and on a Bambu printer that scans its first layer, keeping clear of the calibration area.
// Like the menu's, they are the app's: saved in its config (align to Y in memory only, as the menu keeps
// it), and every later arrange -- the GUI's, clone_object's, a bed fill's -- uses them. The menu keeps
// one set per mode: spacing and auto-rotate apart for printing by layer, by object and SLA, the others
// shared. Apart from the app, so they are tested without it (tests/slic3rutils/test_arrange_options.cpp);
// the tools are OrcaMCPArrangeTools.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// GLCanvas3D::ArrangeSettings, the options an arrange reads.
struct ArrangeOptions
{
    double spacing_mm               = 0.; // 0: automatic spacing
    bool   auto_rotate              = false;
    bool   allow_multiple_materials = true;
    bool   avoid_calibration_region = true;
    bool   align_to_y_axis          = false;

    bool operator==(const ArrangeOptions& other) const;
};

// The option set the menu edits, by the printer's technology and the global print sequence
// (GLCanvas3D::get_arrange_settings).
enum class ArrangeMode { by_layer, by_object, sla };

// The app-config keys (section "arrange") the menu writes and GLCanvas3D::load_arrange_settings reads:
// spacing and auto-rotate carry the mode's postfix, the other two none. Align to Y has no key.
struct ArrangeOptionKeys
{
    std::string spacing;
    std::string auto_rotate;
    std::string allow_multiple_materials;
    std::string avoid_calibration_region;
};
ArrangeOptionKeys arrange_option_keys(ArrangeMode mode);

// What a call asks of the options: the menu's Reset first when `reset`, then each option it gives.
struct ArrangeOptionsRequest
{
    bool                  reset = false;
    std::optional<double> spacing_mm;
    std::optional<bool>   auto_rotate;
    std::optional<bool>   allow_multiple_materials;
    std::optional<bool>   align_to_y_axis;
    std::optional<bool>   avoid_calibration_region;

    // Whether it asks anything.
    bool any() const;
};

// The options a call gives (the arguments arrange_option_properties declares); `error` says what is
// wrong with one given, and nothing is read then.
std::optional<ArrangeOptionsRequest> read_arrange_options(const nlohmann::json& params, std::string& error);

// `current` as the request leaves it, or why it cannot: a negative spacing, align to Y while auto-rotate
// is on (the menu greys it out then), the calibration-area option on a printer that does not offer it
// (`avoid_region_offered`). The menu's Reset gives `defaults`. Auto-rotate turned on turns align to Y
// off, as the menu does. Nothing is changed on a refusal.
std::optional<std::string> apply_arrange_options(const ArrangeOptionsRequest& request, const ArrangeOptions& defaults,
                                                 bool avoid_region_offered, ArrangeOptions& current);

// The names (as the arguments spell them) of the options whose value differs.
std::vector<std::string> changed_arrange_options(const ArrangeOptions& before, const ArrangeOptions& after);

// The options as the answers report them, in the arguments' names, with the print sequence whose set
// they are ("by layer", "by object", or "sla"); the calibration-area option only where it is offered.
nlohmann::json arrange_options_json(const ArrangeOptions& options, ArrangeMode mode, bool avoid_region_offered);

// The schema properties of those arguments, shared by arrange_objects and fill_bed_with_instances.
nlohmann::json arrange_option_properties();

}}} // namespace Slic3r::GUI::OrcaMCP
