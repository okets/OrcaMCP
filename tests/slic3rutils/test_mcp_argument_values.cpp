#include <catch2/catch_test_macros.hpp>

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
