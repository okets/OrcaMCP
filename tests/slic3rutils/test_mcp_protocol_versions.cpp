#include "slic3r/GUI/OrcaMCP/OrcaMCPProtocolVersions.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

// Which MCP protocol versions OrcaMCP speaks and agrees on with a client. MCP's rule: a server that speaks
// the version a client asks for answers with it, otherwise with the newest it speaks; over HTTP it answers a
// later request naming another version (MCP-Protocol-Version) with 400. Through v2.5.0.9 the app answered
// 2024-11-05 whatever was asked.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using json = nlohmann::json;

namespace {

bool supports(const std::string& version)
{
    const auto& versions = supported_protocol_versions();
    return std::find(versions.begin(), versions.end(), version) != versions.end();
}

std::string written(const std::shared_ptr<HttpServer::Response>& response)
{
    std::stringstream out;
    response->write_response(out);
    return out.str();
}

std::string body_of(const std::string& reply) { return reply.substr(reply.find("\r\n\r\n") + 4); }

bool starts_with(const std::string& text, const std::string& prefix) { return text.rfind(prefix, 0) == 0; }

// One POST to /mcp, with an MCP-Protocol-Version header when `version` is not empty.
std::string post(const json& message, const std::string& version)
{
    http_headers headers;
    if (!version.empty())
        headers.on_read_header("MCP-Protocol-Version: " + version);
    return written(OrcaMCPServer::handle_request("POST", "/mcp", message.dump(), headers));
}

json initialize(const json& protocol_version)
{
    json params = {{"capabilities", json::object()}, {"clientInfo", {{"name", "test"}, {"version", "1"}}}};
    if (!protocol_version.is_null())
        params["protocolVersion"] = protocol_version;
    return json::parse(body_of(post({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", params}}, "")));
}

} // namespace

TEST_CASE("the supported versions are the ones OrcaMCP meets, newest first", "[McpProtocolVersions][orcamcp]")
{
    const auto& versions = supported_protocol_versions();
    REQUIRE_FALSE(versions.empty());
    CHECK(std::is_sorted(versions.rbegin(), versions.rend()));
    CHECK(versions.front() == "2025-06-18");
    CHECK(supports("2024-11-05"));
    // A 2025-03-26 server must accept JSON-RPC batches, and OrcaMCP accepts none.
    CHECK_FALSE(supports("2025-03-26"));
    // 2025-11-25 returns argument refusals as tool errors; OrcaMCP's are JSON-RPC -32602.
    CHECK_FALSE(supports("2025-11-25"));
}

TEST_CASE("initialize answers the version a client asks for when OrcaMCP speaks it", "[McpProtocolVersions][orcamcp]")
{
    const std::string version = GENERATE(from_range(supported_protocol_versions()));
    INFO(version);
    CHECK(negotiated_protocol_version({{"protocolVersion", version}}) == version);
    CHECK(initialize(version).at("result").at("protocolVersion") == version);
}

TEST_CASE("initialize answers the newest version OrcaMCP speaks for any other", "[McpProtocolVersions][orcamcp]")
{
    const json asked = GENERATE(json("2025-11-25"), json("2025-03-26"), json("2099-01-01"), json("1.0"), json(20250618), json(nullptr));
    INFO(asked.dump());
    CHECK(initialize(asked).at("result").at("protocolVersion") == supported_protocol_versions().front());
    CHECK(negotiated_protocol_version(json::array()) == supported_protocol_versions().front());
}

TEST_CASE("a request naming a version OrcaMCP speaks, or none, is served", "[McpProtocolVersions][orcamcp]")
{
    const std::string version = GENERATE(as<std::string>(), "", "2025-06-18", "2024-11-05");
    INFO(version);
    CHECK_FALSE(protocol_version_header_refusal(version.empty() ? std::nullopt : std::optional<std::string>(version)));
    const std::string reply = post({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}}, version);
    CHECK(starts_with(reply, "HTTP/1.1 200 OK\r\n"));
    CHECK(json::parse(body_of(reply)).at("result") == json::object());
}

TEST_CASE("a request naming a version OrcaMCP does not speak is refused with 400, saying which it does", "[McpProtocolVersions][orcamcp]")
{
    const std::string version = GENERATE("2025-11-25", "2025-03-26", "garbage");
    INFO(version);
    for (const json& message : {json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}},
                                json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}}) {
        const std::string reply = post(message, version);
        CHECK(starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"));
        const std::string message_text = json::parse(body_of(reply)).at("error").at("message");
        CHECK(message_text.find(version) != std::string::npos);
        CHECK(message_text.find("2025-06-18") != std::string::npos);
    }
}

TEST_CASE("initialize is never refused for its header: it is where the version is agreed", "[McpProtocolVersions][orcamcp]")
{
    const std::string reply = post({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "initialize"}, {"params", {{"protocolVersion", "2025-11-25"}}}},
                                   "2025-11-25");
    CHECK(starts_with(reply, "HTTP/1.1 200 OK\r\n"));
    CHECK(json::parse(body_of(reply)).at("result").at("protocolVersion") == supported_protocol_versions().front());
}

TEST_CASE("the golden tool list carries the supported versions, for the bridge's initialize", "[McpProtocolVersions][orcamcp]")
{
    CHECK(OrcaMCPServer::tools_manifest().at("protocol_versions") == json(supported_protocol_versions()));
}
