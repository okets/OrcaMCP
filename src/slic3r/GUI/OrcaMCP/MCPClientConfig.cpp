#include "MCPClientConfig.hpp"
#include "OrcaMCPAtomicWrite.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/algorithm/string.hpp>
#include <slic3r/Utils/BoostProcessCompat.hpp>
#include "slic3r/plugin/BundledPython.hpp"  // The app's own Python, for the entries Connect writes
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
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

namespace {

// The bridge script and the tool list it reads: they go to ~/.orcamcp together, since the bridge
// cannot serve a single tool, start_orca included, without its list.
const std::vector<std::string>& bridge_file_names()
{
    static const std::vector<std::string> names{"orcamcp-bridge.py", "orcamcp_tools.json"};
    return names;
}

std::string read_whole_file(const boost::filesystem::path& path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    std::ostringstream      content;
    content << in.rdbuf();
    return content.str();
}

// What Connect and the startup refresh say when no interpreter can run the bridge (the user's words,
// fixed: the docs and troubleshooting page quote them). Each names the install the user actually has:
// an AppImage ships no Python by design and the fix is the distribution's package; any other install
// lost the runtime it ships, so reinstalling is the first thing to try.
const char* const NO_PYTHON_MESSAGE_APPIMAGE = "Connecting needs Python 3.9 or newer, which this AppImage does not "
                                               "include: install your distribution's python3 package, then press "
                                               "Connect again.";
const char* const NO_PYTHON_MESSAGE_INSTALL  = "Connecting needs Python 3.9 or newer, which this OrcaMCP install does "
                                               "not include: reinstall OrcaMCP, or install Python 3.9 or newer, then "
                                               "press Connect again.";

// The system's python3 on PATH, and its version: run once in the bridge's own isolated mode, printing
// "3 12" on stdout, waited for at most 5 s. Anything short of a version read -- none found, no output,
// too slow to answer, a launch that fails -- leaves the inputs as they are, as if there were no
// system Python. Never throws: the app asks for an interpreter on paths that must not fail.
void probe_system_python(MCPClientConfig::InterpreterInputs& inputs)
{
    namespace bp = boost::process;
    try {
        const boost::filesystem::path python3 = bp::search_path("python3");
        if (python3.empty())
            return;

        bp::ipstream out;
        bp::child    proc(python3.string(), std::string("-I"), std::string("-c"),
                          std::string("import sys; print(*sys.version_info[:2])"), bp::std_out > out);
        if (!proc.wait_for(std::chrono::seconds(5))) {
            proc.terminate();
            return;
        }

        std::string line;
        std::getline(out, line);
        // "major" and "minor" are macros in Linux's sysmacros.h: spell them out.
        int              version_major = 0;
        int              version_minor = 0;
        std::istringstream words(line);
        if (!(words >> version_major >> version_minor) || version_major <= 0)
            return;

        inputs.system_python = python3.string();
        inputs.system_major  = version_major;
        inputs.system_minor  = version_minor;
    } catch (...) {
        inputs.system_python.clear();
        inputs.system_major = inputs.system_minor = 0;
    }
}

} // namespace

// The bundled interpreter wherever its path stays put; a Linux AppImage mounts its own at a new path
// every launch, so there, and where no runtime was found, the system's python3 runs the bridge when it
// is 3.9 or newer -- the version the bridge holds itself to (from __future__ import annotations).
MCPClientConfig::InterpreterChoice MCPClientConfig::choose_interpreter(const InterpreterInputs& inputs)
{
    if (!inputs.bundled.empty() && !inputs.in_appimage)
        return InterpreterChoice{inputs.bundled, {}};
    if (!inputs.system_python.empty() && inputs.system_major >= 3 &&
        (inputs.system_major > 3 || inputs.system_minor >= 9))
        return InterpreterChoice{inputs.system_python, {}};
    return InterpreterChoice{{}, inputs.in_appimage ? NO_PYTHON_MESSAGE_APPIMAGE : NO_PYTHON_MESSAGE_INSTALL};
}

MCPClientConfig::InterpreterChoice MCPClientConfig::bridge_interpreter()
{
    InterpreterInputs inputs;
    try {
        const boost::filesystem::path python_home = Slic3r::find_bundled_python_home();
        if (!python_home.empty()) {
            const boost::filesystem::path executable = Slic3r::find_python_executable(python_home);
            if (!executable.empty())
                inputs.bundled = executable.string();
        }
    } catch (...) {
        // A folder the finder could not inspect is "no bundled runtime": the system Python is asked,
        // and this never throws into the startup path or into a Connect click.
        inputs.bundled.clear();
    }
#ifdef __linux__
    const char* appimage = std::getenv("APPIMAGE");
    inputs.in_appimage   = appimage != nullptr && appimage[0] != '\0';
#endif
    // Running a probe costs a process start: ask the system only when the bundled answer is not taken.
    if (inputs.bundled.empty() || inputs.in_appimage)
        probe_system_python(inputs);
    return choose_interpreter(inputs);
}

bool MCPClientConfig::file_content_differs(const std::string& original, const std::string& copy)
{
    if (!boost::filesystem::exists(copy))
        return true;
    return read_whole_file(original) != read_whole_file(copy);
}

bool MCPClientConfig::executable_in_build_tree(const std::string& executable)
{
    boost::system::error_code ec;
    for (boost::filesystem::path folder = boost::filesystem::path(executable).parent_path(); !folder.empty();
         folder = folder.parent_path()) {
        if (boost::filesystem::exists(folder / "CMakeCache.txt", ec))
            return true;
        if (folder == folder.parent_path())
            break;
    }
    return false;
}

MCPClientConfig::BridgeCopyDecision MCPClientConfig::bridge_copy_decision(bool own_data_folder, bool build_tree,
                                                                          bool content_differs)
{
    if (own_data_folder)
        return {false, "it runs on a data folder of its own (--datadir), as every test launch does: ~/.orcamcp is "
                       "left to the installed app, whose bridge the user's own agent sessions run"};
    if (build_tree)
        return {false, "it is a build in a source tree (a CMakeCache.txt above it): ~/.orcamcp is left to the "
                       "installed app, whose bridge the user's own agent sessions run"};
    if (!content_differs)
        return {false, "~/.orcamcp already holds this app's bridge and tool list"};
    return {true, "~/.orcamcp's bridge or tool list differs from this app's"};
}

// Copies the app's bridge and tool list over ~/.orcamcp's when either differs, both together. By
// content, not by date: an installed app older by date than a dev build's copy puts its own back.
bool MCPClientConfig::ensure_bridge_script_copied(std::string& error)
{
    try {
        std::string shared_dir_str = get_shared_scripts_dir();
        if (shared_dir_str.empty()) {
            error = "Could not determine home directory";
            return false;
        }

        boost::filesystem::path dst_dir = shared_dir_str;
        if (!boost::filesystem::exists(dst_dir)) {
            boost::filesystem::create_directories(dst_dir);
        }

        boost::filesystem::path src_dir =
            boost::filesystem::path(resources_dir()) / "scripts";

        for (const std::string& name : bridge_file_names()) {
            if (!boost::filesystem::exists(src_dir / name)) {
                error = "File not found: " + (src_dir / name).string();
                return false;
            }
        }
        if (!shared_bridge_differs())
            return true;
        for (const std::string& name : bridge_file_names())
            boost::filesystem::copy_file(src_dir / name, dst_dir / name, boost::filesystem::copy_option::overwrite_if_exists);
        return true;
    }
    catch (const std::exception& e) {
        error = "Failed to copy bridge scripts: " + std::string(e.what());
        return false;
    }
}

bool MCPClientConfig::shared_bridge_differs()
{
    const std::string shared_dir = get_shared_scripts_dir();
    if (shared_dir.empty())
        return false;
    const boost::filesystem::path src_dir = boost::filesystem::path(resources_dir()) / "scripts";
    for (const std::string& name : bridge_file_names())
        if (file_content_differs((src_dir / name).string(), (boost::filesystem::path(shared_dir) / name).string()))
            return true;
    return false;
}

void MCPClientConfig::refresh_shared_bridge_at_startup(bool own_data_folder, const std::string& executable)
{
    const bool build_tree = executable_in_build_tree(executable);
    // Content is read only when it can decide anything: a test launch never looks.
    const bool                differs  = !own_data_folder && !build_tree && shared_bridge_differs();
    const BridgeCopyDecision decision = bridge_copy_decision(own_data_folder, build_tree, differs);
    if (!decision.copy) {
        BOOST_LOG_TRIVIAL(info) << "MCP bridge: ~/.orcamcp left as it is: " << decision.reason;
        return;
    }
    std::string error;
    if (ensure_bridge_script_copied(error))
        BOOST_LOG_TRIVIAL(info) << "MCP bridge: copied this app's bridge and tool list to ~/.orcamcp: " << decision.reason;
    else
        BOOST_LOG_TRIVIAL(warning) << "MCP bridge: could not copy this app's bridge to ~/.orcamcp: " << error;
}

// Get the bridge script path (shared location)
std::string MCPClientConfig::get_bridge_script_path()
{
    boost::filesystem::path script_path =
        boost::filesystem::path(get_shared_scripts_dir()) / "orcamcp-bridge.py";
    return script_path.string();
}

MCPClientConfig::BridgeCommand MCPClientConfig::bridge_command(const std::string& interpreter, const std::string& bridge_script)
{
    return BridgeCommand{interpreter, {"-I", bridge_script}};
}

// The orca-slicer entry a client's config holds: the bridge command, plus the client's own extra fields.
nlohmann::json MCPClientConfig::server_entry(const MCPClientInfo& info, const BridgeCommand& command)
{
    return updated_entry(info, nlohmann::json(), command);
}

// An entry updated, never replaced: the command, args and the client's required fields are ours, and
// every other field of an entry that already exists -- env (ORCAMCP_TIMEOUT, ORCAMCP_PORT), cwd, a
// client's own flags -- is kept as it is. Our docs tell users to put their settings in `env`.
nlohmann::json MCPClientConfig::updated_entry(const MCPClientInfo& info, const nlohmann::json& existing,
                                              const BridgeCommand& command)
{
    nlohmann::json entry = existing.is_object() ? existing : nlohmann::json::object();
    entry["command"] = command.command;
    entry["args"]    = command.args;

    // Claude Code requires "type": "stdio"
    if (info.requires_type_field)
        entry["type"] = "stdio";

    // GitHub Copilot requires "tools": ["*"] -- and keeps the user's own list when the entry has one.
    if (info.requires_tools_field && !entry.contains("tools"))
        entry["tools"] = nlohmann::json::array({"*"});

    return entry;
}

// `value` as a TOML basic string. Only '"' and '\' need escaping and control characters are written
// \u00XX; everything else, apostrophes and UTF-8 bytes included, goes through as it is.
std::string MCPClientConfig::toml_basic_string(const std::string& value)
{
    static const char HEX_DIGITS[] = "0123456789ABCDEF";

    std::string out("\"");
    for (const unsigned char c : value) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '"')
            out += "\\\"";
        else if (c < 0x20 || c == 0x7F) {
            out += "\\u00";
            out += HEX_DIGITS[c >> 4];
            out += HEX_DIGITS[c & 0x0F];
        } else
            out += static_cast<char>(c);
    }
    out += '"';
    return out;
}

namespace {

// Codex's timeouts Connect writes: room for the bridge to start, and to answer a call (over its own
// 120 s). An existing table's larger value is kept; never shorten what the user asked for.
constexpr int STARTUP_TIMEOUT_SEC = 20;
constexpr int TOOL_TIMEOUT_SEC    = 140;

// The two lines that say what to run, as the new table writes them and an updated one puts back.
std::string toml_command_lines(const MCPClientConfig::BridgeCommand& command)
{
    std::string args = "[";
    for (size_t i = 0; i < command.args.size(); ++i) {
        if (i > 0)
            args += ", ";
        args += MCPClientConfig::toml_basic_string(command.args[i]);
    }
    args += "]";

    return "command = " + MCPClientConfig::toml_basic_string(command.command) + "\n"
           "args = " + args + "\n";
}

} // namespace

std::string MCPClientConfig::toml_server_table(const BridgeCommand& command)
{
    return "[mcp_servers." + SERVER_NAME + "]\n" + toml_command_lines(command) +
           "startup_timeout_sec = " + std::to_string(STARTUP_TIMEOUT_SEC) + "\n"
           "tool_timeout_sec = " + std::to_string(TOOL_TIMEOUT_SEC) + "\n";
}

namespace {

// A TOML table header is a line whose first non-blank character is '['.
bool is_toml_header(const std::string& line)
{
    for (const char c : line) {
        if (c == ' ' || c == '\t')
            continue;
        return c == '[';
    }
    return false;
}

// The key path a table header names: the text between the first '[' and its ']', with spaces and tabs
// outside quoted parts removed and anything after the closing bracket -- a "# comment" -- ignored.
// "" for an array-of-tables header ([[...]]), which is never our table, or a line that is not a header.
std::string toml_header_key(const std::string& line)
{
    const size_t open = line.find('[');
    if (open == std::string::npos)
        return {};
    for (size_t i = 0; i < open; ++i)
        if (line[i] != ' ' && line[i] != '\t')
            return {};
    if (open + 1 < line.size() && line[open + 1] == '[')
        return {};

    std::string key;
    bool        in_quotes = false;
    for (size_t pos = open + 1; pos < line.size(); ++pos) {
        const char c = line[pos];
        if (c == '"') {
            in_quotes = !in_quotes;
            key += c;
        } else if (c == ']' && !in_quotes) {
            return key;
        } else if ((c == ' ' || c == '\t') && !in_quotes) {
            continue;
        } else {
            key += c;
        }
    }
    return {}; // no closing bracket: a header whose key we cannot read
}

// Whether this line of a table's body sets `key`: the key after optional blanks, then optional spaces,
// then '='. (TOML also allows tabs here, so blanks covers both.)
bool sets_toml_key(const std::string& line, const std::string& key)
{
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;
    if (line.compare(pos, key.size(), key) != 0)
        return false;
    pos += key.size();
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;
    return pos < line.size() && line[pos] == '=';
}

// The integer a key line gives, when its whole value is one. A value with anything else in it -- a
// comment, a string, a float -- is not one, and the caller keeps its own.
bool toml_key_int(const std::string& line, int& value)
{
    const size_t equals = line.find('=');
    if (equals == std::string::npos)
        return false;
    const std::string text = boost::algorithm::trim_copy(line.substr(equals + 1));
    try {
        size_t     used = 0;
        const int  read = std::stoi(text, &used);
        if (used != text.size())
            return false;
        value = read;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Whether an "args =" line opens an array that does not close on this line: the removal then runs
// through the line holding the closing ']'.
bool toml_value_opens_multiline(const std::string& line)
{
    const size_t equals = line.find('=');
    if (equals == std::string::npos)
        return false;
    const std::string value = line.substr(equals + 1);
    return value.find('[') != std::string::npos && value.find(']') == std::string::npos;
}

} // namespace

// Our table runs from the header whose key is mcp_servers.orca-slicer (bare or quoted, however the
// brackets are spaced, comment included) through every following [mcp_servers.orca-slicer.*] subtable,
// and stops before the next table header that is not one of them. Non-header lines belong to the
// table they follow.
std::pair<size_t, size_t> MCPClientConfig::toml_table_span(const std::string& content)
{
    const std::string key_bare   = "mcp_servers." + SERVER_NAME;
    const std::string key_quoted = "mcp_servers.\"" + SERVER_NAME + "\"";
    // A following header continues our table only while its key names one of our subtables.
    const std::string subtable_bare   = key_bare + ".";
    const std::string subtable_quoted = key_quoted + ".";

    size_t start = std::string::npos;
    size_t end   = content.size();
    size_t pos   = 0;
    while (pos < content.size()) {
        const size_t      newline  = content.find('\n', pos);
        const size_t      line_end = (newline == std::string::npos) ? content.size() : newline + 1;
        const std::string line     = content.substr(pos, line_end - pos);
        if (is_toml_header(line)) {
            const std::string key = toml_header_key(line);
            const bool        ours = boost::algorithm::starts_with(key, subtable_bare) ||
                                boost::algorithm::starts_with(key, subtable_quoted);
            if (start == std::string::npos) {
                if (key == key_bare || key == key_quoted)
                    start = pos;
            } else if (!ours) {
                end = pos;
                break;
            }
        }
        pos = line_end;
    }
    if (start == std::string::npos)
        return {std::string::npos, 0};
    return {start, end - start};
}

std::string MCPClientConfig::toml_server_table_of(const std::string& content)
{
    const auto span = toml_table_span(content);
    if (span.first == std::string::npos)
        return {};
    return content.substr(span.first, span.second);
}

std::string MCPClientConfig::replace_toml_server_table(const std::string& content, const std::string& table)
{
    const auto    span = toml_table_span(content);
    std::string   rest = content;
    if (span.first != std::string::npos)
        rest = content.substr(0, span.first) + content.substr(span.first + span.second);

    if (!rest.empty()) {
        if (rest.back() != '\n')
            rest += '\n';
        rest += '\n'; // one blank line before the new table
    }
    return rest + table;
}

// `content` with our table's text replaced in place by `table`: the table keeps its position in the
// file. Without our table the content is returned as it is -- appending is replace_toml_server_table's
// job, and write_client_entry only calls this once it has read a table.
std::string MCPClientConfig::replace_toml_server_table_span(const std::string& content, const std::string& table)
{
    const auto span = toml_table_span(content);
    if (span.first == std::string::npos)
        return content;
    return content.substr(0, span.first) + table + content.substr(span.first + span.second);
}

// Codex's table updated, never replaced: the header line stays as it is and every subtable stays
// unchanged, the user's own keys stay in their order -- and the timeouts only ever grow, so a user who
// raised tool_timeout_sec for a slow machine keeps it. What Connect owns is the command, the args and
// the minimum the bridge needs.
std::string MCPClientConfig::updated_toml_server_table(const std::string& existing_table, const BridgeCommand& command)
{
    if (existing_table.empty())
        return toml_server_table(command);

    // Split into lines, each keeping its newline (a Windows file's '\r' stays inside the line and
    // survives untouched). The first line is our header; the main table runs until the first following
    // header, and everything from there on is subtable text kept verbatim.
    std::vector<std::string> lines;
    for (size_t pos = 0; pos < existing_table.size();) {
        const size_t newline = existing_table.find('\n', pos);
        const size_t end     = (newline == std::string::npos) ? existing_table.size() : newline + 1;
        lines.push_back(existing_table.substr(pos, end - pos));
        pos = end;
    }

    size_t main_end = lines.size();
    for (size_t i = 1; i < lines.size(); ++i) {
        if (is_toml_header(lines[i])) {
            main_end = i;
            break;
        }
    }

    // Our keys' values, from the main table only, and every other line of it, in order.
    int         startup = STARTUP_TIMEOUT_SEC;
    int         tool    = TOOL_TIMEOUT_SEC;
    std::string kept;
    for (size_t i = 1; i < main_end; ++i) {
        int value = 0;
        if (sets_toml_key(lines[i], "startup_timeout_sec")) {
            if (toml_key_int(lines[i], value))
                startup = std::max(startup, value);
        } else if (sets_toml_key(lines[i], "tool_timeout_sec")) {
            if (toml_key_int(lines[i], value))
                tool = std::max(tool, value);
        } else if (sets_toml_key(lines[i], "command") || sets_toml_key(lines[i], "args")) {
            // A multi-line args array is removed whole: on through the line that closes it.
            if (sets_toml_key(lines[i], "args") && toml_value_opens_multiline(lines[i]))
                while (++i < main_end && lines[i].find(']') == std::string::npos) {}
        } else {
            kept += lines[i];
        }
    }

    std::string out = lines[0]; // the header line, kept as it is -- comment and spacing included
    if (out.empty() || out.back() != '\n')
        out += '\n';
    out += toml_command_lines(command);
    out += "startup_timeout_sec = " + std::to_string(startup) + "\n";
    out += "tool_timeout_sec = " + std::to_string(tool) + "\n";
    out += kept;
    for (size_t i = main_end; i < lines.size(); ++i)
        out += lines[i];
    return out;
}

namespace {

// Every spelling of our bridge's location ends this way, once '\' reads as '/'.
const std::string& shared_bridge_suffix()
{
    static const std::string suffix = "/.orcamcp/orcamcp-bridge.py";
    return suffix;
}

} // namespace

// Ours only when some argument, Windows separators read as '/', names the shared bridge: an entry
// pointing at a repository script (./scripts/orcamcp-bridge.py) is the developer's, never touched.
bool MCPClientConfig::is_our_entry(const nlohmann::json& entry)
{
    if (!entry.is_object() || !entry.contains("args"))
        return false;
    const nlohmann::json& args = entry.at("args");
    if (!args.is_array())
        return false;

    for (const nlohmann::json& arg : args) {
        if (!arg.is_string())
            continue;
        std::string path = arg.get<std::string>();
        std::replace(path.begin(), path.end(), '\\', '/');
        if (boost::algorithm::ends_with(path, shared_bridge_suffix()))
            return true;
    }
    return false;
}

bool MCPClientConfig::is_our_toml_table(const std::string& table_text)
{
    // A TOML basic string escapes a backslash as "\\"; read both spellings as '/'.
    std::string text = table_text;
    for (size_t pos = text.find("\\\\"); pos != std::string::npos; pos = text.find("\\\\", pos + 1))
        text.replace(pos, 2, "/");
    std::replace(text.begin(), text.end(), '\\', '/');
    return boost::algorithm::contains(text, shared_bridge_suffix());
}

// Write one client's orca-slicer entry, whatever it held before. A JSON config that does not parse is
// refused here and stays byte for byte as it is; every write that happens is atomic, so no client ever
// reads a half-written config.
bool MCPClientConfig::write_client_entry(const MCPClientInfo& info, const BridgeCommand& command, std::string& error)
{
    using OrcaMCP::write_file_atomically;
    using OrcaMCP::WritePermissions;
    try {
        const boost::filesystem::path path(info.config_path);
        const boost::filesystem::path parent = path.parent_path();
        if (!parent.empty() && !boost::filesystem::exists(parent))
            boost::filesystem::create_directories(parent);

        std::string content;
        if (info.is_toml) {
            // An entry is updated where it stands, so its position, its own keys and its subtables
            // survive; a client without one gains a fresh table at the end.
            const std::string before   = read_whole_file(path);
            const std::string existing = toml_server_table_of(before);
            content = existing.empty()
                          ? replace_toml_server_table(before, toml_server_table(command))
                          : replace_toml_server_table_span(before, updated_toml_server_table(existing, command));
        } else {
            nlohmann::json config;
            if (!read_json_config(info.config_path, config, error))
                return false;
            if (!config.contains(info.servers_key) || !config[info.servers_key].is_object())
                config[info.servers_key] = nlohmann::json::object();
            const nlohmann::json existing = config[info.servers_key].contains(SERVER_NAME)
                                                ? config[info.servers_key][SERVER_NAME]
                                                : nlohmann::json();
            config[info.servers_key][SERVER_NAME] = updated_entry(info, existing, command);
            content = config.dump(2);
        }

        const std::string write_error = write_file_atomically(path, content, WritePermissions::KeepExisting);
        if (!write_error.empty()) {
            error = write_error;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = "Error writing client config: " + std::string(e.what());
        return false;
    }
}

// Get MCP server config as JSON string for copying to project .mcp.json
std::string MCPClientConfig::get_mcp_server_json()
{
    // A client with neither extra field: the snippet is for a hand-written .mcp.json, built exactly
    // like the entries Connect writes.
    MCPClientInfo info{};
    info.requires_type_field  = false;
    info.requires_tools_field = false;

    const InterpreterChoice interpreter = bridge_interpreter();
    // A hint the user pastes by hand: "python3" is all it can name when this app finds no interpreter.
    const std::string command = interpreter.error.empty() ? interpreter.path : std::string("python3");

    const nlohmann::json entry = server_entry(info, bridge_command(command, get_bridge_script_path()));
    return "\"" + SERVER_NAME + "\": " + entry.dump(2);
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

// Write JSON config file, atomically and keeping the file's own permissions: a crash or a full disk
// must never leave a half-written client config behind.
bool MCPClientConfig::write_json_config(const std::string& path, const nlohmann::json& config, std::string& error)
{
    try {
        // Ensure parent directory exists
        boost::filesystem::path fs_path(path);
        boost::filesystem::path parent = fs_path.parent_path();
        if (!parent.empty() && !boost::filesystem::exists(parent)) {
            boost::filesystem::create_directories(parent);
        }

        const std::string write_error = OrcaMCP::write_file_atomically(path, config.dump(2),  // Pretty print with 2-space indent
                                                                       OrcaMCP::WritePermissions::KeepExisting);
        if (!write_error.empty()) {
            error = write_error;
            return false;
        }
        return true;
    }
    catch (const std::exception& e) {
        error = "Error writing config file: " + std::string(e.what());
        return false;
    }
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

    // The entry runs the bridge with the app's own Python. When the app cannot name one, Connect says
    // so and writes nothing, rather than an entry that would die at the client's next start.
    const InterpreterChoice interpreter = bridge_interpreter();
    if (!interpreter.error.empty()) {
        error_message = interpreter.error;
        return false;
    }

    return write_client_entry(info, bridge_command(interpreter.path, get_bridge_script_path()), error_message);
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

        // Trim trailing whitespace and add single newline; write back atomically, like every other
        // client config write.
        boost::algorithm::trim_right(new_content);
        new_content += "\n";
        const std::string write_error = OrcaMCP::write_file_atomically(path, new_content,
                                                                       OrcaMCP::WritePermissions::KeepExisting);
        if (!write_error.empty()) {
            error = write_error;
            return false;
        }
        return true;
    }
    catch (const std::exception& e) {
        error = "Error modifying TOML config: " + std::string(e.what());
        return false;
    }
}

// One client's entry, kept current: an entry of ours is rewritten when it differs from what Connect
// would write now, and never when it is not ours or there is none. A config that does not parse fails
// and stays untouched.
EntryRefresh MCPClientConfig::refresh_client_entry(const MCPClientInfo& info, const BridgeCommand& expected,
                                                   std::string& error)
{
    error.clear();
    try {
        if (!config_file_exists(info.config_path))
            return EntryRefresh::NoEntry;

        if (info.is_toml) {
            const std::string content = read_whole_file(info.config_path);
            const std::string table   = toml_server_table_of(content);
            if (table.empty())
                return EntryRefresh::NoEntry;
            if (!is_our_toml_table(table))
                return EntryRefresh::NotOurs;
            if (updated_toml_server_table(table, expected) == table)
                return EntryRefresh::Current;
            return write_client_entry(info, expected, error) ? EntryRefresh::Rewritten : EntryRefresh::Failed;
        }

        nlohmann::json config;
        if (!read_json_config(info.config_path, config, error))
            return EntryRefresh::Failed;
        if (!config.contains(info.servers_key) || !config[info.servers_key].is_object() ||
            !config[info.servers_key].contains(SERVER_NAME))
            return EntryRefresh::NoEntry;

        const nlohmann::json& entry = config[info.servers_key][SERVER_NAME];
        if (!is_our_entry(entry))
            return EntryRefresh::NotOurs;
        if (updated_entry(info, entry, expected) == entry)
            return EntryRefresh::Current;
        return write_client_entry(info, expected, error) ? EntryRefresh::Rewritten : EntryRefresh::Failed;
    }
    catch (const std::exception& e) {
        error = "Error refreshing client entry: " + std::string(e.what());
        return EntryRefresh::Failed;
    }
}

// The user's client entries follow the same rule as ~/.orcamcp: the installed app keeps them current,
// never a test launch and never a build. This repairs an old entry's "python3" -- which most machines
// have no working answer for -- on the first launch of a new version, and follows the app when it moves.
void MCPClientConfig::refresh_client_entries_at_startup(bool own_data_folder, const std::string& executable)
{
    if (own_data_folder) {
        BOOST_LOG_TRIVIAL(info) << "MCP client entries: left as they are: this launch runs on a data folder of "
                                   "its own (--datadir), as every test launch does: the user's client configs "
                                   "are left to the installed app";
        return;
    }
    if (executable_in_build_tree(executable)) {
        BOOST_LOG_TRIVIAL(info) << "MCP client entries: left as they are: this is a build in a source tree "
                                   "(a CMakeCache.txt above it): the user's client configs are left to the "
                                   "installed app";
        return;
    }

    const InterpreterChoice interpreter = bridge_interpreter();
    if (!interpreter.error.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "MCP client entries: left as they are: " << interpreter.error;
        return;
    }

    const BridgeCommand command = bridge_command(interpreter.path, get_bridge_script_path());
    for (const MCPClientInfo& info : get_all_clients()) {
        std::string error;
        switch (refresh_client_entry(info, command, error)) {
        case EntryRefresh::Rewritten:
            BOOST_LOG_TRIVIAL(info) << "MCP client entries: rewrote the orca-slicer entry in " << info.config_path;
            break;
        case EntryRefresh::Failed:
            BOOST_LOG_TRIVIAL(warning) << "MCP client entries: could not rewrite " << info.config_path << ": " << error;
            break;
        case EntryRefresh::NoEntry:
        case EntryRefresh::NotOurs:
        case EntryRefresh::Current:
            break;
        }
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
