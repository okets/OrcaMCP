#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Preset.hpp"

using namespace Slic3r;

// Why these lists exist: on 2026-09-15 opening a 3MF saved by an older build blanked the Obico link
// on the user's printer preset, because the project's (absent) values were applied over it. The
// same list keeps connection settings and credentials out of every project the slicer writes.
TEST_CASE("print-host connection keys cover every host's connection setting", "[preset][obico]")
{
    for (const char* key : {"print_host", "print_host_webui", "printhost_apikey", "printhost_cafile", "printhost_user",
                            "printhost_password", "printhost_port", "flashforge_serial_number", "flashforge_obico_url",
                            "flashforge_obico_token"})
        CHECK(Preset::is_print_host_connection_key(key));
    for (const char* key : {"printer_model", "nozzle_diameter", "inherits", "printer_settings_id", ""})
        CHECK_FALSE(Preset::is_print_host_connection_key(key));
}

TEST_CASE("print-host secret keys are the credentials only", "[preset][obico]")
{
    for (const char* key : {"printhost_apikey", "printhost_password", "flashforge_obico_token"}) {
        CHECK(Preset::is_print_host_secret_key(key));
        CHECK(Preset::is_print_host_connection_key(key)); // every secret is also a connection setting
    }
    for (const char* key : {"print_host", "flashforge_serial_number", "flashforge_obico_url", "printhost_user"})
        CHECK_FALSE(Preset::is_print_host_secret_key(key));
}
