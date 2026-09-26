#ifndef slic3r_OrcaMCPServer_hpp_
#define slic3r_OrcaMCPServer_hpp_

#include <string>
#include <map>
#include <functional>
#include <memory>
#include <vector>
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

    // The categories of CLAUDE.md's tool table, in the order get_server_info lists them.
    enum class ToolCategory {
        Scene,
        Models,
        Transforms,
        Plates,
        Config,
        PerObject,
        LayerRanges,
        FilamentsColour,
        Painting,
        Slicing,
        Visualization,
        Printers,
        Adaptive,
        History,
        Info,
    };
    static const std::vector<ToolCategory>& all_tool_categories();
    static const char*                      tool_category_name(ToolCategory category);

    // Tool definition structure. The category comes straight after the name, so a registration
    // written without one puts its description where the enum goes and does not compile.
    struct ToolDefinition {
        std::string name;
        ToolCategory category;
        std::string summary;          // get_server_info's catalogue line: one line, at most 40 characters
        std::string description;
        nlohmann::json input_schema;  // JSON Schema for parameters
        ToolHandler handler;
    };
    static constexpr size_t max_summary_length = 40;

    // Initialize the MCP server and register all tools
    static void init();

    // Handle incoming HTTP requests for MCP endpoint
    static std::shared_ptr<HttpServer::Response> handle_request(
        const std::string& method,
        const std::string& url,
        const std::string& body);

    // Register a tool with the MCP server. Throws std::logic_error for a name that is already
    // registered, or a tool with no handler.
    static void register_tool(const ToolDefinition& tool);

    // Every registered tool, keyed by name. Registers them first if
    // nothing has yet; unlike init() it has no other side effect, so a unit test can call it.
    static const std::map<std::string, ToolDefinition>& registered_tools();

    // The version every MCP surface reports: SoftFever_VERSION, from version.inc.
    static std::string version();

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
    static bool s_tools_registered;
    static bool s_initialized;

    // Register every tool once, with no other side effect
    static void ensure_tools_registered();

    // Register all built-in tools
    static void register_builtin_tools();
    // Filament and mixed-filament tools (OrcaMCPFilamentTools.cpp)
    static void register_filament_tools();
    // Printer and physical-printer tools (OrcaMCPPrinterTools.cpp)
    static void register_printer_tools();
    // Facet painting and brim ears (OrcaMCPPaintTools.cpp)
    static void register_paint_tools();
};

}} // namespace Slic3r::GUI

#endif // slic3r_OrcaMCPServer_hpp_
