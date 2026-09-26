// src/slic3r/GUI/OrcaMCP/OrcaMCPJsonRpcError.hpp
#pragma once
#include <stdexcept>
#include <string>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A request failure with its own JSON-RPC error code, where -32603 Internal error would be wrong.
// OrcaMCPServer::handle_request answers every one of them with its code and message.
struct JsonRpcError : std::runtime_error
{
    JsonRpcError(int code, const std::string& message) : std::runtime_error(message), code(code) {}
    int code;
};

// -32002: the app is quitting, so the call was not run. Not "starting up" (-32001): retrying will not
// help, starting the app again will.
struct McpShuttingDown : JsonRpcError
{
    static constexpr int error_code = -32002;
    McpShuttingDown()
        : JsonRpcError(error_code, "OrcaMCP is quitting, so this call was not run. Use start_orca to start it again.")
    {}
};

}}} // namespace Slic3r::GUI::OrcaMCP
