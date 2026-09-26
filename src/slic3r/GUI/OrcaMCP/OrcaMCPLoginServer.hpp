// src/slic3r/GUI/OrcaMCP/OrcaMCPLoginServer.hpp
#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "slic3r/GUI/HttpServer.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// The cloud login's loopback callback server, kept apart from the MCP server.
//
// The login dialog asks for a callback server on a port of its choosing (ZUserLogin::
// ensure_loopback_port). There used to be one HttpServer for both, so opening the login replaced the
// MCP server's routes with login-only ones and restarted it on the login's port: MCP was gone until
// the app was restarted. The restart also joined the server's thread from the main thread, the wait
// that deadlocked quit_app. Now the login listens on a server of its own, and never stops, moves or
// re-routes the MCP server. Unit-tested in tests/slic3rutils/test_http_server.cpp.
class LoginCallbackServer
{
public:
    // Answers one login callback for a cloud provider, e.g. HttpServer::auth_handle_request.
    using AuthHandler = std::function<std::shared_ptr<HttpServer::Response>(const std::string& url, const std::string& provider)>;

    // `mcp_server` is only looked at, never started or stopped. `provider` answers callbacks until
    // a login names its own.
    LoginCallbackServer(HttpServer& mcp_server, AuthHandler auth, std::string provider);

    // Answers `provider`'s callbacks on `port`. Only one server can hold a port, so on the MCP
    // server's own port that server answers them: its routes that are not /mcp call answer().
    void listen(int port, const std::string& provider);

    // The login callback at `url`, answered for the provider of the login in progress. Called on a
    // server's thread.
    std::shared_ptr<HttpServer::Response> answer(const std::string& url) const;

    // The provider callbacks are answered for, until a listen() names another.
    void set_provider(const std::string& provider);

    void stop();

private:
    HttpServer&        m_mcp_server;
    AuthHandler        m_auth;
    mutable std::mutex m_provider_mutex;
    std::string        m_provider;
    HttpServer         m_server;
};

}}} // namespace Slic3r::GUI::OrcaMCP
