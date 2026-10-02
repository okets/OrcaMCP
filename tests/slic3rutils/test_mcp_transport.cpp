#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPTransport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <optional>
#include <initializer_list>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

// How /mcp speaks MCP's Streamable HTTP transport, which standard MCP clients (the MCP TypeScript SDK,
// Claude Code's `type: "http"`) hold it to: a notification is accepted with 202 and no body, a GET for an
// event stream is refused with 405, and the bridge's plain GET still gets the info document. In v2.5.0.9
// notifications/initialized got 400 "Method not found", and the SDK's connect failed on it.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using json = nlohmann::json;

namespace {

std::string written(const std::shared_ptr<HttpServer::Response>& response)
{
    std::stringstream out;
    response->write_response(out);
    return out.str();
}

// The reply's body, after the blank line that ends its head.
std::string body_of(const std::string& reply)
{
    const auto end = reply.find("\r\n\r\n");
    return end == std::string::npos ? std::string() : reply.substr(end + 4);
}

bool starts_with(const std::string& text, const std::string& prefix) { return text.rfind(prefix, 0) == 0; }
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A request's headers, as the server reads them off the wire: one "Name: value" each.
http_headers headers_of(std::initializer_list<std::string> lines)
{
    http_headers headers;
    for (const std::string& line : lines)
        headers.on_read_header(line);
    return headers;
}

std::string post(const json& message) { return written(OrcaMCPServer::handle_request("POST", "/mcp", message.dump(), headers_of({}))); }

} // namespace

TEST_CASE("a JSON-RPC message with a method and no id is a notification", "[McpTransport][orcamcp]")
{
    CHECK(is_notification({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
    CHECK(is_notification({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 3}}}}));

    CHECK_FALSE(is_notification({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}));
    CHECK_FALSE(is_notification({{"jsonrpc", "2.0"}, {"id", "a"}, {"method", "tools/list"}}));
    // An id of null is a request JSON-RPC discourages, not a notification.
    CHECK_FALSE(is_notification({{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "ping"}}));
    // A response to a request the server never sent.
    CHECK_FALSE(is_notification({{"jsonrpc", "2.0"}, {"result", json::object()}}));
    CHECK_FALSE(is_notification(json::array({{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}})));
    CHECK_FALSE(is_notification("notifications/initialized"));
}

TEST_CASE("an Accept header that lists text/event-stream asks for an event stream", "[McpTransport][orcamcp]")
{
    const std::string accept = GENERATE("text/event-stream", "application/json, text/event-stream", "TEXT/Event-Stream",
                                        "text/event-stream; charset=utf-8", "application/json,text/event-stream;q=0.5",
                                        " text/event-stream ");
    INFO(accept);
    CHECK(asks_for_event_stream(accept));
}

TEST_CASE("no Accept header, another type, or text/event-stream refused with q=0 asks for no stream", "[McpTransport][orcamcp]")
{
    CHECK_FALSE(asks_for_event_stream(std::nullopt));
    const std::string accept = GENERATE("", "*/*", "application/json", "text/*", "text/event-streams", "text/event-stream;q=0",
                                        "text/event-stream; q=0.0", "application/json, text/event-stream;q=0.000");
    INFO(accept);
    CHECK_FALSE(asks_for_event_stream(accept));
}

TEST_CASE("a notification is accepted with 202 and no body", "[McpTransport][orcamcp]")
{
    const json notification = GENERATE(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
                                       json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
                                            {"params", {{"requestId", 3}, {"reason", "timed out"}}}},
                                       json{{"jsonrpc", "2.0"}, {"method", "notifications/roots/list_changed"}});
    INFO(notification.dump());
    const std::string reply = post(notification);
    CHECK(starts_with(reply, "HTTP/1.1 202 Accepted\r\n"));
    CHECK(contains(reply, "\r\nContent-Length: 0\r\n"));
    CHECK(body_of(reply).empty());
}

TEST_CASE("a request whose id is null is still answered", "[McpTransport][orcamcp]")
{
    const std::string reply = post({{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "ping"}});
    CHECK(starts_with(reply, "HTTP/1.1 200 OK\r\n"));
    const json answer = json::parse(body_of(reply));
    CHECK(answer.at("result") == json::object());
}

TEST_CASE("a notification that is not JSON-RPC 2.0 is refused, not accepted", "[McpTransport][orcamcp]")
{
    const std::string reply = post({{"jsonrpc", "1.0"}, {"method", "notifications/initialized"}});
    CHECK(starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"));
    CHECK(json::parse(body_of(reply)).at("error").at("code") == -32600);
}

TEST_CASE("a message whose method is not a string is refused, and the server goes on", "[McpTransport][orcamcp]")
{
    // It was read before the handler's error handling: the exception left the server's thread, and a single
    // POST ended the app (v2.5.0.9).
    const json method = GENERATE(json(5), json(nullptr), json::object(), json::array());
    INFO(method.dump());
    for (const json& message : {json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}}, json{{"jsonrpc", "2.0"}, {"method", method}}}) {
        const std::string reply = post(message);
        CHECK(starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"));
        const json answer = json::parse(body_of(reply));
        CHECK(answer.at("error").at("code") == -32600);
    }
}

TEST_CASE("a GET that asks for an event stream is refused with 405, saying what /mcp takes", "[McpTransport][orcamcp]")
{
    const std::string reply = written(OrcaMCPServer::handle_request("GET", "/mcp", "", headers_of({"Accept: text/event-stream"})));
    CHECK(starts_with(reply, "HTTP/1.1 405 Method Not Allowed\r\n"));
    CHECK(contains(reply, "\r\nAllow: GET, POST\r\n"));
    const json answer = json::parse(body_of(reply));
    CHECK(answer.at("error").at("code") == -32600);
    CHECK(contains(answer.at("error").at("message").get<std::string>(), "event stream"));
}

TEST_CASE("the bridge's GET, which asks for no stream, still gets the server's info", "[McpTransport][orcamcp]")
{
    const std::string accept = GENERATE("", "Accept: */*", "Accept: application/json");
    INFO(accept);
    const std::string reply = written(OrcaMCPServer::handle_request("GET", "/mcp", "", accept.empty() ? headers_of({}) : headers_of({accept})));
    CHECK(starts_with(reply, "HTTP/1.1 200 OK\r\n"));
    const json info = json::parse(body_of(reply));
    CHECK(info.at("name") == "orca-slicer");
    CHECK(info.at("protocol") == "mcp");
}

TEST_CASE("a method /mcp does not take is refused with 405 and the methods it does", "[McpTransport][orcamcp]")
{
    const std::string method = GENERATE("DELETE", "PUT", "PATCH");
    INFO(method);
    const std::string reply = written(OrcaMCPServer::handle_request(method, "/mcp", "", headers_of({})));
    CHECK(starts_with(reply, "HTTP/1.1 405 Method Not Allowed\r\n"));
    CHECK(contains(reply, "\r\nAllow: GET, POST\r\n"));
    CHECK(json::parse(body_of(reply)).at("error").at("code") == -32600);
}
