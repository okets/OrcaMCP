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

namespace bp = boost::process;

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

// Reads a config a client entry is edited inside of, telling "there is no file yet" apart from "the
// file is there but I cannot read it": true with the file's bytes, or true with "" when there is no
// file; false with an error when the file exists but cannot be opened or read. A file we cannot read
// must never be replaced by a write of ours, so no config read on an edit path uses read_whole_file,
// which answers a failed open with the empty string.
bool read_existing_file(const boost::filesystem::path& path, std::string& content, std::string& error)
{
    if (!boost::filesystem::exists(path)) {
        content.clear();
        return true;
    }
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    if (!in) {
        error = "Cannot open file for reading: " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        error = "Cannot open file for reading: " + path.string();
        return false;
    }
    content = buffer.str();
    return true;
}

// What Connect says when Codex's config names our server in a TOML form this code will not guess at
// (an inline table or dotted keys), and what an edit would corrupt: the user fixes that form by hand.
std::string toml_undefined_form_error(const std::string& path, const std::string& server_name, const std::string& table_key)
{
    return "Codex's config " + path + " defines " + server_name + " in a form OrcaMCP cannot edit "
           "(an inline table or dotted keys): replace it with a [" + table_key + "] table, or remove it, "
           "then press Connect again.";
}

// What Connect says when our own table is there but rewriting its command and args safely is not
// possible (a multi-line string command, a value whose end cannot be found): the table's two running
// keys are the user's to edit by hand; nothing else of the file is touched.
std::string toml_undatable_table_error(const std::string& path, const std::string& server_name)
{
    return "Codex's config " + path + " has an " + server_name + " table OrcaMCP cannot update "
           "safely: edit its command and args by hand, or remove the table, then press Connect again.";
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

// The probe runs the SYSTEM's python3 the system's way. Inside an AppImage, AppRun has put the
// AppImage's own library folder in LD_LIBRARY_PATH and (for the bundled runtime) PYTHONHOME /
// PYTHONPATH in the environment the probe would inherit: python3 launched with those loads the
// AppImage's libpython against the system's standard library, which fails or prints the wrong
// version. So the probe starts from a copy of this process' environment with exactly those three
// taken out. (Needs no unit test: only a Linux AppImage sets them.)
void strip_appimage_environment(bp::environment& env)
{
    env["LD_LIBRARY_PATH"].clear();
    env["PYTHONHOME"].clear();
    env["PYTHONPATH"].clear();
}

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

        bp::environment env = boost::this_process::environment();
        strip_appimage_environment(env);
        bp::ipstream out;
        bp::child    proc(python3.string(), std::string("-I"), std::string("-c"),
                          std::string("import sys; print(*sys.version_info[:2])"), env, bp::std_out > out);
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

// Every spelling of our bridge's location ends this way, once '\' reads as '/'.
const std::string& shared_bridge_suffix()
{
    static const std::string suffix = "/.orcamcp/orcamcp-bridge.py";
    return suffix;
}

// Whether some path in `text` names the shared bridge, once a doubled '\\' has read as one and any
// remaining '\' as '/'. Callers pass value bytes only: comments never reach here.
bool names_shared_bridge(const std::string& text)
{
    std::string paths = text;
    for (size_t pos = paths.find("\\\\"); pos != std::string::npos; pos = paths.find("\\\\", pos + 1))
        paths.replace(pos, 2, "/");
    std::replace(paths.begin(), paths.end(), '\\', '/');
    return boost::algorithm::contains(paths, shared_bridge_suffix());
}

// What scan_toml_value read: where the value ends, and whether it used a multi-line string.
struct TomlValue
{
    size_t end            = std::string::npos; // npos when the value never closes inside `content`
    bool   multiline_text = false;             // the value opened a """ or ''' string
};

// Whether the characters at `pos` open a multi-line string of the quote character found there.
bool opens_multiline_text(const std::string& content, size_t pos)
{
    const char quote = content[pos];
    return pos + 2 < content.size() && content[pos + 1] == quote && content[pos + 2] == quote;
}

// The one TOML value reader, used wherever a value's real end matters. It answers where the value
// starting at `from` ends: past the bracket that closes an array or inline table, past the closing
// quote of a string, or at the newline that ends a bare value (a trailing comment in none of them).
// Basic strings ("...", '\' escaping) and literal strings ('...') are taken whole, single-line or
// multi-line ("""...""", '''...'''); '#' runs a comment to the end of its line; '[' and '{' nest
// across lines, so a ']' inside a string or a comment never closes an array. `end` is npos when the
// value runs unclosed to the end of `content` -- a caller may not touch such a line.
TomlValue scan_toml_value(const std::string& content, size_t from)
{
    TomlValue result;
    size_t    depth = 0;
    size_t    pos   = from;
    while (pos < content.size()) {
        const char c = content[pos];
        if (c == '#') {
            if (depth == 0) { // a bare value ends where its comment begins
                result.end = pos;
                return result;
            }
            const size_t newline = content.find('\n', pos);
            if (newline == std::string::npos)
                return result; // the array that opened never closed
            pos = newline + 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            if (opens_multiline_text(content, pos)) {
                result.multiline_text = true;
                const std::string closer(3, c);
                const size_t      close = content.find(closer, pos + 3);
                if (close == std::string::npos)
                    return result; // the string never closed: end stays npos
                pos = close + closer.size();
                continue;
            }
            // A basic string's '\' escapes the next character (\" does not close it); a literal string has no escapes.
            size_t quote_end = pos + 1;
            while (quote_end < content.size() && content[quote_end] != c && content[quote_end] != '\n')
                quote_end += (c == '"' && content[quote_end] == '\\' && quote_end + 1 < content.size() && content[quote_end + 1] != '\n') ? 2 : 1;
            if (quote_end >= content.size() || content[quote_end] != c) {
                result.end = std::string::npos; // a single-line string that never closes on its line
                return result;
            }
            pos = quote_end + 1;
            continue;
        }
        if (c == '\n') {
            if (depth == 0) { // a bare value ends at its line's end
                result.end = pos;
                return result;
            }
            ++pos; // the array continues on the next line
            continue;
        }
        if (c == '[' || c == '{') {
            ++depth;
            ++pos;
            continue;
        }
        if (c == ']' || c == '}') {
            if (depth == 0) { // a stray closer with nothing of ours open: treat the value as ended
                result.end = pos;
                return result;
            }
            --depth;
            ++pos;
            if (depth == 0) {
                result.end = pos;
                return result;
            }
            continue;
        }
        ++pos;
    }
    return result; // npos: `content` ran out mid-value
}

// A UTF-8 BOM as the file's first bytes: they are not part of the header on that first line, and a
// rewrite of the table leaves them where they are.
size_t bom_prefix(const std::string& line)
{
    return line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                   static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF
               ? 3
               : 0;
}

// A TOML table header is a line whose first non-blank character is '[', a leading BOM not counting.
bool is_toml_header(const std::string& line)
{
    for (size_t pos = bom_prefix(line); pos < line.size(); ++pos) {
        const char c = line[pos];
        if (c == ' ' || c == '\t')
            continue;
        return c == '[';
    }
    return false;
}

// Whether every character of `text` is one a bare TOML key may hold unquoted.
bool is_bare_toml_key(const std::string& text)
{
    if (text.empty())
        return false;
    for (const char c : text) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!word)
            return false;
    }
    return true;
}

// One dotted-key part as it can be compared: blanks removed, and quotes dropped when their text
// would also fit a bare key -- so "orca-slicer" and 'orca-slicer' both read as orca-slicer while a
// quoted key that is not ours keeps its quotes and never matches ours by accident.
std::string normalized_toml_key_part(const std::string& part)
{
    const size_t first = part.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return part;
    const size_t last  = part.find_last_not_of(" \t\r");
    const char   quote = part[first];
    if ((quote == '"' || quote == '\'') && last > first && part[last] == quote) {
        const std::string inner = part.substr(first + 1, last - first - 1);
        if (is_bare_toml_key(inner))
            return inner;
    }
    return part.substr(first, last - first + 1);
}

// A dotted key as it can be compared: every part trimmed and normalized, spaces outside quotes gone,
// dots inside quotes kept as part of their key.
std::string normalized_toml_dotted_key(const std::string& text)
{
    std::string key;
    size_t      part_start = 0;
    char        quote      = 0;
    for (size_t pos = 0; pos <= text.size(); ++pos) {
        const bool at_end = pos == text.size();
        const char c      = at_end ? '.' : text[pos];
        if (quote != 0) {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c; // a dot inside a quoted part belongs to that part
            continue;
        }
        if (c != '.')
            continue;
        if (!key.empty())
            key += '.';
        key += normalized_toml_key_part(text.substr(part_start, pos - part_start));
        part_start = pos + 1;
        if (at_end)
            break;
    }
    return key;
}

// The key path a table header names, in that comparable form: [mcp_servers."orca-slicer"],
// [mcp_servers.'orca-slicer'] and [ mcp_servers . orca-slicer ] all give mcp_servers.orca-slicer,
// and subtable headers likewise. Anything after the closing bracket -- a "# comment" -- is ignored.
// "" for an array-of-tables header ([[...]]), which is never our table, or a line that is not a
// header or whose bracket never closes.
std::string toml_header_key(const std::string& line)
{
    const size_t open = line.find('[');
    if (open == std::string::npos || open < bom_prefix(line))
        return {};
    for (size_t pos = bom_prefix(line); pos < open; ++pos)
        if (line[pos] != ' ' && line[pos] != '\t')
            return {};
    if (open + 1 < line.size() && line[open + 1] == '[')
        return {};

    std::string body;
    char        quote = 0;
    for (size_t pos = open + 1; pos < line.size(); ++pos) {
        const char c = line[pos];
        if (quote != 0) {
            body += c;
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            body += c;
            quote = c;
        } else if (c == ']') {
            return normalized_toml_dotted_key(body);
        } else if (c == '\n' || c == '#') {
            return {}; // the bracket never closes, or a bare '#' sits inside the key
        } else {
            body += c;
        }
    }
    return {}; // no closing bracket: a header whose key we cannot read
}

// Whether this line of a table's body sets `key`: the key -- bare, "quoted" or 'quoted' -- after
// optional blanks, then optional spaces, then '='. (TOML also allows tabs here, so blanks covers
// both.) A quoted key of another text, or an unterminated quote, is not the key.
bool sets_toml_key(const std::string& line, const std::string& key)
{
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;
    if (pos < line.size() && (line[pos] == '"' || line[pos] == '\'')) {
        const char quote = line[pos];
        const size_t close = line.find(quote, pos + 1);
        if (close == std::string::npos || line.compare(pos + 1, close - pos - 1, key) != 0)
            return false;
        pos = close + 1;
    } else {
        if (line.compare(pos, key.size(), key) != 0)
            return false;
        pos += key.size();
    }
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;
    return pos < line.size() && line[pos] == '=';
}

// The integer a key line gives, read the way TOML reads one: an optional sign, digits with single '_'
// between digits allowed (1_000), then optional blanks and an optional '# comment'. Anything else --
// "600.0", a string, digits that run into another character -- is not a plain integer this code may
// act on, and the caller keeps the user's line exactly as it is.
bool toml_key_int(const std::string& line, int& value)
{
    const size_t equals = line.find('=');
    if (equals == std::string::npos)
        return false;
    size_t pos = equals + 1;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;
    std::string digits;
    if (pos < line.size() && (line[pos] == '+' || line[pos] == '-')) {
        digits += line[pos];
        ++pos;
    }
    bool any_digit = false;
    while (pos < line.size()) {
        const char c = line[pos];
        if (c >= '0' && c <= '9') {
            digits += c;
            any_digit = true;
            ++pos;
            continue;
        }
        // A '_' counts only between two digits; "1__0" or a trailing '_' is not an integer here.
        if (c == '_' && any_digit && pos + 1 < line.size() && line[pos + 1] >= '0' && line[pos + 1] <= '9') {
            ++pos;
            continue;
        }
        break;
    }
    if (!any_digit)
        return false;
    // The value ends at the line's end, or at a comment that runs to the line's end.
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r' || line[pos] == '\n'))
        ++pos;
    if (pos < line.size() && line[pos] != '#')
        return false;
    try {
        value = std::stoi(digits);
    } catch (const std::exception&) {
        return false; // out of an int's range: a value this code will not compare, only keep
    }
    return true;
}

// What one Codex timeout key holds in the user's table: whether any of its lines could not be read
// as a plain TOML integer, and the largest value of the lines that could.
struct TimeoutRead
{
    bool any_unreadable = false;
    int  largest        = 0;

    void note(const std::string& line)
    {
        int value = 0;
        if (toml_key_int(line, value))
            largest = std::max(largest, value);
        else
            any_unreadable = true;
    }

    // Our line goes in only where every value read is smaller than ours: an unreadable value is not
    // ours to reinterpret, and a value at least ours already gives the bridge its minimum.
    bool ours_bigger(int ours) const { return !any_unreadable && largest < ours; }

    // Whether the user's own line stays in the table: everything except the one case where it is
    // replaced by our larger value (and with it, its comment).
    bool keep_their_line(int ours) const { return !ours_bigger(ours); }
};

} // namespace

// Our table runs from the header whose key normalizes to mcp_servers.orca-slicer -- however its key
// is quoted or spaced, a comment behind the bracket included, a leading BOM not part of it --
// through every following [mcp_servers.orca-slicer.*] subtable, and stops before the next table
// header that is not one of them. Non-header lines belong to the table they follow.
std::pair<size_t, size_t> MCPClientConfig::toml_table_span(const std::string& content)
{
    const std::string our_key      = "mcp_servers." + SERVER_NAME;
    const std::string our_subtable = our_key + ".";

    size_t start = std::string::npos;
    size_t end   = content.size();
    size_t pos   = 0;
    while (pos < content.size()) {
        const size_t      newline  = content.find('\n', pos);
        const size_t      line_end = (newline == std::string::npos) ? content.size() : newline + 1;
        const std::string line     = content.substr(pos, line_end - pos);
        if (is_toml_header(line)) {
            const std::string key = toml_header_key(line);
            if (start == std::string::npos) {
                if (key == our_key)
                    start = pos + bom_prefix(line); // the file's BOM stays outside the table's span
            } else if (!boost::algorithm::starts_with(key, our_subtable)) {
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

// Whether the text names our server another way than a table this code can edit: an
// `orca-slicer = { ... }` line under [mcp_servers], or a dotted key starting mcp_servers.orca-slicer
// (any quoting) on any line. Read statement by statement in TOML's own terms -- the one value scanner
// jumps over strings and comments, so neither can hide or fake a key -- and outside our table's own
// span, which is already the editable form.
bool MCPClientConfig::toml_names_server_elsewhere(const std::string& content)
{
    const auto  span      = toml_table_span(content);
    const size_t ours_beg = span.first; // npos when the content holds no table of ours
    const size_t ours_end = (span.first == std::string::npos) ? 0 : span.first + span.second;

    const std::string servers_table = "mcp_servers";
    const std::string our_key       = servers_table + "." + SERVER_NAME;

    std::string table;                      // the table the statements so far sit in; "" before any header
    size_t      pos  = bom_prefix(content); // a BOM is the file's bytes, not a statement's
    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\r' || content[pos] == '\n'))
            ++pos;
        if (pos >= content.size())
            break;
        if (ours_beg != std::string::npos && pos >= ours_beg && pos < ours_end) {
            // Inside our own table: the editable form, already found.
            pos = content.find('\n', pos);
            pos = (pos == std::string::npos) ? content.size() : pos + 1;
            continue;
        }
        if (content[pos] == '#') {
            pos = content.find('\n', pos);
            pos = (pos == std::string::npos) ? content.size() : pos + 1;
            continue;
        }
        if (content[pos] == '[') { // a table header: the statements that follow sit in it
            const size_t newline  = content.find('\n', pos);
            const size_t line_end = (newline == std::string::npos) ? content.size() : newline;
            table                 = toml_header_key(content.substr(pos, line_end - pos));
            pos                   = (newline == std::string::npos) ? content.size() : newline + 1;
            continue;
        }

        // A key/value statement. The key runs to its '=', a quoted part of it stepped over to its own
        // closing quote (as a header's quoted key is); then the value scanner jumps over the value,
        // multi-line strings included, so no line inside it can be mistaken for a key.
        const size_t key_start  = pos;
        size_t       key_end    = content.size();
        bool         has_value  = false;
        while (pos < content.size() && content[pos] != '\n' && content[pos] != '#') {
            const char c = content[pos];
            if (c == '=') {
                key_end   = pos;
                has_value = true;
                ++pos;
                break;
            }
            if (c == '"' || c == '\'') {
                size_t quote_end = pos + 1;
                bool   closed    = false;
                for (;; ++quote_end) {
                    if (quote_end >= content.size() || content[quote_end] == '\n')
                        break; // a quoted key that never closes on its line: not a statement we can read
                    if (c == '"' && content[quote_end] == '\\')
                        ++quote_end; // the escaped character belongs to the string
                    else if (content[quote_end] == c) {
                        closed = true;
                        break;
                    }
                }
                if (!closed) {
                    pos = content.size();
                    break;
                }
                pos = quote_end + 1;
                continue;
            }
            ++pos;
        }
        if (!has_value) { // no '=' on the line: nothing this statement can name
            pos = content.find('\n', pos);
            pos = (pos == std::string::npos) ? content.size() : pos + 1;
            continue;
        }
        const std::string key = normalized_toml_dotted_key(content.substr(key_start, key_end - key_start));
        const TomlValue   value = scan_toml_value(content, pos);
        pos                     = (value.end == std::string::npos) ? content.size() : value.end;

        if (key == our_key || boost::algorithm::starts_with(key, our_key + "."))
            return true;
        if (table == servers_table && key == SERVER_NAME)
            return true;
    }
    return false;
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
// the minimum the bridge needs. A timeout line we cannot read as a plain integer stays exactly as the
// user wrote it and ours is not added for that key, because guessing at a value would change what
// Codex does; a command or args value we cannot read to its real end (a multi-line string, an
// unterminated array) means the whole update is refused -- nothing half-written.
std::optional<std::string> MCPClientConfig::updated_toml_server_table(const std::string& existing_table,
                                                                      const BridgeCommand& command)
{
    if (existing_table.empty())
        return toml_server_table(command);

    // Split into lines, each keeping its newline (a Windows file's '\r' stays inside the line and
    // survives untouched), and remember where each line starts: the one value scanner answers a
    // multi-line value's end in positions of the table text, and whole lines are removed around it.
    std::vector<std::string> lines;
    std::vector<size_t>      line_start;
    for (size_t pos = 0; pos < existing_table.size();) {
        const size_t newline = existing_table.find('\n', pos);
        const size_t end     = (newline == std::string::npos) ? existing_table.size() : newline + 1;
        line_start.push_back(pos);
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

    // The timeouts, decided first: the largest plain integer value the main table gives, and whether
    // any line of the key could not be read at all.
    TimeoutRead startup;
    TimeoutRead tool;
    for (size_t i = 1; i < main_end; ++i) {
        if (sets_toml_key(lines[i], "startup_timeout_sec"))
            startup.note(lines[i]);
        else if (sets_toml_key(lines[i], "tool_timeout_sec"))
            tool.note(lines[i]);
    }

    std::string kept;
    size_t      i = 1;
    while (i < main_end) {
        const std::string& line = lines[i];
        if (sets_toml_key(line, "command") || sets_toml_key(line, "args")) {
            // Ours to write anew: the line goes, on through the real end of its value -- a multi-line
            // array's closing bracket may sit lines later, and a ']' inside a string or comment in it
            // is not that bracket. A value whose end we cannot find, or a multi-line string standing
            // where a command or args belongs, stops the whole update.
            const size_t    equals = line.find('=');
            const TomlValue value  = scan_toml_value(existing_table, line_start[i] + equals + 1);
            if (value.multiline_text || value.end == std::string::npos)
                return std::nullopt;
            while (i < main_end && line_start[i] < value.end)
                ++i;
            continue;
        }
        // A timeout line the user's value already covers, or one we cannot read, is kept as written;
        // one we are raising is dropped (our line goes under the command).
        const bool startup_line = sets_toml_key(line, "startup_timeout_sec");
        const bool tool_line    = !startup_line && sets_toml_key(line, "tool_timeout_sec");
        if (startup_line || tool_line) {
            const TimeoutRead& timeout = startup_line ? startup : tool;
            const int          ours    = startup_line ? STARTUP_TIMEOUT_SEC : TOOL_TIMEOUT_SEC;
            if (timeout.keep_their_line(ours))
                kept += line;
        } else {
            kept += line;
        }
        ++i;
    }

    std::string out = lines[0]; // the header line, kept as it is -- comment and spacing included
    if (out.empty() || out.back() != '\n')
        out += '\n';
    out += toml_command_lines(command);
    if (startup.ours_bigger(STARTUP_TIMEOUT_SEC))
        out += "startup_timeout_sec = " + std::to_string(STARTUP_TIMEOUT_SEC) + "\n";
    if (tool.ours_bigger(TOOL_TIMEOUT_SEC))
        out += "tool_timeout_sec = " + std::to_string(TOOL_TIMEOUT_SEC) + "\n";
    out += kept;
    for (i = main_end; i < lines.size(); ++i)
        out += lines[i];
    return out;
}

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

// The same for a Codex table -- and read like TOML, not like text: only the main table's args value
// says what an entry runs. A comment after it, another key's value and a subtable's own keys never
// make a table ours, so a developer's table whose comment merely mentions the shared path stays the
// developer's.
bool MCPClientConfig::is_our_toml_table(const std::string& table_text)
{
    const size_t body = table_text.find('\n'); // everything but the header line
    size_t       pos  = (body == std::string::npos) ? table_text.size() : body + 1;
    while (pos < table_text.size()) {
        const size_t      newline   = table_text.find('\n', pos);
        const size_t      line_end  = (newline == std::string::npos) ? table_text.size() : newline;
        const std::string line      = table_text.substr(pos, line_end - pos);
        if (is_toml_header(line))
            break; // the main table ends; a subtable's args is not the entry's
        if (sets_toml_key(line, "args")) {
            const size_t    equals = line.find('=');
            const TomlValue value  = scan_toml_value(table_text, pos + equals + 1);
            if (value.end == std::string::npos)
                return false; // an args value that never ends names nothing
            if (names_shared_bridge(table_text.substr(pos + equals + 1, value.end - (pos + equals + 1))))
                return true;
        }
        pos = (newline == std::string::npos) ? table_text.size() : newline + 1;
    }
    return false;
}

// Write one client's orca-slicer entry, whatever it held before. Nothing that cannot be edited
// safely is touched at all: a JSON config that does not parse, a servers value that is not an object,
// a TOML table whose command or args cannot be rewritten, a config that names our server only in a
// TOML form this code would have to guess at, and any file that exists but cannot be read are all
// refused here and stay byte for byte as they are. Every write that happens is atomic and keeps the
// user's key order, so no client ever reads a half-written config or loses the layout of its own.
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
            // survive; a client without one gains a fresh table at the end. A file we cannot read is
            // never written over, and a table we cannot update safely is left for the user.
            std::string before;
            if (!read_existing_file(path, before, error))
                return false;
            const std::string existing = toml_server_table_of(before);
            if (existing.empty() && toml_names_server_elsewhere(before)) {
                error = toml_undefined_form_error(info.config_path, SERVER_NAME, "mcp_servers." + SERVER_NAME);
                return false;
            }
            const std::optional<std::string> updated = updated_toml_server_table(existing, command);
            if (!updated) {
                error = toml_undatable_table_error(info.config_path, SERVER_NAME);
                return false;
            }
            content = existing.empty() ? replace_toml_server_table(before, *updated)
                                       : replace_toml_server_table_span(before, *updated);
        } else {
            nlohmann::ordered_json config;
            if (!read_json_config(info.config_path, config, error))
                return false;
            if (config.contains(info.servers_key) && !config[info.servers_key].is_object()) {
                // The user's own value under the servers key: replacing it would delete their
                // settings, and writing into it would not be JSON. Theirs to fix.
                error = info.config_path + ": \"" + info.servers_key +
                        "\" is not an object; fix it by hand, then press Connect again.";
                return false;
            }
            if (!config.contains(info.servers_key))
                config[info.servers_key] = nlohmann::ordered_json::object();
            const nlohmann::json existing = config[info.servers_key].contains(SERVER_NAME)
                                                ? nlohmann::json(config[info.servers_key][SERVER_NAME])
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
    std::string              error;
    nlohmann::ordered_json   config;
    if (!read_json_config(path, config, error))
        return false;
    return config.contains(servers_key) && config[servers_key].is_object() && config[servers_key].contains(SERVER_NAME);
}

// Check if orca-slicer is configured in a TOML config file: the editable table, however its header
// is spelled, or the server named in a form only the user can edit.
bool MCPClientConfig::is_configured_in_toml(const std::string& path)
{
    if (!config_file_exists(path))
        return false;

    try {
        std::string content;
        std::string read_error;
        if (!read_existing_file(boost::filesystem::path(path), content, read_error))
            return false;
        return !toml_server_table_of(content).empty() || toml_names_server_elsewhere(content);
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

// Read JSON config file. ordered_json all the way through: a client's config is rewritten as the
// user laid it out, keys and all.
bool MCPClientConfig::read_json_config(const std::string& path, nlohmann::ordered_json& config, std::string& error)
{
    try {
        if (!config_file_exists(path)) {
            // Return empty object for new file
            config = nlohmann::ordered_json::object();
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
            config = nlohmann::ordered_json::object();
            return true;
        }

        config = nlohmann::ordered_json::parse(content);
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
bool MCPClientConfig::write_json_config(const std::string& path, const nlohmann::ordered_json& config, std::string& error)
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

    nlohmann::ordered_json config;
    if (!read_json_config(info.config_path, config, error_message)) {
        return false;
    }

    if (config.contains(info.servers_key) && !config[info.servers_key].is_object()) {
        // Not an object is the user's own value, not a servers list: nothing of it may be removed,
        // and nothing is written.
        error_message = info.config_path + ": \"" + info.servers_key +
                        "\" is not an object; fix it by hand, then press Connect again.";
        return false;
    }

    if (!config.contains(info.servers_key) || !config[info.servers_key].contains(SERVER_NAME)) {
        error_message = SERVER_NAME + " not found in config";
        return false;
    }

    config[info.servers_key].erase(SERVER_NAME);

    return write_json_config(info.config_path, config, error_message);
}

// Remove orca-slicer from TOML config (Codex CLI): our table and its subtables, by the very span the
// Connect update edits -- nothing else, whatever lines or servers stand around it. A table not found
// but the server named in a form this code cannot edit is refused like Connect refuses it, the file
// left alone; an old regex here once swallowed a ']' inside a string.
bool MCPClientConfig::remove_from_toml_config(const std::string& path, std::string& error)
{
    try {
        if (!boost::filesystem::exists(path)) {
            error = "Config file does not exist";
            return false;
        }
        std::string content;
        if (!read_existing_file(boost::filesystem::path(path), content, error))
            return false;

        const auto span = toml_table_span(content);
        if (span.first == std::string::npos) {
            if (toml_names_server_elsewhere(content))
                error = toml_undefined_form_error(path, SERVER_NAME, "mcp_servers." + SERVER_NAME);
            else
                error = SERVER_NAME + " not found in config";
            return false;
        }

        std::string new_content = content.substr(0, span.first) + content.substr(span.first + span.second);

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
// would write now, and never when it is not ours or there is none. A config that does not parse, or
// cannot be read, or holds a table whose command and args this code cannot rewrite, fails and stays
// untouched; a server named only in a TOML form this code would have to guess at is NotOurs -- it is
// the user's entry, in their form, and the startup refresh never edits it.
EntryRefresh MCPClientConfig::refresh_client_entry(const MCPClientInfo& info, const BridgeCommand& expected,
                                                   std::string& error)
{
    error.clear();
    try {
        if (!config_file_exists(info.config_path))
            return EntryRefresh::NoEntry;

        if (info.is_toml) {
            std::string content;
            if (!read_existing_file(boost::filesystem::path(info.config_path), content, error))
                return EntryRefresh::Failed;
            const std::string table = toml_server_table_of(content);
            if (table.empty())
                return toml_names_server_elsewhere(content) ? EntryRefresh::NotOurs : EntryRefresh::NoEntry;
            if (!is_our_toml_table(table))
                return EntryRefresh::NotOurs;
            const std::optional<std::string> updated = updated_toml_server_table(table, expected);
            if (!updated) {
                error = toml_undatable_table_error(info.config_path, SERVER_NAME);
                return EntryRefresh::Failed;
            }
            if (*updated == table)
                return EntryRefresh::Current;
            return write_client_entry(info, expected, error) ? EntryRefresh::Rewritten : EntryRefresh::Failed;
        }

        nlohmann::ordered_json config;
        if (!read_json_config(info.config_path, config, error))
            return EntryRefresh::Failed;
        if (!config.contains(info.servers_key) || !config[info.servers_key].is_object() ||
            !config[info.servers_key].contains(SERVER_NAME))
            return EntryRefresh::NoEntry;

        // Read as plain json: whether an entry needs rewriting is never a matter of key order.
        const nlohmann::json entry = config[info.servers_key][SERVER_NAME];
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

// Whether one client's config holds an entry of ours, which the startup refresh may have to bring up
// to date -- decided by reading the config alone, with no interpreter probe, so a machine whose
// configs hold none of ours starts no process at startup. A config that cannot be read or parsed
// counts as one: refresh_client_entry reports its failure as it always did.
bool MCPClientConfig::config_holds_our_entry(const MCPClientInfo& info, std::string& error)
{
    try {
        if (!config_file_exists(info.config_path))
            return false;
        if (info.is_toml) {
            std::string content;
            if (!read_existing_file(boost::filesystem::path(info.config_path), content, error))
                return true; // unreadable: let the refresh report it, rather than hush it
            const std::string table = toml_server_table_of(content);
            return !table.empty() && is_our_toml_table(table);
        }
        nlohmann::ordered_json config;
        if (!read_json_config(info.config_path, config, error))
            return true; // unparsable: the same
        if (!config.contains(info.servers_key) || !config[info.servers_key].is_object() ||
            !config[info.servers_key].contains(SERVER_NAME))
            return false;
        return is_our_entry(config[info.servers_key][SERVER_NAME]);
    } catch (const std::exception&) {
        return true; // a surprise here costs one probe, not a silently skipped repair
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

    // An outdated entry is the only reason to ask which interpreter a rewrite would name, and asking
    // can cost a process: read every config first, and probe only when one holds an entry of ours.
    std::vector<MCPClientInfo> candidates;
    for (const MCPClientInfo& info : get_all_clients()) {
        std::string error;
        if (config_holds_our_entry(info, error))
            candidates.push_back(info);
    }
    if (candidates.empty())
        return;

    const InterpreterChoice interpreter = bridge_interpreter();
    if (!interpreter.error.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "MCP client entries: left as they are: " << interpreter.error;
        return;
    }

    const BridgeCommand command = bridge_command(interpreter.path, get_bridge_script_path());
    for (const MCPClientInfo& info : candidates) {
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
