#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPortChoice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// Which port an OrcaMCP instance's MCP server takes, so several can run at once (OrcaMCPPortChoice.hpp):
// the first of 13618-13627 nothing listens on, at 127.0.0.1 or [::1], including an older OrcaMCP that
// listens on every interface, which macOS lets a 127.0.0.1 listener bind beside.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using boost::asio::ip::tcp;

namespace {

// A port nothing listens on right now, on the loopback address.
Port free_loopback_port()
{
    boost::asio::io_context io;
    tcp::acceptor           probe(io, {boost::asio::ip::address_v4::loopback(), 0});
    return probe.local_endpoint().port();
}

// Listens with asio's defaults, SO_REUSEADDR included, as OrcaMCP 2.5.0.5 and upstream's login server do.
struct Listener
{
    boost::asio::io_context io;
    tcp::acceptor           acceptor;
    explicit Listener(const tcp::endpoint& where) : acceptor(io, where) {}
    Port port() const { return acceptor.local_endpoint().port(); }
};

std::function<bool(Port)> listeners_on(std::vector<Port> taken)
{
    return [taken](Port port) { return std::find(taken.begin(), taken.end(), port) != taken.end(); };
}

std::function<std::string(Port)> listens_except(std::vector<Port> failing, std::vector<Port>* tried = nullptr)
{
    return [failing, tried](Port port) -> std::string {
        if (tried)
            tried->push_back(port);
        return std::find(failing.begin(), failing.end(), port) != failing.end() ? "bind: Address already in use" : "";
    };
}

} // namespace

TEST_CASE("the MCP ports are 13618 to 13627, tried in order", "[McpPortChoice]")
{
    const std::vector<Port> ports = mcp_ports();
    REQUIRE(ports.size() == 10);
    CHECK(ports.front() == 13618);
    CHECK(ports.back() == 13627);
    CHECK(std::is_sorted(ports.begin(), ports.end()));
}

TEST_CASE("the first port nothing listens on is taken", "[McpPortChoice]")
{
    std::vector<Port> tried;
    const PortChoice  choice = choose_port({13618, 13619, 13620}, listeners_on({}), listens_except({}, &tried));
    REQUIRE(choice.port);
    CHECK(*choice.port == 13618);
    CHECK(choice.skipped.empty());
    CHECK(tried == std::vector<Port>{13618});
}

TEST_CASE("a port something listens on is passed over without binding it", "[McpPortChoice]")
{
    std::vector<Port> tried;
    const PortChoice  choice = choose_port({13618, 13619, 13620}, listeners_on({13618}), listens_except({}, &tried));
    REQUIRE(choice.port);
    CHECK(*choice.port == 13619);
    CHECK(tried == std::vector<Port>{13619}); // 13618 was never bound: binding beside a listener can succeed
    REQUIRE(choice.skipped.size() == 1);
    CHECK(choice.skipped[0] == "13618: in use");
}

TEST_CASE("a port that cannot be bound is passed over too", "[McpPortChoice]")
{
    // Two instances starting at the same moment both find a port free; one of them loses the bind.
    const PortChoice choice = choose_port({13618, 13619}, listeners_on({}), listens_except({13618}));
    REQUIRE(choice.port);
    CHECK(*choice.port == 13619);
    REQUIRE(choice.skipped.size() == 1);
    CHECK(choice.skipped[0] == "13618: bind: Address already in use");
}

TEST_CASE("with every port taken there is no MCP server, and the reason names each port", "[McpPortChoice]")
{
    const std::vector<Port> ports  = mcp_ports();
    const PortChoice        choice = choose_port(ports, listeners_on(ports), listens_except({}));
    CHECK_FALSE(choice.port);
    CHECK(choice.skipped.size() == ports.size());
    const std::string described = describe_port_choice(choice);
    CHECK(described.find("no MCP server") == 0);
    CHECK(described.find("13618 to 13627") != std::string::npos);
}

TEST_CASE("a listener on 127.0.0.1 is found", "[McpPortChoice]")
{
    Listener listener({boost::asio::ip::address_v4::loopback(), 0});
    CHECK(port_has_listener(listener.port()));
}

TEST_CASE("a listener on every interface is found, as an older OrcaMCP listens", "[McpPortChoice]")
{
    // OrcaMCP 2.5.0.5 binds *:13618. macOS lets a 127.0.0.1 listener bind beside it, and a call could
    // then reach either (2026-09-27), so binding is no test: the connection is.
    Listener listener({tcp::v4(), 0});
    CHECK(port_has_listener(listener.port()));
}

TEST_CASE("a port nothing listens on is free", "[McpPortChoice]")
{
    CHECK_FALSE(port_has_listener(free_loopback_port()));
}

TEST_CASE("an MCP server passes over a port an older OrcaMCP listens on every interface", "[McpPortChoice][HttpServer]")
{
    Listener         older({tcp::v4(), 0});
    const Port       spare = free_loopback_port();
    HttpServer       server;
    const PortChoice choice = choose_port({older.port(), spare}, [](Port port) { return port_has_listener(port); },
                                          [&server](Port port) {
                                              server.set_port(port);
                                              return server.try_start();
                                          });
    REQUIRE(choice.port);
    CHECK(*choice.port == spare);
    CHECK(server.local_endpoint().port() == spare);
    server.stop();
}
