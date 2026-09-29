#include <catch2/catch_test_macros.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/PrintHostDialogs.hpp"

#include <string>

// What the Flashforge send dialog starts with: every print option off until a box was ticked, then
// the boxes as the user last left them -- each is saved the moment it is toggled, so Cancel, the close
// box and Send all keep it. The material station starts on when the printer has one, then as last left.

using Slic3r::AppConfig;
using Slic3r::FlashforgeApi::PrintOptions;
using Slic3r::GUI::remember_flashforge_material_station;
using Slic3r::GUI::remember_flashforge_print_options;
using Slic3r::GUI::remembered_flashforge_material_station;
using Slic3r::GUI::remembered_flashforge_print_options;
using Slic3r::GUI::sent_flashforge_print_options;

TEST_CASE("The Flashforge send dialog starts with every print option off", "[FlashforgeSendOptions]")
{
    const AppConfig config;
    CHECK(remembered_flashforge_print_options(config) == PrintOptions{});
}

TEST_CASE("The Flashforge send dialog remembers the options as last left", "[FlashforgeSendOptions]")
{
    AppConfig    config;
    PrintOptions chosen;
    chosen.flow_calibration = true;
    chosen.time_lapse       = true;
    remember_flashforge_print_options(config, chosen);
    CHECK(remembered_flashforge_print_options(config) == chosen);

    PrintOptions leveling_only;
    leveling_only.leveling = true;
    remember_flashforge_print_options(config, leveling_only);
    CHECK(remembered_flashforge_print_options(config) == leveling_only);

    remember_flashforge_print_options(config, PrintOptions{}); // turning them off is remembered too
    CHECK(remembered_flashforge_print_options(config) == PrintOptions{});
}

TEST_CASE("What an older build saved is not taken for a choice", "[FlashforgeSendOptions]")
{
    // Older builds saved each box with app_config->set("recent", key, on ? "1" : "0"), which picks
    // AppConfig's bool overload: a pointer is always true, so every config holds "true" whatever was
    // ticked. It says nothing about the user's choice, so the dialog starts with the boxes off.
    AppConfig config;
    config.set("recent", "flashforge_leveling_before_print", std::string("true"));
    config.set("recent", "flashforge_timelapse_video", std::string("true"));
    CHECK(remembered_flashforge_print_options(config) == PrintOptions{});
}

TEST_CASE("The remembered options are stored as the dialog reads them", "[FlashforgeSendOptions]")
{
    AppConfig    config;
    PrintOptions chosen;
    chosen.leveling = true;
    remember_flashforge_print_options(config, chosen);
    CHECK(config.get("recent", "flashforge_leveling_before_print") == "1");
    CHECK(config.get("recent", "flashforge_flow_calibration") == "0");
    CHECK(config.get("recent", "flashforge_timelapse_video") == "0");
}

TEST_CASE("The dialog sends flow calibration only to a printer whose start screen offers it", "[FlashforgeSendOptions]")
{
    PrintOptions ticked;
    ticked.leveling         = true;
    ticked.flow_calibration = true; // remembered from a Creator 5 Pro, say
    ticked.time_lapse       = true;
    CHECK(sent_flashforge_print_options(ticked, true) == ticked);

    // Its box is not shown there, so the remembered tick must not travel either. Leveling and
    // time-lapse are sent as ticked on every model.
    PrintOptions without_flow = ticked;
    without_flow.flow_calibration = false;
    CHECK(sent_flashforge_print_options(ticked, false) == without_flow);
}

TEST_CASE("The material station starts on where the printer has one", "[FlashforgeSendOptions]")
{
    const AppConfig config;
    CHECK(remembered_flashforge_material_station(config, true));
    CHECK_FALSE(remembered_flashforge_material_station(config, false));
}

TEST_CASE("The material station box is remembered as last left", "[FlashforgeSendOptions]")
{
    AppConfig config;
    remember_flashforge_material_station(config, false);
    CHECK(config.get("recent", "flashforge_use_material_station") == "0");
    CHECK_FALSE(remembered_flashforge_material_station(config, true));

    remember_flashforge_material_station(config, true);
    CHECK(remembered_flashforge_material_station(config, true));
    // A printer without a station feeds from none, whatever was left ticked for another.
    CHECK_FALSE(remembered_flashforge_material_station(config, false));
}

TEST_CASE("What an older build saved for the material station is not taken for a choice", "[FlashforgeSendOptions]")
{
    // Older builds saved "true" whatever was ticked (AppConfig's bool overload), and never read it back.
    AppConfig config;
    config.set("recent", "flashforge_use_material_station", std::string("true"));
    CHECK(remembered_flashforge_material_station(config, true));
}
