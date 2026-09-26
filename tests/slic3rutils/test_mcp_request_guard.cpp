// OrcaMCPRequestGuard.hpp first: it pulls in HttpServer.hpp -> boost/asio, which on Windows must see
// <windows.h> before the libslic3r headers do.
#include "slic3r/GUI/OrcaMCP/OrcaMCPRequestGuard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <optional>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

// Which requests the MCP server answers at all: never a web page's, never one addressed to another
// name (DNS rebinding), and a cloud-login callback only where a login listens. The server listens on
// 127.0.0.1 only, but the user's browser runs here too, and MCP can start prints.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

constexpr unsigned short k_mcp_port   = 13618;
constexpr unsigned short k_login_port = 41172;

std::string written(const std::shared_ptr<HttpServer::Response>& response)
{
    std::stringstream out;
    response->write_response(out);
    return out.str();
}

HttpServer::RequestInfo request(const std::string& url, std::optional<std::string> origin, std::optional<std::string> host,
                                unsigned short local_port = k_mcp_port)
{
    return {"POST", url, std::move(origin), std::move(host), local_port};
}

} // namespace

TEST_CASE("an MCP request from a local client, addressed to this machine, is served", "[McpRequestGuard][orcamcp]")
{
    const std::string host = GENERATE("127.0.0.1:13618", "localhost:13618", "[::1]:13618", "LocalHost:13618");
    INFO(host);
    CHECK_FALSE(mcp_request_refusal(std::nullopt, host, k_mcp_port).has_value());
}

TEST_CASE("an MCP request that carries an Origin header is refused, whatever the origin", "[McpRequestGuard][orcamcp]")
{
    // Browsers send Origin on cross-origin fetches and form posts; no page in the app calls MCP.
    const std::string origin = GENERATE("http://evil.example", "null", "http://localhost:13618", "file://");
    INFO(origin);
    const auto refusal = mcp_request_refusal(origin, std::string("localhost:13618"), k_mcp_port);
    REQUIRE(refusal.has_value());
    CHECK(refusal->find("does not answer web pages") != std::string::npos);
}

TEST_CASE("an MCP request addressed to any other name or port is refused", "[McpRequestGuard][orcamcp]")
{
    // DNS rebinding: an attacker's name that resolves to 127.0.0.1 reaches the server same-origin,
    // with no Origin header, but with its own name in Host.
    const std::string host = GENERATE("evil.example:13618", "localhost.evil.example:13618", "127.0.0.1", "localhost",
                                      "127.0.0.1:41172", "localhost:13618.evil.example", "10.0.0.5:13618", "");
    INFO("Host: " << host);
    const auto refusal = mcp_request_refusal(std::nullopt, host, k_mcp_port);
    REQUIRE(refusal.has_value());
    CHECK(refusal->find("127.0.0.1:13618") != std::string::npos);
}

TEST_CASE("an MCP request without a Host header is refused", "[McpRequestGuard][orcamcp]")
{
    const auto refusal = mcp_request_refusal(std::nullopt, std::nullopt, k_mcp_port);
    REQUIRE(refusal.has_value());
    CHECK(refusal->find("no host") != std::string::npos);
}

TEST_CASE("a refused MCP request gets HTTP 403 with a JSON-RPC error that says why", "[McpRequestGuard][orcamcp]")
{
    const std::string reply = written(mcp_refusal_response("the reason"));
    CHECK(reply.rfind("HTTP/1.1 403 Forbidden", 0) == 0);
    CHECK(reply.find("Access-Control-Allow-Origin") == std::string::npos);
    const nlohmann::json body = nlohmann::json::parse(reply.substr(reply.find('{')));
    CHECK(body.at("error").at("code") == -32003);
    CHECK(body.at("error").at("message") == "the reason");
}

TEST_CASE("no MCP reply tells a browser it may read it", "[McpRequestGuard][orcamcp]")
{
    const std::string reply = written(std::make_shared<HttpServer::ResponseJson>(R"({"result":{}})"));
    CHECK(reply.find("Access-Control-Allow") == std::string::npos);
}

TEST_CASE("the app's guard refuses web pages on MCP, and login callbacks where no login listens", "[McpRequestGuard][orcamcp]")
{
    unsigned short login_port = 0; // no login yet
    const auto guard = app_request_guard(k_mcp_port, [&](unsigned short port) { return port != 0 && port == login_port; });

    // MCP: the bridge's request passes, a web page's and a rebound one's get 403.
    CHECK(guard(request("/mcp", std::nullopt, std::string("localhost:13618"))) == nullptr);
    CHECK(written(guard(request("/mcp", std::string("http://evil.example"), std::string("localhost:13618")))).rfind("HTTP/1.1 403", 0) == 0);
    CHECK(written(guard(request("/mcp", std::nullopt, std::string("evil.example:13618")))).rfind("HTTP/1.1 403", 0) == 0);

    // A login callback on the MCP port, with no login listening there: a page could have forged it.
    CHECK(written(guard(request("/callback?access_token=x", std::nullopt, std::string("localhost:13618")))).rfind("HTTP/1.1 404", 0) == 0);

    // Once a login listens on its port, its callbacks pass there -- browser navigations, no Host rule.
    login_port = k_login_port;
    CHECK(guard(request("/callback?code=1", std::nullopt, std::string("127.0.0.1:41172"), k_login_port)) == nullptr);
    CHECK(written(guard(request("/callback?code=1", std::nullopt, std::string("localhost:13618")))).rfind("HTTP/1.1 404", 0) == 0);

    // A login told the MCP port is answered there.
    login_port = k_mcp_port;
    CHECK(guard(request("/callback?code=2", std::nullopt, std::string("localhost:13618"))) == nullptr);
}
