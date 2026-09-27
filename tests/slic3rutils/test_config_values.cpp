#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPConfigValues.hpp"
#include "libslic3r/Preset.hpp"

// get_config_values: which presets are selected, and just the settings a caller names. The
// alternative, get_edited_presets, answered a 12-key question with 25-48 KB -- including a
// 12,470-character start G-code -- so an agent could not afford to check its settings as it worked.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// A preset config holding exactly the keys of one preset type, at their defaults.
DynamicPrintConfig preset_config(const std::vector<std::string>& keys)
{
    DynamicPrintConfig config;
    config.apply_only(DynamicPrintConfig::full_print_config(), keys);
    return config;
}

// A preset of `type` named `name`, holding exactly that type's keys, at their defaults.
Preset preset_of_type(Preset::Type type, const std::string& name, const std::vector<std::string>& keys)
{
    Preset preset(type, name);
    preset.config = preset_config(keys);
    return preset;
}

// A printer, a process, and two filament slots: slot 1 PLA, slot 2 PETG, which the Filament tab
// edits. Each test changes what it needs and reads the sources back through sources(); a preset's
// dirty flag is the GUI's own (PresetCollection::is_dirty), as config_sources_from reads it.
struct SelectedPresets
{
    Preset             print_saved    = preset_of_type(Preset::TYPE_PRINT, "0.20mm Standard", Preset::print_options());
    Preset             print_edited   = print_saved;
    Preset             printer_saved  = preset_of_type(Preset::TYPE_PRINTER, "Creator 5 Pro 0.4", Preset::printer_options());
    Preset             printer_edited = printer_saved;
    Preset             pla            = preset_of_type(Preset::TYPE_FILAMENT, "Generic PLA", Preset::filament_options());
    Preset             petg_saved     = preset_of_type(Preset::TYPE_FILAMENT, "Generic PETG", Preset::filament_options());
    Preset             petg_edited    = petg_saved;
    DynamicPrintConfig project;

    SelectedPresets()
    {
        petg_saved.config.set_deserialize_strict("filament_type", "PETG");
        petg_edited = petg_saved;
        project.set_deserialize_strict("filament_colour", "#FF0000;#00FF00");
    }

    static PresetConfigs selected(const Preset& edited, const Preset& saved)
    {
        return {edited.name, &edited, &saved, PresetCollection::is_dirty(&edited, &saved)};
    }

    ConfigSources sources() const
    {
        ConfigSources sources;
        sources.print   = selected(print_edited, print_saved);
        sources.printer = selected(printer_edited, printer_saved);
        sources.filaments.push_back({pla.name, &pla, &pla, false});
        sources.filaments.push_back(selected(petg_edited, petg_saved));
        sources.project = &project;
        return sources;
    }
};

const std::vector<std::string> twelve_support_keys = {
    "enable_support",          "support_type",          "support_style",
    "support_threshold_angle", "support_on_build_plate_only", "support_top_z_distance",
    "support_bottom_z_distance", "support_interface_top_layers", "support_interface_bottom_layers",
    "support_interface_spacing", "support_base_pattern_spacing", "support_object_xy_distance"};

} // namespace

TEST_CASE("with no keys, the report is the selected presets and whether each has unsaved changes", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    presets.print_edited.config.set_deserialize_strict("support_type", "tree(auto)");
    const nlohmann::json selected = selected_presets_json(presets.sources());

    CHECK(selected["printer"]["name"] == "Creator 5 Pro 0.4");
    CHECK(selected["printer"]["dirty"] == false);
    CHECK(selected["print"]["name"] == "0.20mm Standard");
    CHECK(selected["print"]["dirty"] == true);
    REQUIRE(selected["filaments"].size() == 2);
    CHECK(selected["filaments"][0] == nlohmann::json{{"slot", 1}, {"name", "Generic PLA"}, {"dirty", false}});
    CHECK(selected["filaments"][1]["name"] == "Generic PETG");
}

TEST_CASE("the selected presets cost well under 1 KB", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    CHECK(selected_presets_json(presets.sources()).dump().size() < 512);
}

TEST_CASE("twelve support settings cost well under 1 KB", "[orcamcp][ConfigValues]")
{
    SelectedPresets      presets;
    const nlohmann::json report = config_values_json(presets.sources(), twelve_support_keys, /*dirty_only=*/false);

    CHECK(report["values"]["print"].size() == twelve_support_keys.size());
    CHECK(report.dump().size() < 1024);
}

TEST_CASE("each setting is grouped under the preset type apply_config takes for it", "[orcamcp][ConfigValues]")
{
    SelectedPresets      presets;
    const nlohmann::json report =
        config_values_json(presets.sources(), {"support_type", "filament_type", "nozzle_diameter", "filament_colour"}, false);

    CHECK(report["values"]["print"].contains("support_type"));
    CHECK(report["values"]["filament"].contains("filament_type"));
    CHECK(report["values"]["printer"].contains("nozzle_diameter"));
    CHECK(report["values"]["project"]["filament_colour"] == "#FF0000;#00FF00");
}

TEST_CASE("a filament setting has one value per slot", "[orcamcp][ConfigValues]")
{
    SelectedPresets      presets;
    const nlohmann::json report = config_values_json(presets.sources(), {"filament_type"}, false);
    CHECK(report["values"]["filament"]["filament_type"] == nlohmann::json::array({"PLA", "PETG"}));
}

TEST_CASE("a setting changed from its saved preset is dirty, with the saved value", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    presets.print_saved.config.set_deserialize_strict("support_type", "normal(auto)");
    presets.print_edited.config.set_deserialize_strict("support_type", "tree(auto)");
    const nlohmann::json report = config_values_json(presets.sources(), {"support_type", "enable_support"}, false);

    CHECK(report["values"]["print"]["support_type"] == "tree(auto)");
    CHECK(report["dirty"]["print"]["support_type"]["saved"] == "normal(auto)");
    CHECK_FALSE(report["dirty"]["print"].contains("enable_support"));
}

TEST_CASE("a read with nothing changed carries no dirty section", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    CHECK_FALSE(config_values_json(presets.sources(), twelve_support_keys, false).contains("dirty"));
}

TEST_CASE("a key only the edited config carries is not an unsaved change, as the GUI judges it", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    presets.printer_edited.config.set_deserialize_strict("extruder_nozzle_stats", "Standard#1");
    REQUIRE_FALSE(presets.printer_saved.config.has("extruder_nozzle_stats"));

    CHECK_FALSE(config_values_json(presets.sources(), {"extruder_nozzle_stats"}, false).contains("dirty"));
    CHECK_FALSE(unsaved_changes_json(presets.sources()).contains("printer"));
}

TEST_CASE("dirty_only keeps just the settings that differ from the saved preset", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    presets.print_edited.config.set_deserialize_strict("support_threshold_angle", "40");
    const nlohmann::json report = config_values_json(presets.sources(), twelve_support_keys, /*dirty_only=*/true);

    CHECK(report["values"]["print"].size() == 1);
    CHECK(report["values"]["print"]["support_threshold_angle"] == "40");
}

TEST_CASE("a changed filament setting names the slots whose preset is being edited", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    presets.petg_edited.config.set_deserialize_strict("filament_type", "PCTG");
    const nlohmann::json report = config_values_json(presets.sources(), {"filament_type"}, false);

    CHECK(report["values"]["filament"]["filament_type"] == nlohmann::json::array({"PLA", "PCTG"}));
    CHECK(report["dirty"]["filament"]["filament_type"]["slots"] == nlohmann::json::array({2}));
    CHECK(report["dirty"]["filament"]["filament_type"]["saved"] == "PETG");
}

TEST_CASE("a known key no selected preset carries is listed apart", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    const ConfigSources sources = presets.sources();
    REQUIRE_FALSE(presets.print_edited.config.has("extruder"));
    REQUIRE_FALSE(presets.printer_edited.config.has("extruder"));
    REQUIRE_FALSE(presets.pla.config.has("extruder"));
    REQUIRE_FALSE(presets.project.has("extruder"));

    const nlohmann::json report = config_values_json(sources, {"extruder", "support_type"}, false);
    CHECK(report["not_in_presets"] == nlohmann::json::array({"extruder"}));
    CHECK(report["values"]["print"].contains("support_type"));
}

TEST_CASE("unknown keys are the ones the slicer does not define", "[orcamcp][ConfigValues]")
{
    CHECK(unknown_config_keys({"support_type", "no_such_setting", "sparse_infil_density", "wall_loops"}) ==
          std::vector<std::string>{"no_such_setting", "sparse_infil_density"});
    CHECK(unknown_config_keys({"support_type"}).empty());
}

TEST_CASE("a print-host credential is reported as set or empty, never by value", "[orcamcp][ConfigValues]")
{
    DynamicPrintConfig config = preset_config(Preset::printer_options());
    config.set_deserialize_strict("printhost_apikey", "s3cret-key");
    CHECK(reported_config_value(config, "printhost_apikey") == "<redacted>");
    config.set_deserialize_strict("printhost_apikey", "");
    CHECK(reported_config_value(config, "printhost_apikey").empty());
}

TEST_CASE("every unsaved change is listed with its value and its saved value", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    presets.print_saved.config.set_deserialize_strict("support_type", "normal(auto)");
    presets.print_edited.config.set_deserialize_strict("support_type", "tree(auto)");
    presets.petg_edited.config.set_deserialize_strict("filament_type", "PCTG");
    const nlohmann::json changes = unsaved_changes_json(presets.sources());

    CHECK(changes["print"].size() == 1);
    CHECK(changes["print"]["support_type"] == nlohmann::json{{"value", "tree(auto)"}, {"saved", "normal(auto)"}});
    CHECK(changes["filament"]["filament_type"]["slots"] == nlohmann::json::array({2}));
    CHECK(changes["filament"]["filament_type"]["value"] == "PCTG");
    CHECK_FALSE(changes.contains("printer"));
}

TEST_CASE("an optional key one side lacks is dirty, as the GUI marks it", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    presets.print_saved.config.erase("compatible_printers");
    presets.print_edited.config.set_deserialize_strict("compatible_printers", "Creator 5 Pro 0.4");
    const nlohmann::json report = config_values_json(presets.sources(), {"compatible_printers"}, false);

    CHECK(report["dirty"]["print"]["compatible_printers"]["saved"].is_null());
    CHECK(unsaved_changes_json(presets.sources())["print"].contains("compatible_printers"));
}

TEST_CASE("a changed credential is reported dirty without either value", "[orcamcp][ConfigValues]")
{
    SelectedPresets presets;
    presets.printer_saved.config.set_deserialize_strict("printhost_apikey", "old-credential");
    presets.printer_edited.config.set_deserialize_strict("printhost_apikey", "new-credential");

    const nlohmann::json report = config_values_json(presets.sources(), {"printhost_apikey"}, false);
    CHECK(report["values"]["printer"]["printhost_apikey"] == "<redacted>");
    CHECK(report["dirty"]["printer"]["printhost_apikey"]["saved"] == "<redacted>");

    const nlohmann::json changes = unsaved_changes_json(presets.sources());
    CHECK(changes["printer"]["printhost_apikey"] == nlohmann::json{{"value", "<redacted>"}, {"saved", "<redacted>"}});

    const std::string everything = report.dump() + changes.dump();
    CHECK(everything.find("old-credential") == std::string::npos);
    CHECK(everything.find("new-credential") == std::string::npos);
}
