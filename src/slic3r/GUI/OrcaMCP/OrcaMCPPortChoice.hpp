// src/slic3r/GUI/OrcaMCP/OrcaMCPPortChoice.hpp
#pragma once
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Which port an OrcaMCP instance's MCP server listens on, so several instances can run at once.
//
// Each instance takes the first free port of a small fixed range, 13618 first: the port every older
// OrcaMCP and every bridge without a registry assumes. A port is free only when nothing accepts a
// connection on it at 127.0.0.1 or [::1], the addresses a local client reaches. Binding alone is not
// enough to tell: macOS lets a 127.0.0.1 listener bind beside one on every interface (OrcaMCP 2.5.0.5
// and older, and upstream's login server, bind *:13618), so two instances listened on one port and a
// call could reach either (2026-09-27: an agent's new_project reached the user's open app). A bind
// without SO_REUSEADDR would tell, but it also fails for the closed connections a quit leaves behind for
// up to a minute, so a relaunch would lose its port for nothing; a connection has no such false alarm.
//
// A listener bound to another address only is not looked for: no local client's connection reaches it.
//
// Unit-tested in tests/slic3rutils/test_mcp_port_choice.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

using Port = unsigned short;

constexpr Port   first_mcp_port = 13618;
constexpr size_t mcp_port_count = 10;

// 13618 to 13627, in the order they are tried.
std::vector<Port> mcp_ports();

// True when something accepts a connection on `port` at 127.0.0.1 or [::1], or holds a connection
// attempt past `timeout` (a listener whose queue is full). False when both are refused or unreachable.
bool port_has_listener(Port port, std::chrono::milliseconds timeout = std::chrono::milliseconds(200));

struct PortChoice
{
    std::optional<Port>      port;    // the port now listened on; nothing when every port was taken
    std::vector<std::string> skipped; // why each port before it was passed over, e.g. "13618: in use"
};

// Tries `ports` in order: one `has_listener` says is in use is skipped; otherwise `listen` is asked to
// listen on it, and returns empty once it does, or why it could not (it is then skipped too).
PortChoice choose_port(const std::vector<Port>& ports, const std::function<bool(Port)>& has_listener,
                       const std::function<std::string(Port)>& listen);

// One line for the log and the notification: which port, or why none.
std::string describe_port_choice(const PortChoice& choice);

}}} // namespace Slic3r::GUI::OrcaMCP
