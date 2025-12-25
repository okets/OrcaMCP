#include "MCPClientConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/algorithm/string.hpp>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <fstream>
#include <sstream>
#include <regex>

#include "libslic3r/Utils.hpp"  // For resources_dir()

namespace Slic3r { namespace GUI {

// Static member definitions
std::map<MCPClientId, MCPClientInfo> MCPClientConfig::s_client_metadata;
bool MCPClientConfig::s_metadata_initialized = false;
const std::string MCPClientConfig::SERVER_NAME = "orca-slicer";

// Initialize client metadata with platform-specific paths
void MCPClientConfig::init_client_metadata()
{
    if (s_metadata_initialized)
        return;

    // Claude Desktop
    {
        MCPClientInfo info;
        info.id = MCPClientId::ClaudeDesktop;
        info.name = "Claude Desktop";
        info.description = "Anthropic Claude desktop application";
#ifdef __APPLE__
        info.config_path_template = "~/Library/Application Support/Claude/claude_desktop_config.json";
#elif defined(_WIN32)
        info.config_path_template = "%APPDATA%/Claude/claude_desktop_config.json";
#else
        info.config_path_template = "~/.config/Claude/claude_desktop_config.json";
#endif
        info.servers_key = "mcpServers";
        info.is_toml = false;
        info.requires_type_field = false;
        info.requires_tools_field = false;
        s_client_metadata[MCPClientId::ClaudeDesktop] = info;
    }

    // Claude Code
    {
        MCPClientInfo info;
        info.id = MCPClientId::ClaudeCode;
        info.name = "Claude Code";
        info.description = "Anthropic Claude CLI for developers";
        info.config_path_template = "~/.claude.json";  // Same on all platforms
        info.servers_key = "mcpServers";
        info.is_toml = false;
        info.requires_type_field = true;  // IMPORTANT: Claude Code requires type: "stdio"
        info.requires_tools_field = false;
        s_client_metadata[MCPClientId::ClaudeCode] = info;
    }

    // Cursor
    {
        MCPClientInfo info;
        info.id = MCPClientId::Cursor;
        info.name = "Cursor";
        info.description = "AI-first code editor";
        info.config_path_template = "~/.cursor/mcp.json";
        info.servers_key = "mcpServers";
        info.is_toml = false;
        info.requires_type_field = false;
        info.requires_tools_field = false;
        s_client_metadata[MCPClientId::Cursor] = info;
    }

    // Windsurf
    {
        MCPClientInfo info;
        info.id = MCPClientId::Windsurf;
        info.name = "Windsurf";
        info.description = "AI-powered IDE (Codeium)";
        info.config_path_template = "~/.codeium/windsurf/mcp_config.json";
        info.servers_key = "mcpServers";
        info.is_toml = false;
        info.requires_type_field = false;
        info.requires_tools_field = false;
        s_client_metadata[MCPClientId::Windsurf] = info;
    }

    // Cline (VS Code extension)
    {
        MCPClientInfo info;
        info.id = MCPClientId::Cline;
        info.name = "Cline";
        info.description = "VS Code AI coding assistant extension";
#ifdef __APPLE__
        info.config_path_template = "~/Library/Application Support/Code/User/globalStorage/saoudrizwan.claude-dev/settings/cline_mcp_settings.json";
#elif defined(_WIN32)
        info.config_path_template = "%APPDATA%/Code/User/globalStorage/saoudrizwan.claude-dev/settings/cline_mcp_settings.json";
#else
        info.config_path_template = "~/.config/Code/User/globalStorage/saoudrizwan.claude-dev/settings/cline_mcp_settings.json";
#endif
        info.servers_key = "mcpServers";
        info.is_toml = false;
        info.requires_type_field = false;
        info.requires_tools_field = false;
        s_client_metadata[MCPClientId::Cline] = info;
    }

    // Codex CLI (TOML format)
    {
        MCPClientInfo info;
        info.id = MCPClientId::CodexCLI;
        info.name = "Codex CLI";
        info.description = "OpenAI Codex command-line tool";
        info.config_path_template = "~/.codex/config.toml";
        info.servers_key = "mcp_servers";  // TOML uses underscore
        info.is_toml = true;               // TOML format, not JSON
        info.requires_type_field = false;
        info.requires_tools_field = false;
        s_client_metadata[MCPClientId::CodexCLI] = info;
    }

    // GitHub Copilot CLI
    {
        MCPClientInfo info;
        info.id = MCPClientId::GitHubCopilot;
        info.name = "GitHub Copilot CLI";
        info.description = "GitHub Copilot command-line tool";
        info.config_path_template = "~/.copilot/mcp-config.json";
        info.servers_key = "mcpServers";
        info.is_toml = false;
        info.requires_type_field = false;
        info.requires_tools_field = true;  // IMPORTANT: Copilot requires tools: ["*"]
        s_client_metadata[MCPClientId::GitHubCopilot] = info;
    }

    s_metadata_initialized = true;
}

// Expand ~ to home directory
std::string MCPClientConfig::expand_home_directory(const std::string& path)
{
    if (path.empty() || path[0] != '~')
        return path;

    std::string home;
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    home = userprofile ? userprofile : "";
#else
    const char* home_env = std::getenv("HOME");
    home = home_env ? home_env : "";
#endif

    if (home.empty())
        return path;

    // Replace ~ with home directory
    return home + path.substr(1);
}

// Expand environment variables like %APPDATA%
std::string MCPClientConfig::expand_environment_variables(const std::string& path)
{
    std::string result = path;

#ifdef _WIN32
    // Expand %APPDATA%
    size_t pos = result.find("%APPDATA%");
    if (pos != std::string::npos) {
        const char* appdata = std::getenv("APPDATA");
        if (appdata) {
            result.replace(pos, 9, appdata);
        }
    }

    // Expand %USERPROFILE%
    pos = result.find("%USERPROFILE%");
    if (pos != std::string::npos) {
        const char* userprofile = std::getenv("USERPROFILE");
        if (userprofile) {
            result.replace(pos, 13, userprofile);
        }
    }
#endif

    return result;
}

// Resolve a config path template to an absolute path
std::string MCPClientConfig::resolve_config_path(const std::string& path_template)
{
    std::string path = path_template;

    // First expand environment variables
    path = expand_environment_variables(path);

    // Then expand home directory
    path = expand_home_directory(path);

    // Normalize path separators
    boost::filesystem::path fs_path(path);
    return fs_path.make_preferred().string();
}

// Check if config directory exists
bool MCPClientConfig::config_directory_exists(const std::string& config_path)
{
    boost::filesystem::path fs_path(config_path);
    boost::filesystem::path parent = fs_path.parent_path();
    return boost::filesystem::exists(parent) && boost::filesystem::is_directory(parent);
}

// Check if config file exists
bool MCPClientConfig::config_file_exists(const std::string& config_path)
{
    return boost::filesystem::exists(config_path);
}

// Get the shared scripts directory path (~/.orcamcp)
std::string MCPClientConfig::get_shared_scripts_dir()
{
    std::string home;
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    home = userprofile ? userprofile : "";
#else
    const char* home_env = std::getenv("HOME");
    home = home_env ? home_env : "";
#endif

    if (home.empty())
        return "";

    boost::filesystem::path shared_dir = boost::filesystem::path(home) / ".orcamcp";
    return shared_dir.string();
}

// Ensure bridge script is copied from app bundle to shared location
bool MCPClientConfig::ensure_bridge_script_copied(std::string& error)
{
    try {
        // Source: app bundle
        boost::filesystem::path src =
            boost::filesystem::path(resources_dir()) / "scripts" / "orcamcp-bridge.py";

        // Destination: shared location
        std::string shared_dir_str = get_shared_scripts_dir();
        if (shared_dir_str.empty()) {
            error = "Could not determine home directory";
            return false;
        }

        boost::filesystem::path dst_dir = shared_dir_str;
        boost::filesystem::path dst = dst_dir / "orcamcp-bridge.py";

        // Create directory if needed
        if (!boost::filesystem::exists(dst_dir)) {
            boost::filesystem::create_directories(dst_dir);
        }

        // Check if source exists
        if (!boost::filesystem::exists(src)) {
            error = "Bridge script not found in app bundle: " + src.string();
            return false;
        }

        // Copy if destination doesn't exist or is older than source
        bool should_copy = !boost::filesystem::exists(dst);
        if (!should_copy) {
            auto src_time = boost::filesystem::last_write_time(src);
            auto dst_time = boost::filesystem::last_write_time(dst);
            should_copy = (src_time > dst_time);
        }

        if (should_copy) {
            boost::filesystem::copy_file(src, dst,
                boost::filesystem::copy_option::overwrite_if_exists);
        }

        return true;
    }
    catch (const std::exception& e) {
        error = "Failed to copy bridge script: " + std::string(e.what());
        return false;
    }
}

// Get the bridge script path (shared location)
std::string MCPClientConfig::get_bridge_script_path()
{
    boost::filesystem::path script_path =
        boost::filesystem::path(get_shared_scripts_dir()) / "orcamcp-bridge.py";
    return script_path.string();
}

// Get all supported clients with resolved paths
std::vector<MCPClientInfo> MCPClientConfig::get_all_clients()
{
    init_client_metadata();

    std::vector<MCPClientInfo> clients;
    for (auto& [id, info] : s_client_metadata) {
        info.config_path = resolve_config_path(info.config_path_template);
        clients.push_back(info);
    }
    return clients;
}

// Get info for a specific client
MCPClientInfo MCPClientConfig::get_client_info(MCPClientId client_id)
{
    init_client_metadata();

    auto it = s_client_metadata.find(client_id);
    if (it != s_client_metadata.end()) {
        MCPClientInfo info = it->second;
        info.config_path = resolve_config_path(info.config_path_template);
        return info;
    }

    // Return empty info if not found
    return MCPClientInfo{};
}

// Check if orca-slicer is configured in a JSON config file
bool MCPClientConfig::is_configured_in_json(const std::string& path, const std::string& servers_key)
{
    if (!config_file_exists(path))
        return false;

    try {
        boost::nowide::ifstream ifs(path);
        if (!ifs)
            return false;

        nlohmann::json config;
        ifs >> config;

        if (!config.contains(servers_key))
            return false;

        auto& servers = config[servers_key];
        return servers.is_object() && servers.contains(SERVER_NAME);
    }
    catch (...) {
        return false;
    }
}

// Check if orca-slicer is configured in a TOML config file
bool MCPClientConfig::is_configured_in_toml(const std::string& path)
{
    if (!config_file_exists(path))
        return false;

    try {
        boost::nowide::ifstream ifs(path);
        if (!ifs)
            return false;

        std::stringstream buffer;
        buffer << ifs.rdbuf();
        std::string content = buffer.str();

        // Check for [mcp_servers.orca-slicer] section
        return content.find("[mcp_servers." + SERVER_NAME + "]") != std::string::npos ||
               content.find("[mcp_servers.\"" + SERVER_NAME + "\"]") != std::string::npos;
    }
    catch (...) {
        return false;
    }
}

// Get status of a specific client
MCPClientStatus MCPClientConfig::get_client_status(MCPClientId client_id)
{
    init_client_metadata();

    auto it = s_client_metadata.find(client_id);
    if (it == s_client_metadata.end())
        return MCPClientStatus::NotInstalled;

    MCPClientInfo info = it->second;
    info.config_path = resolve_config_path(info.config_path_template);

    // Check if config directory exists
    if (!config_directory_exists(info.config_path))
        return MCPClientStatus::NotInstalled;

    // Check if already configured
    bool configured = info.is_toml
        ? is_configured_in_toml(info.config_path)
        : is_configured_in_json(info.config_path, info.servers_key);

    return configured ? MCPClientStatus::Configured : MCPClientStatus::NotConfigured;
}

// Read JSON config file
bool MCPClientConfig::read_json_config(const std::string& path, nlohmann::json& config, std::string& error)
{
    try {
        if (!config_file_exists(path)) {
            // Return empty object for new file
            config = nlohmann::json::object();
            return true;
        }

        boost::nowide::ifstream ifs(path);
        if (!ifs) {
            error = "Cannot open file for reading: " + path;
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());

        // Handle empty file
        if (content.empty() || boost::algorithm::trim_copy(content).empty()) {
            config = nlohmann::json::object();
            return true;
        }

        config = nlohmann::json::parse(content);
        return true;
    }
    catch (const nlohmann::json::parse_error& e) {
        error = "Invalid JSON in config file: " + std::string(e.what());
        return false;
    }
    catch (const std::exception& e) {
        error = "Error reading config file: " + std::string(e.what());
        return false;
    }
}

// Write JSON config file
bool MCPClientConfig::write_json_config(const std::string& path, const nlohmann::json& config, std::string& error)
{
    try {
        // Ensure parent directory exists
        boost::filesystem::path fs_path(path);
        boost::filesystem::path parent = fs_path.parent_path();
        if (!parent.empty() && !boost::filesystem::exists(parent)) {
            boost::filesystem::create_directories(parent);
        }

        boost::nowide::ofstream ofs(path);
        if (!ofs) {
            error = "Cannot open file for writing: " + path;
            return false;
        }

        ofs << config.dump(2);  // Pretty print with 2-space indent
        return true;
    }
    catch (const std::exception& e) {
        error = "Error writing config file: " + std::string(e.what());
        return false;
    }
}

// Generate the MCP server entry for a specific client
nlohmann::json MCPClientConfig::generate_server_entry(MCPClientId client_id)
{
    init_client_metadata();

    auto it = s_client_metadata.find(client_id);
    if (it == s_client_metadata.end())
        return nlohmann::json::object();

    const MCPClientInfo& info = it->second;
    std::string bridge_path = get_bridge_script_path();

    nlohmann::json entry;
    entry["command"] = "python3";
    entry["args"] = nlohmann::json::array({bridge_path});

    // Claude Code requires "type": "stdio"
    if (info.requires_type_field) {
        entry["type"] = "stdio";
    }

    // GitHub Copilot requires "tools": ["*"]
    if (info.requires_tools_field) {
        entry["tools"] = nlohmann::json::array({"*"});
    }

    return entry;
}

// Connect a client (add orca-slicer to config)
bool MCPClientConfig::connect_client(MCPClientId client_id, std::string& error_message)
{
    init_client_metadata();

    // Ensure bridge script is copied to shared location first
    if (!ensure_bridge_script_copied(error_message)) {
        return false;
    }

    auto it = s_client_metadata.find(client_id);
    if (it == s_client_metadata.end()) {
        error_message = "Unknown client ID";
        return false;
    }

    MCPClientInfo info = it->second;
    info.config_path = resolve_config_path(info.config_path_template);

    // Handle TOML format (Codex CLI)
    if (info.is_toml) {
        return add_to_toml_config(info.config_path, error_message);
    }

    // Handle JSON format
    nlohmann::json config;
    if (!read_json_config(info.config_path, config, error_message)) {
        return false;
    }

    // Ensure servers object exists
    if (!config.contains(info.servers_key)) {
        config[info.servers_key] = nlohmann::json::object();
    }

    // Add our server entry
    config[info.servers_key][SERVER_NAME] = generate_server_entry(client_id);

    // Write back
    return write_json_config(info.config_path, config, error_message);
}

// Disconnect a client (remove orca-slicer from config)
bool MCPClientConfig::disconnect_client(MCPClientId client_id, std::string& error_message)
{
    init_client_metadata();

    auto it = s_client_metadata.find(client_id);
    if (it == s_client_metadata.end()) {
        error_message = "Unknown client ID";
        return false;
    }

    MCPClientInfo info = it->second;
    info.config_path = resolve_config_path(info.config_path_template);

    // Handle TOML format (Codex CLI)
    if (info.is_toml) {
        return remove_from_toml_config(info.config_path, error_message);
    }

    // Handle JSON format
    if (!config_file_exists(info.config_path)) {
        error_message = "Config file does not exist";
        return false;
    }

    nlohmann::json config;
    if (!read_json_config(info.config_path, config, error_message)) {
        return false;
    }

    if (!config.contains(info.servers_key)) {
        error_message = SERVER_NAME + " not found in config";
        return false;
    }

    auto& servers = config[info.servers_key];
    if (!servers.contains(SERVER_NAME)) {
        error_message = SERVER_NAME + " not found in config";
        return false;
    }

    servers.erase(SERVER_NAME);

    return write_json_config(info.config_path, config, error_message);
}

// Add orca-slicer to TOML config (Codex CLI)
bool MCPClientConfig::add_to_toml_config(const std::string& path, std::string& error)
{
    try {
        // Ensure parent directory exists
        boost::filesystem::path fs_path(path);
        boost::filesystem::path parent = fs_path.parent_path();
        if (!parent.empty() && !boost::filesystem::exists(parent)) {
            boost::filesystem::create_directories(parent);
        }

        std::string content;
        if (config_file_exists(path)) {
            boost::nowide::ifstream ifs(path);
            if (!ifs) {
                error = "Cannot open file for reading: " + path;
                return false;
            }
            std::stringstream buffer;
            buffer << ifs.rdbuf();
            content = buffer.str();
        }

        // Check if already configured
        if (content.find("[mcp_servers." + SERVER_NAME + "]") != std::string::npos ||
            content.find("[mcp_servers.\"" + SERVER_NAME + "\"]") != std::string::npos) {
            // Already configured
            return true;
        }

        // Generate TOML entry
        std::string bridge_path = get_bridge_script_path();
        std::string toml_entry = "\n[mcp_servers." + SERVER_NAME + "]\n"
                                 "command = \"python3\"\n"
                                 "args = [\"" + bridge_path + "\"]\n";

        // Append to file
        boost::nowide::ofstream ofs(path, std::ios::app);
        if (!ofs) {
            error = "Cannot open file for writing: " + path;
            return false;
        }
        ofs << toml_entry;
        return true;
    }
    catch (const std::exception& e) {
        error = "Error writing TOML config: " + std::string(e.what());
        return false;
    }
}

// Remove orca-slicer from TOML config (Codex CLI)
bool MCPClientConfig::remove_from_toml_config(const std::string& path, std::string& error)
{
    try {
        if (!config_file_exists(path)) {
            error = "Config file does not exist";
            return false;
        }

        boost::nowide::ifstream ifs(path);
        if (!ifs) {
            error = "Cannot open file for reading: " + path;
            return false;
        }

        std::stringstream buffer;
        buffer << ifs.rdbuf();
        std::string content = buffer.str();
        ifs.close();

        // Check if configured
        if (content.find("[mcp_servers." + SERVER_NAME + "]") == std::string::npos &&
            content.find("[mcp_servers.\"" + SERVER_NAME + "\"]") == std::string::npos) {
            error = SERVER_NAME + " not found in config";
            return false;
        }

        // Remove the section using regex
        // Match [mcp_servers.orca-slicer] or [mcp_servers."orca-slicer"] and all following lines until next section
        std::regex section_regex(
            R"(\n?\[mcp_servers\.(?:")" + SERVER_NAME + R"("|)" + SERVER_NAME + R"()\][^\[]*(?=\[|$))",
            std::regex::ECMAScript);

        std::string new_content = std::regex_replace(content, section_regex, "");

        // Write back
        boost::nowide::ofstream ofs(path);
        if (!ofs) {
            error = "Cannot open file for writing: " + path;
            return false;
        }

        // Trim trailing whitespace and add single newline
        boost::algorithm::trim_right(new_content);
        ofs << new_content << "\n";
        return true;
    }
    catch (const std::exception& e) {
        error = "Error modifying TOML config: " + std::string(e.what());
        return false;
    }
}

// Test if MCP server is responding
bool MCPClientConfig::test_mcp_server()
{
    // TODO: Implement HTTP check to localhost:13618/mcp
    // For now, just return true since we're running inside OrcaSlicer
    return true;
}

}} // namespace Slic3r::GUI
