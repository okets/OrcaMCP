#ifndef slic3r_MCPClientConfig_hpp_
#define slic3r_MCPClientConfig_hpp_

#include <string>
#include <vector>
#include <map>
#include <utility>
#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI {

/**
 * MCP Client Auto-Configuration
 *
 * Manages automatic configuration of MCP clients (Claude Code, Cursor, etc.)
 * to connect to OrcaSlicer's MCP server via the bridge script.
 */

// Supported MCP client identifiers
enum class MCPClientId {
    ClaudeDesktop,
    ClaudeCode,
    Cursor,
    Windsurf,
    Cline,
    CodexCLI,
    GitHubCopilot
};

// Client installation/configuration status
enum class MCPClientStatus {
    NotInstalled,      // Config directory does not exist
    NotConfigured,     // Directory exists, orca-slicer not in config
    Configured         // orca-slicer entry present in config
};

// Client metadata structure
struct MCPClientInfo {
    MCPClientId     id;
    std::string     name;
    std::string     description;
    std::string     config_path_template;  // Path template with ~ and %APPDATA%
    std::string     config_path;           // Resolved absolute path
    std::string     servers_key;           // "mcpServers" or "servers" or "mcp_servers"
    bool            is_toml;               // true for Codex CLI (TOML format)
    bool            requires_type_field;   // true for Claude Code (needs type: "stdio")
    bool            requires_tools_field;  // true for GitHub Copilot (needs tools: ["*"])
};

class MCPClientConfig
{
public:
    // Get list of all supported clients with resolved paths
    static std::vector<MCPClientInfo> get_all_clients();

    // Get info for a specific client
    static MCPClientInfo get_client_info(MCPClientId client_id);

    // Check status of a specific client
    static MCPClientStatus get_client_status(MCPClientId client_id);

    // Configure a client (add orca-slicer entry)
    // Returns true on success, false on failure with error message
    static bool connect_client(MCPClientId client_id, std::string& error_message);

    // Remove orca-slicer from client config
    // Returns true on success, false on failure with error message
    static bool disconnect_client(MCPClientId client_id, std::string& error_message);

    // Get the bridge script path (bundled with app)
    static std::string get_bridge_script_path();

    // The command a client runs to start the bridge: the interpreter, and the bridge script in isolated
    // mode ("-I": no PYTHON* variables and no user site-packages, so the user's own Python setup cannot
    // break the bridge).
    struct BridgeCommand
    {
        std::string                command;
        std::vector<std::string>   args;
    };
    static BridgeCommand bridge_command(const std::string& interpreter, const std::string& bridge_script);

    // The orca-slicer entry a client's config holds: the command to run, plus the extra fields that
    // client needs ("type": "stdio", "tools": ["*"]).
    static nlohmann::json server_entry(const MCPClientInfo& info, const BridgeCommand& command);

    // `value` as a TOML basic string: quoted, with '\' and '"' escaped and control characters as \u00XX.
    static std::string toml_basic_string(const std::string& value);

    // Codex's [mcp_servers.orca-slicer] table: the command to run, and timeouts wide enough for the
    // bridge to start (startup_timeout_sec) and to answer a call (tool_timeout_sec, over its 120 s).
    static std::string toml_server_table(const BridgeCommand& command);

    // The exact text of the [mcp_servers.orca-slicer] table -- bare or quoted header -- and every
    // [mcp_servers.orca-slicer.*] subtable that follows it, up to the next other table header.
    // "" when the content holds no such table.
    static std::string toml_server_table_of(const std::string& content);

    // `content` without that table, `table` appended after one blank line; every other line of the
    // content kept byte for byte.
    static std::string replace_toml_server_table(const std::string& content, const std::string& table);

    // Whether a JSON entry is one of ours: one of its args, with '\' read as '/', names the shared bridge.
    static bool is_our_entry(const nlohmann::json& entry);
    // The same for a Codex TOML table text, which may spell a backslash as "\\" or "\".
    static bool is_our_toml_table(const std::string& table_text);

    // Get MCP server config as JSON string for copying to project .mcp.json
    static std::string get_mcp_server_json();

    // Test if MCP server is responding on port 13618
    static bool test_mcp_server();

    // Copies this app's bridge script and its tool list (orcamcp_tools.json) into ~/.orcamcp when either
    // differs in content from the copy there; both go together. Connecting a client does it.
    static bool ensure_bridge_script_copied(std::string& error);

    // At startup: whether ~/.orcamcp gets this app's bridge, logged either way. The user's own agent
    // sessions run the bridge there, so a test launch never touches it: one on a data folder of its own
    // (--datadir, which every test launch has) or of a build in a source tree (`executable` under a
    // CMakeCache.txt). Any other launch -- the installed app's, an agent's start_orca included -- leaves
    // it matching this app, by content: every dev-build launch used to replace it, and the installed app,
    // older by date, never put its own back.
    static void refresh_shared_bridge_at_startup(bool own_data_folder, const std::string& executable);

    // Whether `executable` is a build in a source tree: a folder above it holds a CMakeCache.txt.
    static bool executable_in_build_tree(const std::string& executable);

    struct BridgeCopyDecision
    {
        bool        copy = false;
        std::string reason; // for the log
    };
    // refresh_shared_bridge_at_startup's decision, apart from the files (tested in test_mcp_client_config.cpp).
    static BridgeCopyDecision bridge_copy_decision(bool own_data_folder, bool build_tree, bool content_differs);

    // Whether `copy` holds other bytes than `original`; a missing copy differs.
    static bool file_content_differs(const std::string& original, const std::string& copy);

    // The per-user ~/.orcamcp folder (%USERPROFILE%\.orcamcp on Windows): the bridge script, and the
    // instance registry's entries (OrcaMCPInstanceRegistry.hpp). Empty without a home folder.
    static std::string get_shared_scripts_dir();

private:
    // Whether ~/.orcamcp's bridge or tool list differs from this app's.
    static bool shared_bridge_differs();

    // Platform-specific path resolution
    static std::string resolve_config_path(const std::string& path_template);
    static std::string expand_home_directory(const std::string& path);
    static std::string expand_environment_variables(const std::string& path);

    // Directory/file existence checks
    static bool config_directory_exists(const std::string& config_path);
    static bool config_file_exists(const std::string& config_path);

    // JSON config manipulation
    static bool read_json_config(const std::string& path, nlohmann::json& config, std::string& error);
    static bool write_json_config(const std::string& path, const nlohmann::json& config, std::string& error);
    static bool is_configured_in_json(const std::string& path, const std::string& servers_key);

    // TOML config manipulation (for Codex CLI)
    static bool is_configured_in_toml(const std::string& path);
    static bool add_to_toml_config(const std::string& path, std::string& error);
    static bool remove_from_toml_config(const std::string& path, std::string& error);
    // Where our table sits in a TOML content: start and length, or {npos, 0} when there is none.
    static std::pair<size_t, size_t> toml_table_span(const std::string& content);

    // Client metadata storage
    static std::map<MCPClientId, MCPClientInfo> s_client_metadata;
    static bool s_metadata_initialized;
    static void init_client_metadata();

    // Server name used in config files
    static const std::string SERVER_NAME;
};

}} // namespace Slic3r::GUI

#endif // slic3r_MCPClientConfig_hpp_
