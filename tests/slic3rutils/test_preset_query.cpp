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

#include <map>

using Slic3r::GUI::PresetListCount;
using Slic3r::GUI::preset_query_effective_limit;
using Slic3r::GUI::preset_truncation_hint;

TEST_CASE("the default cap depends on how big a row is", "[orcamcp][presets]")
{
    // A summary row is ~164 characters; the unfiltered summary response was ~54,600 and no MCP
    // client would take it. A full-config row is ~5.7 KB, so the same count is 100x worse.
    CHECK(preset_query_effective_limit(-1, /*summary=*/true) == 25);
    CHECK(preset_query_effective_limit(-1, /*summary=*/false) == 5);
}

TEST_CASE("an explicit limit wins, and 0 means no cap", "[orcamcp][presets]")
{
    CHECK(preset_query_effective_limit(100, /*summary=*/true) == 100);
    CHECK(preset_query_effective_limit(1, /*summary=*/false) == 1);
    // The escape hatch for a caller that knows what it is asking for.
    CHECK(preset_query_effective_limit(0, /*summary=*/true) == 0);
}

TEST_CASE("a truncated response says what it dropped and how to narrow it", "[orcamcp][presets]")
{
    std::map<std::string, PresetListCount> counts;
    counts["filamentPresets"] = {318, 25};
    counts["printerPresets"]  = {9, 9};

    const std::string hint = preset_truncation_hint(counts);
    CHECK(hint.find("25 of 318 filamentPresets") != std::string::npos);
    // A type that was not truncated must not be mentioned -- the hint is about what is missing.
    CHECK(hint.find("printerPresets") == std::string::npos);
    CHECK(hint.find("name_contains") != std::string::npos);
    CHECK(hint.find("limit") != std::string::npos);
}

TEST_CASE("nothing truncated means no hint at all", "[orcamcp][presets]")
{
    std::map<std::string, PresetListCount> counts;
    counts["filamentPresets"] = {4, 4};

    CHECK(preset_truncation_hint(counts).empty());
}
