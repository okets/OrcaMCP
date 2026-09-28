#include "OrcaMCPPortChoice.hpp"

#include <boost/asio.hpp>

#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

using boost::asio::ip::tcp;

// Whether a connection to any of `where` is accepted within `timeout`. They are tried together, and the
// probe returns as soon as one is accepted or all have failed. A connection still pending at the timeout
// is no sign of a listener: Windows refuses one to a closed loopback port only after about a second of
// retries, and a firewall that drops loopback IPv6 never answers, while a listener accepts at once. The
// sockets close on return, and a handler never run goes with the io_context.
bool accepted_within(const std::vector<tcp::endpoint>& where, std::chrono::milliseconds timeout)
{
    boost::asio::io_context  io;
    std::vector<tcp::socket> sockets;
    sockets.reserve(where.size());
    bool   accepted = false;
    size_t finished = 0;
    for (const tcp::endpoint& endpoint : where) {
        sockets.emplace_back(io);
        sockets.back().async_connect(endpoint, [&](const boost::system::error_code& ec) {
            accepted = accepted || !ec;
            if (accepted || ++finished == where.size())
                io.stop();
        });
    }
    io.run_for(timeout);
    return accepted;
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
    return accepted_within({{boost::asio::ip::address_v4::loopback(), port}, {boost::asio::ip::address_v6::loopback(), port}},
                           timeout);
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
