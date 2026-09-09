#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp"

// get_presets' filters: the pure half. The full preset list is ~1.9 MB, so these two text filters
// are what stands between an agent and a response no MCP client can take.

using Slic3r::GUI::PresetQuery;
using Slic3r::GUI::preset_query_matches;

TEST_CASE("preset_query_matches without filters accepts everything", "[orcamcp][presets]")
{
    PresetQuery query;

    CHECK(preset_query_matches("Flashforge PETG Pro @FF C5P", "Flashforge", query));
    CHECK(preset_query_matches("My own profile", "", query));
}

TEST_CASE("preset_query_matches filters by vendor, case-insensitively", "[orcamcp][presets]")
{
    PresetQuery query;
    query.vendor = "flashforge";

    CHECK(preset_query_matches("Flashforge PETG Pro @FF C5P", "Flashforge", query));
    CHECK_FALSE(preset_query_matches("Bambu PETG Basic @System", "Bambu Lab", query));
    // A preset with no vendor at all is not a match for a vendor search.
    CHECK_FALSE(preset_query_matches("My own profile", "", query));
}

TEST_CASE("preset_query_matches filters by name substring", "[orcamcp][presets]")
{
    PresetQuery query;
    query.name_contains = "petg";

    CHECK(preset_query_matches("Flashforge PETG Pro @FF C5P", "Flashforge", query));
    CHECK(preset_query_matches("Generic PETG @System", "Generic", query));
    CHECK_FALSE(preset_query_matches("Flashforge PLA Basic @FF C5P", "Flashforge", query));
}

TEST_CASE("preset_query_matches combines the filters", "[orcamcp][presets]")
{
    PresetQuery query;
    query.vendor = "Flashforge";
    query.name_contains = "PETG";

    CHECK(preset_query_matches("Flashforge PETG Pro @FF C5P", "Flashforge", query));
    CHECK_FALSE(preset_query_matches("Generic PETG @System", "Generic", query));
    CHECK_FALSE(preset_query_matches("Flashforge PLA Basic @FF C5P", "Flashforge", query));
}
