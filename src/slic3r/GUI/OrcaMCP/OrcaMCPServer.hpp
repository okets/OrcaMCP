#ifndef slic3r_OrcaMCPServer_hpp_
#define slic3r_OrcaMCPServer_hpp_

#include <string>
#include <map>
#include <functional>
#include <memory>
#include "nlohmann/json.hpp"
#include "slic3r/GUI/HttpServer.hpp"

namespace Slic3r { namespace GUI {

/**
 * MCP (Model Context Protocol) Server for OrcaSlicer
 *
 * Implements the MCP protocol over HTTP for Claude Code integration.
 * Exposes slicer functionality as MCP tools.
 */
class OrcaMCPServer
{
public:
    // Tool handler function type
    using ToolHandler = std::function<nlohmann::json(const nlohmann::json& params)>;

    // Tool definition structure
    struct ToolDefinition {
        std::string name;
        std::string description;
        nlohmann::json input_schema;  // JSON Schema for parameters
        ToolHandler handler;
    };

    // Initialize the MCP server and register all tools
    static void init();

    // Handle incoming HTTP requests for MCP endpoint
    static std::shared_ptr<HttpServer::Response> handle_request(
        const std::string& method,
        const std::string& url,
        const std::string& body);

    // Register a tool with the MCP server
    static void register_tool(const ToolDefinition& tool);

private:
    // MCP protocol handlers
    static nlohmann::json handle_initialize(const nlohmann::json& params);
    static nlohmann::json handle_tools_list();
    static nlohmann::json handle_tools_call(const nlohmann::json& params);

    // Tool-specific handlers
    static nlohmann::json handle_get_preview_base64(const nlohmann::json& params);

    // JSON-RPC 2.0 response helpers
    static nlohmann::json make_success_response(const nlohmann::json& id, const nlohmann::json& result);
    static nlohmann::json make_error_response(const nlohmann::json& id, int code, const std::string& message);

    // Registered tools
    static std::map<std::string, ToolDefinition> s_tools;
    static bool s_initialized;

    // Register all built-in tools
    static void register_builtin_tools();
};

}} // namespace Slic3r::GUI

#endif // slic3r_OrcaMCPServer_hpp_
