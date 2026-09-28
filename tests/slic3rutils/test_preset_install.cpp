#include <catch2/catch_test_macros.hpp>

#include <boost/filesystem.hpp>
#include <fstream>
#include <iterator>
#include <string>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPresetInstall.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"

#include <nlohmann/json.hpp>

// Printers and filaments the user has not installed, and installing them, as the Setup Wizard does. Every
// test runs on a data folder and a resources folder of its own, with a small vendor of its own: nothing
// here reads or writes the user's data folder or the app's shipped profiles.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

struct TempDir
{
    fs::path path = fs::temp_directory_path() / fs::unique_path("orcamcp-install-test-%%%%-%%%%");
    TempDir() { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file.string()) << text;
}

// The filament library: one base filament and one a user can install.
void write_library(const fs::path& dir)
{
    const std::string lib(PresetBundle::ORCA_FILAMENT_LIBRARY);
    write(dir / (lib + ".json"),
          R"({"version":"1.0.0","name":")" + lib + R"(","filament_list":[)"
          R"({"name":"fdm_filament_common","sub_path":"filament/fdm_filament_common.json"},)"
          R"({"name":"Generic PLA @System","sub_path":"filament/generic_pla.json"}]})");
    write(dir / lib / "filament" / "fdm_filament_common.json",
          R"({"type":"filament","name":"fdm_filament_common","from":"system","instantiation":"false","filament_type":["PLA"]})");
    write(dir / lib / "filament" / "generic_pla.json",
          R"({"type":"filament","name":"Generic PLA @System","from":"system","instantiation":"true","inherits":"fdm_filament_common",)"
          R"("filament_id":"OGFL99","filament_vendor":["Generic"]})");
}

// Vendor Acme, not installed: one model, "Acme One", with a 0.4 and a 0.6 mm nozzle.
void write_acme(const fs::path& dir)
{
    write(dir / "Acme.json",
          R"({"version":"1.0.0","name":"Acme",)"
          R"("machine_model_list":[{"name":"Acme One","sub_path":"machine/Acme One.json"}],)"
          R"("machine_list":[{"name":"fdm_acme_common","sub_path":"machine/fdm_acme_common.json"},)"
          R"({"name":"Acme One 0.4 nozzle","sub_path":"machine/Acme One 0.4 nozzle.json"},)"
          R"({"name":"Acme One 0.6 nozzle","sub_path":"machine/Acme One 0.6 nozzle.json"}]})");
    write(dir / "Acme" / "machine" / "Acme One.json",
          R"({"type":"machine_model","name":"Acme One","nozzle_diameter":"0.4;0.6","default_materials":"Generic PLA @System"})");
    write(dir / "Acme" / "machine" / "fdm_acme_common.json",
          R"({"type":"machine","name":"fdm_acme_common","from":"system","instantiation":"false","printer_technology":"FFF"})");
    for (const std::string nozzle : {"0.4", "0.6"})
        write(dir / "Acme" / "machine" / ("Acme One " + nozzle + " nozzle.json"),
              R"({"type":"machine","name":"Acme One )" + nozzle + R"( nozzle","from":"system","instantiation":"true",)"
              R"("inherits":"fdm_acme_common","printer_model":"Acme One","printer_variant":")" + nozzle + R"(",)"
              R"("nozzle_diameter":[")" + nozzle + R"("]})");
}

// A data folder with only the filament library installed, and resources that ship it and Acme. The
// process-wide folders point at them for the test's length, however it ends.
struct InstallFolders
{
    TempDir     data, rsrc;
    fs::path    system   = data.path / PRESET_SYSTEM_DIR;
    fs::path    profiles = rsrc.path / "profiles";
    std::string prev_data{data_dir()}, prev_rsrc{resources_dir()};

    InstallFolders()
    {
        write_library(profiles);
        write_acme(profiles);
        write_library(system);
        set_data_dir(data.path.string());
        set_resources_dir(rsrc.path.string());
    }
    ~InstallFolders()
    {
        set_data_dir(prev_data);
        set_resources_dir(prev_rsrc);
    }
};

std::string slurp(const fs::path& file)
{
    std::ifstream in(file.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string> names_of(const std::vector<CatalogPrinter>& printers)
{
    std::vector<std::string> names;
    for (const CatalogPrinter& printer : printers)
        names.push_back(printer.name);
    return names;
}

} // namespace

TEST_CASE("the vendors the app ships and the user has not installed are listed, the filament library not", "[PresetInstall]")
{
    InstallFolders folders;
    CHECK(uninstalled_vendors() == std::vector<std::string>{"Acme"});
}

TEST_CASE("a vendor that is not installed offers its printers, a model and a nozzle each, none installed", "[PresetInstall]")
{
    InstallFolders folders;
    std::string    error;
    const auto     printers = shipped_vendor_printers("Acme", error);
    INFO(error);
    REQUIRE(printers.has_value());
    CHECK(names_of(*printers) == std::vector<std::string>{"Acme One 0.4 nozzle", "Acme One 0.6 nozzle"});
    for (const CatalogPrinter& printer : *printers) {
        CHECK(printer.vendor_id == "Acme");
        CHECK(printer.model == "Acme One");
        CHECK_FALSE(printer.installed);
    }
    CHECK((*printers)[1].nozzle == "0.6");

    CHECK_FALSE(shipped_vendor_printers("Nobody", error).has_value());
    CHECK(error.find("Nobody") != std::string::npos);
}

TEST_CASE("a printer's name, or the vendor named, says which vendors to read", "[PresetInstall]")
{
    const std::vector<std::string> vendors = {"Acme", "BBL", "Flashforge"};
    CHECK(vendors_to_search(vendors, std::nullopt, {"Flashforge AD5X 0.4 nozzle"}) == std::vector<std::string>{"Flashforge"});
    CHECK(vendors_to_search(vendors, std::nullopt, {"Bambu Lab A1 0.4 nozzle"}).empty()); // BBL's folder does not begin it
    CHECK(vendors_to_search(vendors, std::string("bbl"), {"Bambu Lab A1 0.4 nozzle"}) == std::vector<std::string>{"BBL"});
}

TEST_CASE("an install plan takes the printers and filaments named, as the app config lists them", "[PresetInstall]")
{
    const std::vector<CatalogPrinter> printers = {
        {"Acme One 0.4 nozzle", "Acme", "Acme", "Acme One", "0.4", true},
        {"Acme One 0.6 nozzle", "Acme", "Acme", "Acme One", "0.6", false},
    };
    const std::vector<CatalogFilament> filaments = {{"Generic PLA @System", "Generic", "PLA", true, true},
                                                    {"Generic PETG @System", "Generic", "PETG", false, true}};
    PresetInstallPlan plan;

    REQUIRE_FALSE(plan_preset_install({"Acme One 0.6 nozzle", "Acme One 0.6 nozzle"}, {"Generic PETG @System"}, printers, filaments, plan));
    CHECK(plan.vendors == std::map<std::string, std::map<std::string, std::set<std::string>>>{{"Acme", {{"Acme One", {"0.6"}}}}});
    CHECK(plan.filaments == std::map<std::string, std::string>{{"Generic PETG @System", "true"}});
    CHECK(plan.printers.size() == 1);
    CHECK(plan.installs_anything());

    // What is there already is said, and installs nothing.
    REQUIRE_FALSE(plan_preset_install({"Acme One 0.4 nozzle"}, {"Generic PLA @System"}, printers, filaments, plan));
    CHECK(plan.already_installed == std::vector<std::string>{"Acme One 0.4 nozzle", "Generic PLA @System"});
    CHECK_FALSE(plan.installs_anything());

    const auto unknown = plan_preset_install({"Acme Two 0.4 nozzle"}, {"Unobtainium"}, printers, filaments, plan);
    REQUIRE(unknown.has_value());
    CHECK(unknown->find("'Acme Two 0.4 nozzle'") != std::string::npos);
    CHECK(unknown->find("'Unobtainium'") != std::string::npos);
    CHECK(unknown->find("get_presets") != std::string::npos);
}

TEST_CASE("installing a printer lays its vendor into the data folder and enables that model and nozzle only", "[PresetInstall]")
{
    InstallFolders    folders;
    const std::string shipped_profile = slurp(folders.profiles / "Acme.json");
    std::string       error;
    const auto        shipped = shipped_vendor_printers("Acme", error);
    REQUIRE(shipped.has_value());
    PresetInstallPlan plan;
    REQUIRE_FALSE(plan_preset_install({"Acme One 0.6 nozzle"}, {}, *shipped, {}, plan));

    AppConfig    app_config;
    PresetBundle bundle;
    // The Setup Wizard's installer, in the merge mode the cloud sync uses: it adds to what is installed.
    REQUIRE(bundle.apply_vendor_config(plan.vendors, plan.filaments, &app_config, /*overwrite=*/false));

    CHECK(is_vendor_installed("Acme"));
    CHECK(fs::exists(folders.system / "Acme.json"));
    CHECK(app_config.get_variant("Acme", "Acme One", "0.6"));
    CHECK_FALSE(app_config.get_variant("Acme", "Acme One", "0.4"));

    const std::vector<CatalogPrinter> installed = catalog_printers(bundle);
    REQUIRE(names_of(installed) == std::vector<std::string>{"Acme One 0.4 nozzle", "Acme One 0.6 nozzle"});
    CHECK_FALSE(installed[0].installed);
    CHECK(installed[1].installed);

    // The shipped profiles are read, never written, and the vendor is no longer one to install.
    CHECK(slurp(folders.profiles / "Acme.json") == shipped_profile);
    CHECK(uninstalled_vendors().empty());
}

TEST_CASE("nothing is installed while slicing, while a job runs, or over unsaved preset changes", "[PresetInstall]")
{
    CHECK_FALSE(install_refusal(PipelineState{}, false, {}).has_value());

    PipelineState slicing;
    slicing.is_slicing = true;
    CHECK(install_refusal(slicing, false, {})->find("wait_for_slice") != std::string::npos);
    CHECK(install_refusal(PipelineState{}, true, {})->find("install_presets again") != std::string::npos);

    const std::string unsaved = *install_refusal(PipelineState{}, false, {"the print preset '0.20mm Standard' (layer_height)",
                                                                         "the filament preset 'Generic PLA' (nozzle_temperature)"});
    CHECK(unsaved.find("the print preset '0.20mm Standard' (layer_height) and the filament preset") != std::string::npos);
    CHECK(unsaved.find("save_preset") != std::string::npos);
    CHECK(unsaved.find("reset_preset") != std::string::npos);
}

TEST_CASE("get_presets lists what is not installed, printers and filaments apart", "[PresetInstall]")
{
    const std::vector<CatalogPrinter> printers = {
        {"Acme One 0.4 nozzle", "Acme", "Acme", "Acme One", "0.4", true},
        {"Acme One 0.6 nozzle", "Acme", "Acme", "Acme One", "0.6", false},
        {"Bambu Lab A1 0.4 nozzle", "BBL", "Bambu Lab", "Bambu Lab A1", "0.4", false},
    };
    const std::vector<CatalogFilament> filaments = {{"Generic PLA @System", "Generic", "PLA", true, true},
                                                    {"Generic PETG @System", "Generic", "PETG", false, true},
                                                    {"Acme PLA @Acme Two", "Acme", "PLA", false, false}};
    GUI::PresetQuery query;

    const json all = not_installed_json(printers, filaments, {"Zeta"}, "", query);
    REQUIRE(all["printerPresets"].size() == 2); // the installed one left out
    CHECK(all["printerPresets"][0] == json{{"name", "Acme One 0.6 nozzle"}, {"vendor", "Acme"}, {"vendor_id", "Acme"},
                                           {"printer_model", "Acme One"}, {"nozzle", "0.6"}, {"installed", false}});
    // Only a filament that suits the selected printer, and is not installed.
    REQUIRE(all["filamentPresets"].size() == 1);
    CHECK(all["filamentPresets"][0]["name"] == "Generic PETG @System");
    CHECK(all["vendors_not_installed"] == json::array({"Zeta"}));
    CHECK(all["query"]["installed"] == false);
    CHECK(all["hint"].get<std::string>().find("vendor names it") != std::string::npos);

    // The vendor filter reads a printer's vendor by name or folder.
    query.vendor     = "bbl";
    const json bambu = not_installed_json(printers, filaments, {}, "printer", query);
    REQUIRE(bambu["printerPresets"].size() == 1);
    CHECK(bambu["printerPresets"][0]["vendor"] == "Bambu Lab");
    CHECK_FALSE(bambu.contains("filamentPresets"));

    // Capped per type, as get_presets caps, saying how many matched.
    query        = GUI::PresetQuery();
    query.limit  = 1;
    const json capped = not_installed_json(printers, filaments, {}, "printer", query);
    CHECK(capped["printerPresets"].size() == 1);
    CHECK(capped["query"]["counts"]["printerPresets"] == 2);
    CHECK(capped["query"]["truncated"] == true);
}

TEST_CASE("install_presets and get_presets installed: false refuse a call they could not act on, before the app", "[PresetInstall][orcamcp]")
{
    auto answer = [](const std::string& tool, const json& arguments) {
        const json result = GUI::OrcaMCPServer::handle_tools_call({{"name", tool}, {"arguments", arguments}});
        return json::parse(result.at("content").at(0).at("text").get<std::string>());
    };
    CHECK(answer("install_presets", json::object())["message"].get<std::string>().find("needs something to install") != std::string::npos);
    CHECK(answer("install_presets", {{"printers", "Acme One 0.4 nozzle"}})["message"] == "printers must be an array of preset names");
    CHECK(answer("install_presets", {{"filaments", {""}}})["message"].get<std::string>().find("filaments must be an array") != std::string::npos);
    CHECK(answer("install_presets", {{"printers", {"X"}}, {"vendor", ""}})["message"].get<std::string>().find("vendor must be") != std::string::npos);

    CHECK(answer("get_presets", {{"installed", false}, {"summary", false}})["message"].get<std::string>().find("names only") != std::string::npos);
    CHECK(answer("get_presets", {{"installed", false}, {"type", "print"}})["message"].get<std::string>().find("printer, filament or all") != std::string::npos);
    CHECK(answer("get_presets", {{"installed", "no"}})["message"] == "installed must be a boolean");
}
