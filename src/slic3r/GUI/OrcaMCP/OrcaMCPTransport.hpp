// src/slic3r/GUI/OrcaMCP/OrcaMCPTransport.hpp
#pragma once
#include "slic3r/GUI/HttpServer.hpp"
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

// How /mcp speaks MCP's Streamable HTTP transport, so a standard MCP client -- the MCP TypeScript SDK,
// Claude Code's `type: "http"` -- works with it directly, not only the bridge:
//
//  - A POSTed JSON-RPC notification (a method and no id) is accepted with 202 and no body. JSON-RPC never
//    answers one, and the 400 "Method not found" notifications/initialized got failed the SDK's connect.
//    Every notification is accepted: none asks the server for anything it must do. A client's
//    notifications/cancelled cannot reach the call it names anyway: the server serves one request at a
//    time, so it is read only once that call has been answered.
//  - A GET that asks for an event stream is refused with 405: the server sends nothing of its own. A GET
//    that does not (the bridge's, curl's) still gets the info document that names the instance.
//
// The server answers every reply with Connection: close and CRLF line ends (HttpServer::Response).
//
// Unit-tested in tests/slic3rutils/test_mcp_transport.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// True for a JSON-RPC notification: an object with a method and no id. One whose id is null is a request.
bool is_notification(const nlohmann::json& message);

// True when an Accept header lists text/event-stream, in any case and with any parameters, and does not
// refuse it with q=0. No Accept header asks for no stream.
bool asks_for_event_stream(const std::optional<std::string>& accept);

// HTTP 405 for a request /mcp does not take, with the methods it does (Allow) and JSON-RPC error -32600
// saying `reason`.
std::shared_ptr<HttpServer::Response> method_not_allowed_response(const std::string& reason);

}}} // namespace Slic3r::GUI::OrcaMCP
