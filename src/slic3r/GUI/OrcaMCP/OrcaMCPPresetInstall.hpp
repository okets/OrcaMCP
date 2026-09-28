// src/slic3r/GUI/OrcaMCP/OrcaMCPPresetInstall.hpp
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "OrcaMCPPresetConfigUtils.hpp" // PresetQuery
#include "OrcaMCPSliceProgress.hpp"

// Printers and filaments the user has not installed, and installing them, apart from the app, so it is
// tested without it (tests/slic3rutils/test_preset_install.cpp) on a data folder of the test's own. What
// the Setup Wizard offers and does: its printer and filament pages list the system presets of every
// vendor the app ships (GuideFrame::BuildProfileDataFromVendors), and its Finish installs the chosen ones
// through PresetBundle::apply_vendor_config -- the installer the cloud sync's load_pending_vendors uses
// too, whose merge mode adds to what is installed. get_presets {installed: false} and install_presets.

namespace Slic3r {
class PresetBundle;
namespace GUI { namespace OrcaMCP {

// A system printer preset, as the Setup Wizard's printer page offers it: a model and a nozzle.
struct CatalogPrinter
{
    std::string name;
    std::string vendor_id;   // the vendor's folder, which the app config lists it under ("BBL")
    std::string vendor_name; // its name ("Bambu Lab")
    std::string model;       // printer_model, the app config's model id
    std::string nozzle;      // printer_variant ("0.4")
    bool        installed = false;
};

// A system filament preset, as the Setup Wizard's filament page offers it.
struct CatalogFilament
{
    std::string name;
    std::string vendor; // its brand (filament_vendor), else its vendor profile's name
    std::string type;
    bool        installed  = false;
    bool        compatible = false; // with the selected printer
};

// Every system printer preset of `bundle` that names its model, in the bundle's order. Installed is the
// preset's visibility: its model and nozzle enabled in the app config. Any thread, for a bundle no other
// thread changes.
std::vector<CatalogPrinter> catalog_printers(const PresetBundle& bundle);

// Every system filament preset of `bundle`.
std::vector<CatalogFilament> catalog_filaments(const PresetBundle& bundle);

// The vendors the app ships (resources/profiles) that are not installed in the data folder, as the
// wizard names them, the filament library and profiles without a version (the blacklist) left out.
std::vector<std::string> uninstalled_vendors();

// The printers of vendor `vendor_id` as shipped, none installed: the vendor loaded into a bundle of its
// own with the installed filament library as its base, as the Setup Wizard reads a vendor that is not
// installed. Nothing, with `error`, when it cannot be read. Any thread: it touches no bundle but its own.
std::optional<std::vector<CatalogPrinter>> shipped_vendor_printers(const std::string& vendor_id, std::string& error);

// Of `vendors` (uninstalled ones), those a request could mean: the `vendor` it names (id or name, any
// case, a part of either), or with none named, those whose id begins a requested printer's name.
std::vector<std::string> vendors_to_search(const std::vector<std::string>& vendors, const std::optional<std::string>& vendor,
                                           const std::vector<std::string>& printers);

// What install_presets installs, in the shape apply_vendor_config takes.
struct PresetInstallPlan
{
    std::map<std::string, std::map<std::string, std::set<std::string>>> vendors;   // vendor id -> model -> nozzles
    std::map<std::string, std::string>                                  filaments; // preset name -> "true"
    std::vector<CatalogPrinter>                                         printers;
    std::vector<std::string>                                            filament_names;
    std::vector<std::string>                                            already_installed;

    bool installs_anything() const { return !vendors.empty() || !filaments.empty(); }
};

// The plan for `printers` and `filaments` (preset names) against what the catalogs know, or why not: a
// name it does not know, which says where to look.
std::optional<std::string> plan_preset_install(const std::vector<std::string>& printers, const std::vector<std::string>& filaments,
                                               const std::vector<CatalogPrinter>& known_printers,
                                               const std::vector<CatalogFilament>& known_filaments, PresetInstallPlan& plan);

// Why install_presets must not install now: slicing or exporting, a job (arrange, orient, bed fill), or
// unsaved preset changes (`unsaved`, one line per preset, "the print preset 'X' (layer_height)"), which
// the reload of every preset an install ends with would drop -- the Setup Wizard asks what to do with
// them first.
std::optional<std::string> install_refusal(const PipelineState& pipeline, bool ui_job_running, const std::vector<std::string>& unsaved);

// {name, vendor, vendor_id, printer_model, nozzle, installed} and {name, vendor, filament_type, installed},
// as get_presets {installed: false} lists them.
nlohmann::json catalog_printer_json(const CatalogPrinter& printer);
nlohmann::json catalog_filament_json(const CatalogFilament& filament);

// get_presets {installed: false}'s answer: of `printers` and `filaments`, those not installed that match
// `query` (its vendor and name_contains), filaments only when compatible with the selected printer, capped
// per type as get_presets caps; `type` "printer", "filament" or "" for both. With printers asked for,
// `vendors_not_installed`, and a hint to name one of them as vendor when none was (`vendor_named`).
nlohmann::json not_installed_json(const std::vector<CatalogPrinter>& printers, const std::vector<CatalogFilament>& filaments,
                                  const std::vector<std::string>& vendors_not_installed, const std::string& type,
                                  const PresetQuery& query);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
