#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"

#include <string>

// Argument values a tool used to act on as something else -- a no-op reported as success, or a change
// wider than asked -- and now refuses with its own error, {"status": "error", "message"}, saying what
// to send instead. Each refusal comes before the tool touches the scene (or the printer), so none of
// this needs the app: a call that got past its refusal would need the GUI this test does not have.

using Slic3r::GUI::OrcaMCPServer;
using json = nlohmann::json;

namespace {

// The tool's own answer to a tools/call, decoded.
json call_tool(const std::string& tool, const json& arguments)
{
    const json result = OrcaMCPServer::handle_tools_call({{"name", tool}, {"arguments", arguments}});
    return json::parse(result.at("content").at(0).at("text").get<std::string>());
}

// The message of the tool's error, or a note that it did not refuse.
std::string refusal(const std::string& tool, const json& arguments)
{
    const json answer = call_tool(tool, arguments);
    if (answer.value("status", "") != "error")
        return "not refused: " + answer.dump();
    return answer.value("message", "");
}

} // namespace

TEST_CASE("rotate_object refuses relative false, which it would have applied as relative", "[McpArgumentValues][orcamcp]")
{
    CHECK(refusal("rotate_object", {{"object_id", 0}, {"z", 90}, {"relative", false}}) ==
          "absolute rotation is not supported: pass the change in degrees, relative to rotation_degrees from "
          "get_object_info");
}

TEST_CASE("delete_object_layer_range refuses one bound without the other, which deleted every range",
          "[McpArgumentValues][orcamcp]")
{
    CHECK(refusal("delete_object_layer_range", {{"object_id", 0}, {"z_min", 1.0}}) ==
          "z_max is missing: pass both z_min and z_max to delete that range, or neither to delete every range of the object");
    CHECK(refusal("delete_object_layer_range", {{"object_id", 0}, {"z_max", 2.0}}) ==
          "z_min is missing: pass both z_min and z_max to delete that range, or neither to delete every range of the object");
}

TEST_CASE("reset_object_config refuses an empty or malformed keys, which reset every override", "[McpArgumentValues][orcamcp]")
{
    CHECK(refusal("reset_object_config", {{"object_id", 0}, {"keys", json::array()}}) ==
          "keys is empty: omit keys to reset every override but the filament");
    CHECK(refusal("reset_object_config", {{"object_id", 0}, {"keys", "wall_loops"}}) ==
          "keys must be an array of setting names; omit it to reset every override but the filament");
    CHECK(refusal("reset_object_config", {{"object_id", 0}, {"keys", {"wall_loops", 3}}}) ==
          "keys must be an array of setting names; got 3");
}

TEST_CASE("clone_object refuses a count below 1, which made no copy but still rearranged the plate",
          "[McpArgumentValues][orcamcp]")
{
    const int count = GENERATE(0, -2);
    CHECK(refusal("clone_object", {{"object_id", 0}, {"count", count}}) == "count must be 1 or more: the number of copies to make");
}

TEST_CASE("cut_object refuses a keep it does not know, which it cut as below", "[McpArgumentValues][orcamcp]")
{
    CHECK(refusal("cut_object", {{"object_id", 0}, {"z_height", 5.0}, {"keep", "top"}}) ==
          "keep must be one of below, above, both; got \"top\"");
}

TEST_CASE("get_valid_config_keys refuses a category it does not know, which listed no keys", "[McpArgumentValues][orcamcp]")
{
    CHECK(refusal("get_valid_config_keys", {{"category", "speed"}}) ==
          "category must be one of per_object, print, filament, printer, toolchanger, project, all; got \"speed\"");
}

TEST_CASE("scale_object refuses uniform without x, whose y or z it ignored", "[McpArgumentValues][orcamcp]")
{
    CHECK(refusal("scale_object", {{"object_id", 0}, {"uniform", true}, {"y", 2.0}}) == "uniform scales every axis by x: give x");
    CHECK(refusal("scale_object", {{"object_id", 0}, {"uniform", true}, {"z", 0.5}}) == "uniform scales every axis by x: give x");
}
