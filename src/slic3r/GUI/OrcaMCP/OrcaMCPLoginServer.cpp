#include "OrcaMCPLoginServer.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

LoginCallbackServer::LoginCallbackServer(HttpServer& mcp_server, AuthHandler auth, std::string provider)
    : m_mcp_server(mcp_server), m_auth(std::move(auth)), m_provider(std::move(provider))
{
    // Set once, here: replacing a running server's handler races the thread that calls it.
    m_server.set_request_handler([this](const std::string& url) { return answer(url); });
}

void LoginCallbackServer::listen(int port, const std::string& provider)
{
    set_provider(provider);
    if (m_mcp_server.is_started() && port == m_mcp_server.get_port()) {
        m_server.stop();
        return;
    }
    if (m_server.is_started() && port == m_server.get_port())
        return;
    m_server.stop();
    m_server.set_port(static_cast<boost::asio::ip::port_type>(port));
    m_server.start();
}

std::shared_ptr<HttpServer::Response> LoginCallbackServer::answer(const std::string& url) const
{
    std::string provider;
    {
        std::lock_guard<std::mutex> lock(m_provider_mutex);
        provider = m_provider;
    }
    return m_auth(url, provider);
}

void LoginCallbackServer::set_provider(const std::string& provider)
{
    std::lock_guard<std::mutex> lock(m_provider_mutex);
    m_provider = provider;
}

void LoginCallbackServer::stop() { m_server.stop(); }

}}} // namespace Slic3r::GUI::OrcaMCP
