#include <catch2/catch_test_macros.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/PrintHostDialogs.hpp"

#include <string>

// What the Flashforge send dialog starts with: every print option off until a send was confirmed
// with it on, then whatever was confirmed last.

using Slic3r::AppConfig;
using Slic3r::FlashforgeApi::PrintOptions;
using Slic3r::GUI::remember_flashforge_print_options;
using Slic3r::GUI::remembered_flashforge_print_options;

TEST_CASE("The Flashforge send dialog starts with every print option off", "[FlashforgeSendOptions]")
{
    const AppConfig config;
    CHECK(remembered_flashforge_print_options(config) == PrintOptions{});
}

TEST_CASE("The Flashforge send dialog remembers the last confirmed options", "[FlashforgeSendOptions]")
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
