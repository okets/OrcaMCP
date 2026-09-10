#include <catch2/catch_test_macros.hpp>

#include <map>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp"

// get_presets' filters: the pure half. The full preset list is ~1.9 MB, so these two text filters
// are what stands between an agent and a response no MCP client can take.

using Slic3r::GUI::PresetListCount;
using Slic3r::GUI::PresetQuery;
using Slic3r::GUI::parse_preset_limit_param;
using Slic3r::GUI::preset_list_truncated;
using Slic3r::GUI::preset_query_effective_limit;
using Slic3r::GUI::preset_query_matches;
using Slic3r::GUI::preset_truncation_hint;

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

TEST_CASE("a stray negative degrades to the default, not to no cap", "[orcamcp][presets]")
{
    // Only a caller-supplied 0 means "no cap" (query.limit uses -1, not a negative number, as
    // its own sentinel). Any other negative that reaches this function is not "no cap" in
    // disguise -- it falls back to the same default -1 would have picked.
    CHECK(preset_query_effective_limit(-5, /*summary=*/true) == 25);
    CHECK(preset_query_effective_limit(-5, /*summary=*/false) == 5);
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
    CHECK(preset_list_truncated(counts));
}

TEST_CASE("nothing truncated means no hint at all", "[orcamcp][presets]")
{
    std::map<std::string, PresetListCount> counts;
    counts["filamentPresets"] = {4, 4};

    CHECK(preset_truncation_hint(counts).empty());
    CHECK_FALSE(preset_list_truncated(counts));
}

// The decision that broke in the field: get_presets called with no `limit` argument at all must
// not fall into the same code path as an explicit, invalid one. These cover the handler's actual
// wiring decision, not just the arithmetic preset_query_effective_limit does with the result.

TEST_CASE("limit absent from the request leaves the -1 sentinel and is not an error", "[orcamcp][presets]")
{
    int limit = 0;
    const std::string error = parse_preset_limit_param(nlohmann::json::object(), limit);

    CHECK(error.empty());
    CHECK(limit == -1);
}

TEST_CASE("limit present and a valid non-negative integer is accepted", "[orcamcp][presets]")
{
    int limit = -1;
    const std::string error = parse_preset_limit_param({{"limit", 10}}, limit);

    CHECK(error.empty());
    CHECK(limit == 10);

    // 0 is the explicit "no cap" escape hatch, not a falsy "no value".
    const std::string zero_error = parse_preset_limit_param({{"limit", 0}}, limit);
    CHECK(zero_error.empty());
    CHECK(limit == 0);
}

TEST_CASE("limit present and negative is a request error", "[orcamcp][presets]")
{
    int limit = -1;
    const std::string error = parse_preset_limit_param({{"limit", -5}}, limit);

    CHECK_FALSE(error.empty());
    CHECK(error.find("0 or more") != std::string::npos);
}

TEST_CASE("limit present and not an integer is a request error", "[orcamcp][presets]")
{
    int limit = -1;
    const std::string error = parse_preset_limit_param({{"limit", "twenty-five"}}, limit);

    CHECK_FALSE(error.empty());
    CHECK(error.find("integer") != std::string::npos);
}
