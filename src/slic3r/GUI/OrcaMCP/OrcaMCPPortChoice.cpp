#include "OrcaMCPPortChoice.hpp"

#include <boost/asio.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

using boost::asio::ip::tcp;

// Whether a connection to `where` is accepted, or still pending after `timeout`.
bool accepts_or_holds(const tcp::endpoint& where, std::chrono::milliseconds timeout)
{
    boost::asio::io_context   io;
    tcp::socket               socket(io);
    std::optional<bool>       connected;
    socket.async_connect(where, [&connected](const boost::system::error_code& ec) { connected = !ec; });
    io.run_for(timeout);
    // Not finished in time: something holds the port without answering. The socket closes on return,
    // and its handler, never run, goes with the io_context.
    return connected.value_or(true);
}

} // namespace

std::vector<Port> mcp_ports()
{
    std::vector<Port> ports;
    for (size_t i = 0; i < mcp_port_count; ++i)
        ports.push_back(static_cast<Port>(first_mcp_port + i));
    return ports;
}

bool port_has_listener(Port port, std::chrono::milliseconds timeout)
{
    return accepts_or_holds({boost::asio::ip::address_v4::loopback(), port}, timeout) ||
           accepts_or_holds({boost::asio::ip::address_v6::loopback(), port}, timeout);
}

PortChoice choose_port(const std::vector<Port>& ports, const std::function<bool(Port)>& has_listener,
                       const std::function<std::string(Port)>& listen)
{
    PortChoice choice;
    for (const Port port : ports) {
        if (has_listener(port)) {
            choice.skipped.push_back(std::to_string(port) + ": in use");
            continue;
        }
        const std::string failure = listen(port);
        if (failure.empty()) {
            choice.port = port;
            return choice;
        }
        choice.skipped.push_back(std::to_string(port) + ": " + failure);
    }
    return choice;
}

std::string describe_port_choice(const PortChoice& choice)
{
    std::string skipped;
    for (const std::string& reason : choice.skipped)
        skipped += (skipped.empty() ? "" : "; ") + reason;
    if (choice.port)
        return "MCP server on port " + std::to_string(*choice.port) + (skipped.empty() ? "" : " (passed over " + skipped + ")");
    return "no MCP server: every port from " + std::to_string(first_mcp_port) + " to " +
           std::to_string(first_mcp_port + mcp_port_count - 1) + " was taken (" + skipped + ")";
}

}}} // namespace Slic3r::GUI::OrcaMCP
