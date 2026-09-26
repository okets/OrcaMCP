// src/slic3r/GUI/OrcaMCP/OrcaMCPRequestGuard.hpp
#pragma once
// HttpServer.hpp first: it pulls in boost/asio, which on Windows must see <windows.h> before other
// headers do.
#include "slic3r/GUI/HttpServer.hpp"
#include <functional>
#include <optional>
#include <string>

// Which requests the MCP server answers at all.
//
// It listens on 127.0.0.1 only, but a web page in the user's browser runs on this machine too.
// Without these rules any site could POST a tool call to http://localhost:13618/mcp -- as a
// plain-text "simple" request, which needs no CORS preflight -- and start a print; the server even
// told browsers they could read the reply (Access-Control-Allow-Origin: *, dropped). So:
//
//  - An MCP request must carry no Origin header. Browsers send one on every cross-origin fetch and
//    form post; the bridge, curl and other local MCP clients send none. No page in the app calls
//    /mcp (checked 2026-09-26: the Device tab, the printer and Obico pages, Home and every script
//    under resources/web), so no origin is allowed.
//  - Its Host header must name this machine and the MCP port: 127.0.0.1:<port>, localhost:<port> or
//    [::1]:<port>. DNS rebinding points an attacker's own name at 127.0.0.1, so its page's requests
//    reach the server as same-origin, with no Origin header, but with that name in Host.
//  - Anything else on the server is a cloud-login callback, answered only on a port a login is
//    listening on (LoginCallbackServer). Those are browser navigations from the cloud's page, so
//    they carry no Origin and name localhost or 127.0.0.1 themselves; the rules above do not apply,
//    but on the MCP port, with no login there, a page could otherwise make the browser deliver a
//    forged callback that signs the user in to someone else's account.
//
// Unit-tested in tests/slic3rutils/test_mcp_request_guard.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// True for the URLs the MCP server answers; every other URL is a cloud-login callback.
bool is_mcp_url(const std::string& url);

// Why an MCP request must be refused, or nothing when it may be served.
std::optional<std::string> mcp_request_refusal(const std::optional<std::string>& origin, const std::optional<std::string>& host,
                                               unsigned port);

// The response a refused MCP request gets: HTTP 403 with JSON-RPC error -32003 and the reason.
std::shared_ptr<HttpServer::Response> mcp_refusal_response(const std::string& reason);

// The guard for an MCP server on `mcp_port` that also carries the cloud login's callback port. An
// MCP request is refused by the rules above; a login callback is refused (404) unless
// `login_listens_on` says a login is listening on the port it came in on.
HttpServer::RequestGuardFn app_request_guard(boost::asio::ip::port_type mcp_port,
                                             std::function<bool(boost::asio::ip::port_type)> login_listens_on);

}}} // namespace Slic3r::GUI::OrcaMCP
