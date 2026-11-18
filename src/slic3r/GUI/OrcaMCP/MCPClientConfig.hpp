#ifndef slic3r_MCPClientConfig_hpp_
#define slic3r_MCPClientConfig_hpp_

#include <string>
#include <vector>
#include <map>
#include <optional>
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

// What refreshing one client's entry at startup did (refresh_client_entry).
enum class EntryRefresh {
    NoEntry,    // The config holds no orca-slicer entry (or there is no config): nothing is created
    NotOurs,    // The entry is not one of ours (it runs no ~/.orcamcp bridge): never touched
    Current,    // The entry is ours and already what Connect would write now
    Rewritten,  // The entry is ours and differed: written anew
    Failed      // The config could not be read or written: left untouched
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

    // An existing entry brought up to date rather than replaced: the command, args and the client's
    // required fields are set from `command`, and every other field the user put in the entry -- env
    // (ORCAMCP_TIMEOUT, ORCAMCP_PORT), cwd, a client's own flags -- is kept. `existing` is used when it
    // is an object and ignored otherwise.
    static nlohmann::json updated_entry(const MCPClientInfo& info, const nlohmann::json& existing,
                                        const BridgeCommand& command);

    // `value` as a TOML basic string: quoted, with '\' and '"' escaped and control characters as \u00XX.
    static std::string toml_basic_string(const std::string& value);

    // Codex's [mcp_servers.orca-slicer] table: the command to run, and timeouts wide enough for the
    // bridge to start (startup_timeout_sec) and to answer a call (tool_timeout_sec, over its 120 s).
    static std::string toml_server_table(const BridgeCommand& command);

    // The exact text of the [mcp_servers.orca-slicer] table -- the header however its key is quoted
    // or spaced, a leading BOM not part of it -- and every [mcp_servers.orca-slicer.*] subtable that
    // follows it, up to the next other table header. "" when the content holds no such table.
    static std::string toml_server_table_of(const std::string& content);

    // Whether the text names our server another way than a table this code can edit: an
    // `orca-slicer = { ... }` line under [mcp_servers], or a dotted key starting
    // mcp_servers.orca-slicer (any quoting), on any line. Outside our table's own span, outside
    // comments and strings. Such an entry is never edited: Connect refuses, the startup refresh
    // leaves the file alone.
    static bool toml_names_server_elsewhere(const std::string& content);

    // `content` without that table, `table` appended after one blank line; every other line of the
    // content kept byte for byte.
    static std::string replace_toml_server_table(const std::string& content, const std::string& table);

    // The table text updated to run `command` rather than replaced by a fresh one: the header line kept
    // as it is, the command and args rewritten, each timeout the larger of the value the main table
    // holds (when its line reads as a plain TOML integer, comment allowed) and the 20 / 140 Connect
    // writes -- a timeout line the reader cannot read, or one already at least as large, kept exactly
    // as the user wrote it, our line not added for that key --, every other line of the main table
    // kept in order, and every subtable kept unchanged. "" -- no table yet -- gives
    // toml_server_table(command). nullopt when the table cannot be updated safely: a command or args
    // value that is a multi-line string, or whose end the reader cannot find. The file is then the
    // user's to fix by hand, and nothing of it is written.
    static std::optional<std::string> updated_toml_server_table(const std::string& existing_table,
                                                                const BridgeCommand& command);

    // `path` without our table and its subtables -- exactly the span toml_server_table_of answers --
    // written back atomically with the file's own permissions. A file that cannot be read, and one
    // that names our server only in a form this code cannot edit, are refused and left untouched.
    static bool remove_from_toml_config(const std::string& path, std::string& error);

    // Whether a JSON entry is one of ours: one of its args, with '\' read as '/', names the shared bridge.
    static bool is_our_entry(const nlohmann::json& entry);
    // The same for a Codex TOML table text, which may spell a backslash as "\\" or "\".
    static bool is_our_toml_table(const std::string& table_text);

    // What an entry's interpreter is chosen from: the app's own bundled Python, whether this app runs
    // from a Linux AppImage (where the bundled path is mounted afresh every launch and so cannot be
    // named in an entry), and the system's python3 with its version, read only when the bundled one
    // is not taken.
    struct InterpreterInputs
    {
        std::string bundled;        // The bundled interpreter's path, "" when there is none
        bool        in_appimage = false;
        std::string system_python;  // The system python3's path, "" when there is none
        int         system_major  = 0;
        int         system_minor  = 0;
    };

    struct InterpreterChoice
    {
        std::string path;   // What an entry runs, "" when nothing does
        std::string error;  // Why nothing does; "" when `path` is set
    };

    // The choice from its inputs, apart from the app: the bundled interpreter unless it is missing or
    // this is an AppImage; else a system python3 of 3.9 or newer (the bridge runs on 3.9); else the
    // error naming the install the user has -- an AppImage, whose fix is the distribution's python3,
    // or any other install, whose fix is reinstalling OrcaMCP or installing Python.
    static InterpreterChoice choose_interpreter(const InterpreterInputs& inputs);

    // The interpreter a new entry runs the bridge with: `choose_interpreter` of this app's inputs --
    // upstream's bundled-Python finder (BundledPython.hpp), APPIMAGE, and a system python3 that is
    // found on PATH and run once for its version when the bundled one is not taken.
    static InterpreterChoice bridge_interpreter();

    // Write one client's orca-slicer entry, updating it where it stands rather than replacing it: the
    // command and args become `command`'s and the user's own settings (env, cwd, other keys, Codex's
    // subtables, a longer timeout) survive. A JSON config is read with its key order kept (an
    // unparsable one, a file that exists but cannot be read, and a servers value that is not an object
    // are refused and left untouched) and goes back through the atomic writer with its own
    // permissions; a TOML config a read fails on, or that names our server only in a form this code
    // cannot edit, is refused and left byte for byte as it is; otherwise its table is replaced in
    // place, every other line and server kept. Parent folders are created when missing.
    static bool write_client_entry(const MCPClientInfo& info, const BridgeCommand& command, std::string& error);

    // One client's entry, kept current at startup: never written when there is none, when it is not
    // ours, or when it is already `expected`; rewritten through write_client_entry otherwise. A file
    // that exists but cannot be read, one that names our server only in a form that cannot be edited
    // (NotOurs, never touched), and one whose table cannot be updated (Failed) all stay as they are.
    static EntryRefresh refresh_client_entry(const MCPClientInfo& info, const BridgeCommand& expected, std::string& error);

    // At startup: bring every client entry that is ours up to date, after refresh_shared_bridge_at_startup
    // has put the bridge in ~/.orcamcp. A test launch and a build in a source tree never touch the user's
    // client configs (the same gate), and each rewrite and failure is logged, nothing shown.
    static void refresh_client_entries_at_startup(bool own_data_folder, const std::string& executable);

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

    // JSON config manipulation. A whole config file is nlohmann::ordered_json, so a rewrite keeps the
    // user's key order; individual entries are compared as plain nlohmann::json, where it matters
    // that the order of an entry's own keys is not a difference.
    static bool read_json_config(const std::string& path, nlohmann::ordered_json& config, std::string& error);
    static bool write_json_config(const std::string& path, const nlohmann::ordered_json& config, std::string& error);
    static bool is_configured_in_json(const std::string& path, const std::string& servers_key);

    // Whether one client's config holds an entry of ours, which the startup refresh may have to bring
    // up to date: read without probing for an interpreter, so a launch that holds none starts no
    // process. A config that cannot be read or parsed counts as one, so refresh_client_entry reports
    // its failure as before.
    static bool config_holds_our_entry(const MCPClientInfo& info, std::string& error);

    // TOML config manipulation (for Codex CLI)
    static bool is_configured_in_toml(const std::string& path);
    // Where our table sits in a TOML content: start and length, or {npos, 0} when there is none.
    static std::pair<size_t, size_t> toml_table_span(const std::string& content);
    // `content` with the bytes of our table's span replaced in place by `table`, so the table keeps its
    // position in the file; `content` unchanged when it holds no such table.
    static std::string replace_toml_server_table_span(const std::string& content, const std::string& table);

    // Client metadata storage
    static std::map<MCPClientId, MCPClientInfo> s_client_metadata;
    static bool s_metadata_initialized;
    static void init_client_metadata();

    // Server name used in config files
    static const std::string SERVER_NAME;
};

}} // namespace Slic3r::GUI

#endif // slic3r_MCPClientConfig_hpp_
