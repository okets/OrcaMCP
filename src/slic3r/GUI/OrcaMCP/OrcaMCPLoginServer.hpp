// src/slic3r/GUI/OrcaMCP/OrcaMCPLoginServer.hpp
#pragma once
// HttpServer.hpp first: it pulls in boost/asio, which on Windows must see <windows.h> before other
// headers do.
#include "slic3r/GUI/HttpServer.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

class MainThreadGate;

// Where the cloud login's loopback callback is answered, kept from ever disturbing the MCP server.
//
// The login dialog asks for a callback server on a port of its choosing (ZUserLogin::
// ensure_loopback_port). It used to get the MCP server itself: its routes were swapped for
// login-only ones and it was restarted on the login's port, so MCP was gone until the app restarted,
// and the restart joined the server's thread from the main thread (the quit deadlock's wait).
//
// Now the MCP server carries the login's port as a second listener (HttpServer::listen_also). The
// login's callbacks are therefore served on the MCP server's own thread, one request at a time with
// MCP calls, as they were when both shared one port: nothing is joined when the login moves, and the
// login never binds the MCP server's port, even when it asks for it. Unit-tested in
// tests/slic3rutils/test_http_server.cpp.
class LoginCallbackServer
{
public:
    // Answers one login callback for a cloud provider, e.g. HttpServer::auth_handle_request.
    using AuthHandler = std::function<std::shared_ptr<HttpServer::Response>(const std::string& url, const std::string& provider)>;

    // `mcp_server` gets the login's listener, and must be started before listen(). `provider` answers
    // callbacks until a login names its own. Once `quit_gate` is closed, callbacks are refused
    // without reaching `auth`, so a quit never waits on a sign-in's network calls.
    LoginCallbackServer(HttpServer& mcp_server, AuthHandler auth, std::string provider, const MainThreadGate& quit_gate);

    // Answers `provider`'s callbacks on `port`: on the MCP server's own port that server alone, anywhere
    // else also a listener added to it for the login. Returns false, binding nothing, when the MCP
    // server is not started.
    bool listen(int port, const std::string& provider);

    // The login callback at `url`, answered for the provider of the login in progress. Called on the
    // MCP server's thread, by its routes that are not /mcp.
    std::shared_ptr<HttpServer::Response> answer(const std::string& url) const;

    // The provider callbacks are answered for, until a listen() names another.
    void set_provider(const std::string& provider);

private:
    HttpServer&           m_mcp_server;
    AuthHandler           m_auth;
    const MainThreadGate& m_quit_gate;
    mutable std::mutex    m_provider_mutex;
    std::string           m_provider;
};

}}} // namespace Slic3r::GUI::OrcaMCP
