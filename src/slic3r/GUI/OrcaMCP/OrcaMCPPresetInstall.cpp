// src/slic3r/GUI/OrcaMCP/OrcaMCPPresetInstall.cpp
#include "OrcaMCPPresetInstall.hpp"
#include "OrcaMCPUiJob.hpp"

#include <algorithm>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

std::string config_string(const Preset& preset, const char* key)
{
    const auto* value = preset.config.option<ConfigOptionString>(key);
    return value != nullptr ? value->value : std::string();
}

std::string first_of(const Preset& preset, const char* key)
{
    const auto* values = preset.config.option<ConfigOptionStrings>(key);
    return values != nullptr && !values->values.empty() ? values->values.front() : std::string();
}

fs::path shipped_profiles() { return fs::path(resources_dir()) / "profiles"; }

bool contains_nocase(const std::string& text, const std::string& part)
{
    return boost::algorithm::icontains(text, part);
}

template<class Entry, class Name> const Entry* find_by_name(const std::vector<Entry>& entries, const std::string& name, Name name_of)
{
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return name_of(entry) == name; });
    return it == entries.end() ? nullptr : &*it;
}

// "a", "a and b", "a, b and c"
std::string listed(const std::vector<std::string>& items)
{
    std::string text;
    for (std::size_t i = 0; i < items.size(); ++i)
        text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    return text;
}

std::string quoted_list(const std::vector<std::string>& names)
{
    std::string text;
    for (const std::string& name : names)
        text += (text.empty() ? "'" : ", '") + name + "'";
    return text;
}

} // namespace

std::vector<CatalogPrinter> catalog_printers(const PresetBundle& bundle)
{
    std::vector<CatalogPrinter> printers;
    for (const Preset& preset : bundle.printers) {
        if (!preset.is_system || preset.vendor == nullptr)
            continue;
        const std::string model = config_string(preset, "printer_model");
        if (model.empty())
            continue;
        printers.push_back({preset.name, preset.vendor->id, preset.vendor->name, model, config_string(preset, "printer_variant"),
                            preset.is_visible});
    }
    return printers;
}

std::vector<CatalogFilament> catalog_filaments(const PresetBundle& bundle)
{
    std::vector<CatalogFilament> filaments;
    for (const Preset& preset : bundle.filaments) {
        if (!preset.is_system)
            continue;
        std::string vendor = first_of(preset, "filament_vendor");
        if (vendor.empty() && preset.vendor != nullptr)
            vendor = preset.vendor->name;
        filaments.push_back({preset.name, vendor, first_of(preset, "filament_type"), preset.is_visible, preset.is_compatible});
    }
    return filaments;
}

std::vector<std::string> uninstalled_vendors()
{
    std::vector<std::string> vendors;
    boost::system::error_code ec;
    if (!fs::is_directory(shipped_profiles(), ec))
        return vendors;
    for (const std::string& name : vendor_names_in(shipped_profiles()))
        // The filament library is every install's base, and a profile without a version (the blacklist)
        // carries no presets: the wizard offers neither.
        if (name != PresetBundle::ORCA_FILAMENT_LIBRARY && resource_vendor_version(name).valid() && !is_vendor_installed(name))
            vendors.push_back(name);
    return vendors;
}

std::optional<std::vector<CatalogPrinter>> shipped_vendor_printers(const std::string& vendor_id, std::string& error)
{
    const std::string library_name(PresetBundle::ORCA_FILAMENT_LIBRARY);
    const fs::path    library_dir = is_vendor_installed(library_name) ? fs::path(data_dir()) / PRESET_SYSTEM_DIR : shipped_profiles();
    try {
        // The library first, as the wizard loads it: the vendor's filaments inherit from it.
        PresetBundle library;
        library.load_vendor_configs_from_json(library_dir.string(), library_name, PresetBundle::LoadSystem,
                                              ForwardCompatibilitySubstitutionRule::EnableSilent);
        PresetBundle vendor;
        vendor.load_vendor_configs_from_json(shipped_profiles().string(), vendor_id, PresetBundle::LoadSystem,
                                             ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
        // No app config marked these visible or not: the vendor is not installed, so none of them is.
        std::vector<CatalogPrinter> printers = catalog_printers(vendor);
        for (CatalogPrinter& printer : printers)
            printer.installed = false;
        return printers;
    } catch (const std::exception& e) {
        error = "The shipped profiles of vendor '" + vendor_id + "' could not be read: " + e.what();
        return std::nullopt;
    }
}

std::vector<std::string> vendors_to_search(const std::vector<std::string>& vendors, const std::optional<std::string>& vendor,
                                           const std::vector<std::string>& printers)
{
    std::vector<std::string> chosen;
    for (const std::string& id : vendors) {
        const bool named  = vendor && contains_nocase(id, *vendor);
        const bool begins = !vendor && std::any_of(printers.begin(), printers.end(),
                                                   [&id](const std::string& printer) { return boost::algorithm::istarts_with(printer, id); });
        if (named || begins)
            chosen.push_back(id);
    }
    return chosen;
}

std::optional<std::string> plan_preset_install(const std::vector<std::string>& printers, const std::vector<std::string>& filaments,
                                               const std::vector<CatalogPrinter>& known_printers,
                                               const std::vector<CatalogFilament>& known_filaments, PresetInstallPlan& plan)
{
    plan = PresetInstallPlan();
    std::vector<std::string> unknown_printers, unknown_filaments;
    for (const std::string& name : printers) {
        const CatalogPrinter* printer = find_by_name(known_printers, name, [](const CatalogPrinter& p) { return p.name; });
        if (printer == nullptr) {
            unknown_printers.push_back(name);
        } else if (printer->installed) {
            plan.already_installed.push_back(name);
        } else if (plan.vendors[printer->vendor_id][printer->model].insert(printer->nozzle).second) {
            plan.printers.push_back(*printer);
        }
    }
    for (const std::string& name : filaments) {
        const CatalogFilament* filament = find_by_name(known_filaments, name, [](const CatalogFilament& f) { return f.name; });
        if (filament == nullptr) {
            unknown_filaments.push_back(name);
        } else if (filament->installed) {
            plan.already_installed.push_back(name);
        } else if (plan.filaments.emplace(name, "true").second) {
            plan.filament_names.push_back(name);
        }
    }
    if (unknown_printers.empty() && unknown_filaments.empty())
        return std::nullopt;

    std::string message = "Nothing was installed.";
    if (!unknown_printers.empty())
        message += " No printer preset named " + quoted_list(unknown_printers) +
                   ": a printer is named exactly as get_presets {installed: false, type: printer, vendor} lists it, and a vendor "
                   "whose folder does not begin the name (Bambu Lab's is BBL) needs vendor.";
    if (!unknown_filaments.empty())
        message += " No filament preset named " + quoted_list(unknown_filaments) +
                   ": get_presets {installed: false, type: filament} lists those of the installed vendors; a vendor's own "
                   "filaments come with its printer.";
    return message;
}

std::optional<std::string> install_refusal(const PipelineState& pipeline, bool ui_job_running, const std::vector<std::string>& unsaved,
                                           const std::vector<std::string>& embedded)
{
    if (pipeline_busy(pipeline) != PipelineBusy::idle)
        return pipeline_busy_text(pipeline) + ", so nothing was installed: call wait_for_slice, then install_presets again";
    if (ui_job_running)
        return ui_job_busy_message("install_presets");
    if (!embedded.empty())
        return "Installing reloads every preset, which drops the presets this project carries of its own: " + listed(embedded) +
               ". Nothing was installed. save_project keeps them in the project file: save it, start a new project "
               "(new_project), install, then load_project the file again, and they come back with it. (save_preset with a "
               "name keeps one as your own preset too.)";
    if (!unsaved.empty())
        return "Installing reloads every preset, which drops unsaved changes, and " + listed(unsaved) +
               (unsaved.size() == 1 ? " has some" : " have some") + ": save_preset keeps them, reset_preset drops them. Nothing was installed";
    return std::nullopt;
}

std::vector<std::string> restore_filament_maps(const DynamicPrintConfig& before, DynamicPrintConfig& after)
{
    std::vector<std::string> restored;
    const auto* slots_before = before.option<ConfigOptionStrings>("filament_colour");
    const auto* slots_after  = after.option<ConfigOptionStrings>("filament_colour");
    if (slots_before == nullptr || slots_after == nullptr || slots_before->values.size() != slots_after->values.size())
        return restored;
    for (const char* key : {"filament_map", "filament_nozzle_map", "filament_volume_map"}) {
        const auto* was = before.option<ConfigOptionInts>(key);
        auto*       now = after.option<ConfigOptionInts>(key);
        if (was == nullptr || now == nullptr || was->values == now->values || was->values.size() != slots_before->values.size())
            continue;
        now->values = was->values;
        restored.push_back(key);
    }
    return restored;
}

json catalog_printer_json(const CatalogPrinter& printer)
{
    return {{"name", printer.name},       {"vendor", printer.vendor_name}, {"vendor_id", printer.vendor_id},
            {"printer_model", printer.model}, {"nozzle", printer.nozzle},      {"installed", printer.installed}};
}

json catalog_filament_json(const CatalogFilament& filament)
{
    return {{"name", filament.name}, {"vendor", filament.vendor}, {"filament_type", filament.type}, {"installed", filament.installed}};
}

json not_installed_json(const std::vector<CatalogPrinter>& printers, const std::vector<CatalogFilament>& filaments,
                        const std::vector<std::string>& vendors_not_installed, const std::string& type, const PresetQuery& query)
{
    const int limit = preset_query_effective_limit(query.limit, true);
    std::map<std::string, PresetListCount> counts;
    json                                   result = json::object();
    auto list = [&](const std::string& key, auto&& entries, auto&& vendor_of, auto&& wanted, auto&& to_json) {
        json             listed = json::array();
        PresetListCount& count  = counts[key];
        for (const auto& entry : entries) {
            if (!wanted(entry) || !preset_query_matches(entry.name, vendor_of(entry), query))
                continue;
            ++count.matched;
            if (limit > 0 && count.returned >= limit)
                continue;
            ++count.returned;
            listed.push_back(to_json(entry));
        }
        result[key] = listed;
    };
    if (type.empty() || type == "printer") {
        list("printerPresets", printers, [](const CatalogPrinter& p) { return p.vendor_name + " " + p.vendor_id; },
             [](const CatalogPrinter& p) { return !p.installed; }, catalog_printer_json);
        result["vendors_not_installed"] = vendors_not_installed;
    }
    if (type.empty() || type == "filament")
        list("filamentPresets", filaments, [](const CatalogFilament& f) { return f.vendor; },
             [](const CatalogFilament& f) { return !f.installed && f.compatible; }, catalog_filament_json);

    json matched = json::object(), returned = json::object();
    for (const auto& [key, count] : counts) {
        matched[key]  = count.matched;
        returned[key] = count.returned;
    }
    result["query"] = {{"type", type.empty() ? json(nullptr) : json(type)},
                       {"installed", false},
                       {"vendor", query.vendor},
                       {"name_contains", query.name_contains},
                       {"limit", limit},
                       {"counts", matched},
                       {"returned", returned},
                       {"truncated", preset_list_truncated(counts)}};
    std::vector<std::string> hints;
    if (const std::string truncation = preset_truncation_hint(counts); !truncation.empty())
        hints.push_back(truncation);
    if ((type.empty() || type == "printer") && query.vendor.empty() && !vendors_not_installed.empty())
        hints.push_back("Printers of a vendor not installed (vendors_not_installed) are listed when vendor names it.");
    hints.push_back("install_presets installs them by name.");
    std::string hint;
    for (const std::string& line : hints)
        hint += (hint.empty() ? "" : " ") + line;
    result["hint"] = hint;
    return result;
}

}}} // namespace Slic3r::GUI::OrcaMCP
