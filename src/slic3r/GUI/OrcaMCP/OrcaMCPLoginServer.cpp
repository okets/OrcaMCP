#include "OrcaMCPLoginServer.hpp"
#include "OrcaMCPMainThreadGate.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

LoginCallbackServer::LoginCallbackServer(HttpServer& mcp_server, AuthHandler auth, std::string provider, const MainThreadGate& quit_gate)
    : m_mcp_server(mcp_server), m_auth(std::move(auth)), m_quit_gate(quit_gate), m_provider(std::move(provider))
{}

bool LoginCallbackServer::listen(int port, const std::string& provider)
{
    set_provider(provider);
    if (!m_mcp_server.is_started())
        return false;
    const bool own_port = port == m_mcp_server.get_port();
    return m_mcp_server.listen_also(own_port ? 0 : static_cast<boost::asio::ip::port_type>(port));
}

std::shared_ptr<HttpServer::Response> LoginCallbackServer::answer(const std::string& url) const
{
    if (m_quit_gate.is_closed())
        return std::make_shared<HttpServer::ResponseHtml>(
            "<html><body><p>OrcaSlicer is quitting. Sign in again once it has restarted.</p></body></html>");
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

}}} // namespace Slic3r::GUI::OrcaMCP
