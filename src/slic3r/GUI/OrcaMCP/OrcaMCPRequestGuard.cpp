#include "OrcaMCPRequestGuard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

constexpr int k_forbidden_error = -32003;

std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// Host names are case-insensitive; the port must be the MCP server's own.
bool names_this_machine(const std::string& host, unsigned port)
{
    const std::string                  wanted_port = ":" + std::to_string(port);
    const std::array<std::string, 3>   names{"127.0.0.1", "localhost", "[::1]"};
    const std::string                  given = lowercase(host);
    return std::any_of(names.begin(), names.end(), [&](const std::string& name) { return given == name + wanted_port; });
}

} // namespace

bool is_mcp_url(const std::string& url) { return url.find("/mcp") != std::string::npos; }

std::optional<std::string> mcp_request_refusal(const std::optional<std::string>& origin, const std::optional<std::string>& host,
                                               unsigned port)
{
    if (origin)
        return "OrcaMCP does not answer web pages (the request came from " + *origin +
               "). Use an MCP client such as the OrcaMCP bridge.";
    if (!host || !names_this_machine(*host, port))
        return "OrcaMCP answers only requests addressed to 127.0.0.1:" + std::to_string(port) + ", localhost:" +
               std::to_string(port) + " or [::1]:" + std::to_string(port) + " (this one named " +
               (host ? *host : std::string("no host")) + ").";
    return std::nullopt;
}

std::shared_ptr<HttpServer::Response> mcp_refusal_response(const std::string& reason)
{
    const nlohmann::json error = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", k_forbidden_error}, {"message", reason}}}};
    return std::make_shared<HttpServer::ResponseJson>(error.dump(), 403);
}

HttpServer::RequestGuardFn app_request_guard(boost::asio::ip::port_type mcp_port,
                                             std::function<bool(boost::asio::ip::port_type)> login_listens_on)
{
    return [mcp_port, login_listens_on = std::move(login_listens_on)](const HttpServer::RequestInfo& request)
               -> std::shared_ptr<HttpServer::Response> {
        if (is_mcp_url(request.url)) {
            if (const auto refusal = mcp_request_refusal(request.origin, request.host, mcp_port))
                return mcp_refusal_response(*refusal);
            return nullptr;
        }
        if (!login_listens_on(request.local_port))
            return std::make_shared<HttpServer::ResponseNotFound>();
        return nullptr;
    };
}

}}} // namespace Slic3r::GUI::OrcaMCP
