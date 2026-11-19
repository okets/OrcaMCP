#include "slic3r/GUI/OrcaMCP/MCPClientConfig.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPCodexToml.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/fstream.hpp>

#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "test_utils.hpp"

// Where the user's own agent sessions get their bridge: ~/.orcamcp, refreshed at startup from the app
// that runs (MCPClientConfig::refresh_shared_bridge_at_startup). Every dev-build launch -- every agent's
// live check -- used to replace it by file date, and the installed app, older by date, never put its own
// back. A launch with its own data folder, or of a build in a source tree, now leaves it alone, and any
// other launch -- the installed app's, an agent's included -- makes it match its app, by content.

using namespace Slic3r::GUI;

namespace {

void write(const boost::filesystem::path& path, const std::string& content)
{
    boost::nowide::ofstream out(path.string(), std::ios::binary);
    out << content;
}

std::string read_file(const boost::filesystem::path& path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    std::ostringstream      content;
    content << in.rdbuf();
    return content.str();
}

// A JSON client's metadata, built by hand with its config in the test's own folder.
MCPClientInfo json_client(const boost::filesystem::path& folder, const std::string& file)
{
    MCPClientInfo info{};
    info.id                   = MCPClientId::ClaudeDesktop;
    info.name                 = "Test client";
    info.config_path          = (folder / file).string();
    info.servers_key          = "mcpServers";
    info.is_toml              = false;
    info.requires_type_field  = false;
    info.requires_tools_field = false;
    return info;
}

// Codex's metadata, whose config is TOML.
MCPClientInfo codex_client(const boost::filesystem::path& folder, const std::string& file)
{
    MCPClientInfo info       = json_client(folder, file);
    info.id                  = MCPClientId::CodexCLI;
    info.name                = "Codex CLI";
    info.servers_key         = "mcp_servers";
    info.is_toml             = true;
    return info;
}

size_t count_of(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// The UTF-8 BOM some Windows editors put at a file's very first bytes.
const std::string& utf8_bom()
{
    static const std::string bom = std::string() + static_cast<char>(0xEF) + static_cast<char>(0xBB) + static_cast<char>(0xBF);
    return bom;
}

// The lines of a Codex table's MAIN table -- up to its first subtable header -- that set `key`,
// bare or quoted. A Connect update must leave exactly one command line and one args line: a second
// of either makes Codex reject the user's whole config.
size_t count_toml_key_lines(const std::string& table, const std::string& key)
{
    size_t n         = 0;
    const size_t body = table.find('\n'); // start below the table's own header line
    size_t pos       = (body == std::string::npos) ? table.size() : body + 1;
    while (pos < table.size()) {
        const size_t      newline = table.find('\n', pos);
        const size_t      end     = (newline == std::string::npos) ? table.size() : newline;
        const std::string line    = table.substr(pos, end - pos);
        size_t            i       = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i < line.size() && line[i] == '[')
            break; // the main table ends where a subtable header begins
        bool sets_key = false;
        if (i < line.size() && (line[i] == '"' || line[i] == '\'')) {
            const char   quote = line[i];
            const size_t close = line.find(quote, i + 1);
            sets_key           = close != std::string::npos && line.compare(i + 1, close - i - 1, key) == 0;
            if (sets_key)
                i = close + 1;
        } else if (line.compare(i, key.size(), key) == 0) {
            sets_key = true;
            i += key.size();
        }
        if (sets_key) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                ++i;
            if (i < line.size() && line[i] == '=')
                ++n;
        }
        pos = (newline == std::string::npos) ? table.size() : newline + 1;
    }
    return n;
}

// The keys of a parsed JSON object, in the order the file wrote them.
std::vector<std::string> keys_in_order(const nlohmann::ordered_json& object)
{
    std::vector<std::string> keys;
    for (auto it = object.begin(); it != object.end(); ++it)
        keys.push_back(it.key());
    return keys;
}

} // namespace

TEST_CASE("a launch on a data folder of its own never touches the user's bridge", "[McpClientConfig]")
{
    // Every test launch has one (--datadir).
    for (const bool differs : {false, true}) {
        const auto decision = MCPClientConfig::bridge_copy_decision(true, false, differs);
        CHECK_FALSE(decision.copy);
        CHECK(decision.reason.find("--datadir") != std::string::npos);
    }
}

TEST_CASE("a build in a source tree never touches the user's bridge", "[McpClientConfig]")
{
    for (const bool differs : {false, true}) {
        const auto decision = MCPClientConfig::bridge_copy_decision(false, true, differs);
        CHECK_FALSE(decision.copy);
        CHECK(decision.reason.find("build") != std::string::npos);
    }
}

TEST_CASE("the installed app copies its bridge when the content differs, launched by an agent or not", "[McpClientConfig]")
{
    // start_orca launches only the installed app, as an agent's launch: after an update made that way,
    // skipping agent launches left ~/.orcamcp on the old bridge.
    CHECK_FALSE(MCPClientConfig::bridge_copy_decision(false, false, false).copy);
    CHECK(MCPClientConfig::bridge_copy_decision(false, false, true).copy);
}

TEST_CASE("a program under a CMake build folder is a build in a source tree", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-build");
    const auto build   = dir.path() / "build" / "arm64";
    const auto program = build / "src" / "RelWithDebInfo" / "OrcaSlicer.app" / "Contents" / "MacOS" / "OrcaSlicer";
    boost::filesystem::create_directories(program.parent_path());
    write(program, "");
    CHECK_FALSE(MCPClientConfig::executable_in_build_tree(program.string()));
    write(build / "CMakeCache.txt", "");
    CHECK(MCPClientConfig::executable_in_build_tree(program.string()));
    CHECK_FALSE(MCPClientConfig::executable_in_build_tree("/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer"));
}

TEST_CASE("files differ by content, whatever their dates, and a missing copy differs", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-bridge");
    const auto original = dir.path() / "orcamcp-bridge.py";
    const auto copy     = dir.path() / "copy.py";
    write(original, "print('installed')\n");

    CHECK(MCPClientConfig::file_content_differs(original.string(), copy.string())); // missing

    write(copy, "print('installed')\n");
    boost::filesystem::last_write_time(copy, boost::filesystem::last_write_time(original) + 3600); // newer by date
    CHECK_FALSE(MCPClientConfig::file_content_differs(original.string(), copy.string()));

    write(copy, "print('dev build')\n");
    boost::filesystem::last_write_time(copy, boost::filesystem::last_write_time(original) + 3600); // newer by date
    CHECK(MCPClientConfig::file_content_differs(original.string(), copy.string()));
}

// What Connect writes into a client's config, decided apart from the app: the entry that runs the
// bridge with a given interpreter, Codex's TOML table and its replacement, and which entries are ours
// (docs/superpowers/specs/2026-11-18-connect-without-python-design.md).

TEST_CASE("an entry runs the bridge with the given interpreter, isolated", "[McpClientConfig]")
{
    const MCPClientConfig::BridgeCommand command =
        MCPClientConfig::bridge_command("/A/python3", "/h/.orcamcp/orcamcp-bridge.py");

    // Claude Code needs "type": "stdio".
    const auto claude = MCPClientConfig::server_entry(MCPClientConfig::get_client_info(MCPClientId::ClaudeCode), command);
    CHECK(claude == nlohmann::json::parse(
              R"({"command":"/A/python3","args":["-I","/h/.orcamcp/orcamcp-bridge.py"],"type":"stdio"})"));

    // GitHub Copilot needs "tools": ["*"] and no type.
    const auto copilot =
        MCPClientConfig::server_entry(MCPClientConfig::get_client_info(MCPClientId::GitHubCopilot), command);
    CHECK(copilot == nlohmann::json::parse(
               R"({"command":"/A/python3","args":["-I","/h/.orcamcp/orcamcp-bridge.py"],"tools":["*"]})"));
    CHECK_FALSE(copilot.contains("type"));
}

TEST_CASE("Codex's table gives the bridge time to start and to answer", "[McpClientConfig]")
{
    // A Windows install's Python, a profile folder whose user is O'Brien: backslashes doubled, the
    // apostrophe left as it is.
    const MCPClientConfig::BridgeCommand command{R"(C:\Program Files\OrcaMCP\python\python.exe)",
                                                 {"-I", R"(C:\Users\O'Brien\.orcamcp\orcamcp-bridge.py)"}};
    const std::string expected =
        "[mcp_servers.orca-slicer]\n"
        "command = \"C:\\\\Program Files\\\\OrcaMCP\\\\python\\\\python.exe\"\n"
        "args = [\"-I\", \"C:\\\\Users\\\\O'Brien\\\\.orcamcp\\\\orcamcp-bridge.py\"]\n"
        "startup_timeout_sec = 20\n"
        "tool_timeout_sec = 140\n"; // over the bridge's own 120 s
    CHECK(MCPClientConfig::toml_server_table(command) == expected);
}

TEST_CASE("a quote in a path is escaped", "[McpClientConfig]")
{
    CHECK(MCPClientConfig::toml_basic_string("a\"b") == "\"a\\\"b\"");
}

TEST_CASE("replacing Codex's table keeps everything else", "[McpClientConfig]")
{
    const std::string ours =
        MCPClientConfig::toml_server_table(MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py"));
    const std::string content =
        "# my Codex config\n"
        "\n"
        "[mcp_servers.other]\n"
        "command = \"left\"\n"
        "\n"
        "[mcp_servers.orca-slicer]\n"
        "command = \"python3\"\n"
        "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n"
        "\n"
        "[mcp_servers.orca-slicer.env]\n"
        "ORCAMCP_TIMEOUT = \"120\"\n"
        "\n"
        "[mcp_servers.another]\n" // a table is defined once: Codex rejects a second [mcp_servers.other]
        "command = \"right\"\n";

    // Our table is its header, its keys and its subtable, up to the next other header.
    CHECK(MCPClientConfig::toml_server_table_of(content) ==
          "[mcp_servers.orca-slicer]\n"
          "command = \"python3\"\n"
          "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n"
          "\n"
          "[mcp_servers.orca-slicer.env]\n"
          "ORCAMCP_TIMEOUT = \"120\"\n"
          "\n");

    const std::string replaced = MCPClientConfig::replace_toml_server_table(content, ours);
    CHECK(boost::algorithm::starts_with(replaced, "# my Codex config\n"));
    CHECK(replaced.find("[mcp_servers.other]\ncommand = \"left\"\n") != std::string::npos);
    CHECK(replaced.find("[mcp_servers.another]\ncommand = \"right\"\n") != std::string::npos);
    CHECK(replaced.find("orca-slicer.env") == std::string::npos);
    CHECK(boost::algorithm::ends_with(replaced, ours));

    // The quoted header form is our table too.
    const std::string quoted =
        "[mcp_servers.\"orca-slicer\"]\n"
        "command = \"python3\"\n"
        "\n"
        "[mcp_servers.keep]\n"
        "x = 1\n";
    const std::string replaced_quoted = MCPClientConfig::replace_toml_server_table(quoted, ours);
    CHECK(replaced_quoted.find("python3") == std::string::npos);
    CHECK(replaced_quoted.find("[mcp_servers.keep]\nx = 1\n") != std::string::npos);
    CHECK(boost::algorithm::ends_with(replaced_quoted, ours));

    // A content without our table is kept and gains the new one after a blank line.
    const std::string without = "[mcp_servers.other]\ncommand = \"left\"\n";
    CHECK(MCPClientConfig::replace_toml_server_table(without, ours) == without + "\n" + ours);
}

TEST_CASE("Codex's table is found however its header is spelled", "[McpClientConfig]")
{
    const std::string ours =
        MCPClientConfig::toml_server_table(MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py"));

    // A comment behind the closing bracket is not part of the header.
    const std::string commented =
        "[mcp_servers.orca-slicer]  # added by OrcaMCP\n"
        "command = \"python3\"\n"
        "[mcp_servers.keep]\n"
        "x = 1\n";
    CHECK(MCPClientConfig::toml_server_table_of(commented) ==
          "[mcp_servers.orca-slicer]  # added by OrcaMCP\n"
          "command = \"python3\"\n");
    // Replacing must leave exactly one [mcp_servers.orca-slicer]: a table defined twice makes Codex
    // reject the user's whole config.
    const std::string replaced = MCPClientConfig::replace_toml_server_table(commented, ours);
    CHECK(count_of(replaced, "orca-slicer]") == 1);
    CHECK(boost::algorithm::ends_with(replaced, ours));

    // Spaces around the keys and dots inside the brackets.
    const std::string spaced =
        "[ mcp_servers . orca-slicer ]\n"
        "command = \"python3\"\n";
    CHECK(MCPClientConfig::toml_server_table_of(spaced) == spaced);

    // An array of tables is not our table.
    CHECK(MCPClientConfig::toml_server_table_of("[[mcp_servers.orca-slicer]]\ncommand = \"python3\"\n").empty());
}

TEST_CASE("an entry is ours only when it runs the shared bridge", "[McpClientConfig]")
{
    // The old python3 form, and the isolated form with a Windows path.
    CHECK(MCPClientConfig::is_our_entry(nlohmann::json::parse(R"({"command":"python3","args":["/Users/x/.orcamcp/orcamcp-bridge.py"]})")));
    CHECK(MCPClientConfig::is_our_entry(nlohmann::json::parse(
        R"({"command":"C:\\Program Files\\OrcaMCP\\python\\python.exe","args":["-I","C:\\Users\\x\\.orcamcp\\orcamcp-bridge.py"]})")));

    // A repository script is the developer's own; an entry with no args runs no bridge of ours.
    CHECK_FALSE(MCPClientConfig::is_our_entry(nlohmann::json::parse(R"({"command":"python3","args":["./scripts/orcamcp-bridge.py"]})")));
    CHECK_FALSE(MCPClientConfig::is_our_entry(nlohmann::json::parse(R"({"command":"python3"})")));

    // TOML: the same, with a path spelled with '/' or with escaped '\'.
    CHECK(MCPClientConfig::is_our_toml_table("[mcp_servers.orca-slicer]\ncommand = \"python3\"\nargs = [\"C:/Users/x/.orcamcp/orcamcp-bridge.py\"]\n"));
    CHECK(MCPClientConfig::is_our_toml_table(R"([mcp_servers.orca-slicer]
command = "python3"
args = ["C:\\Users\\x\\.orcamcp\\orcamcp-bridge.py"]
)"));
    CHECK_FALSE(MCPClientConfig::is_our_toml_table(""));
}

// Which interpreter an entry runs the bridge with, and what Connect writes with it
// (docs/superpowers/plans/2026-11-18-connect-without-python.md, Task 3): no user installs Python.

namespace {

// The refusal an AppImage gets when no system Python can run the bridge -- the user's words, quoted by
// the docs. (What an ordinary install says is tested by its own case, naming the install it is.)
const char* const NO_PYTHON = "Connecting needs Python 3.9 or newer, which this AppImage does not include: "
                              "install your distribution's python3 package, then press Connect again.";

} // namespace

TEST_CASE("the app's own Python is chosen wherever its path stays put", "[McpClientConfig]")
{
    // A system Python is offered, and the app's own still wins: it ships with the app and needs no
    // install. (The AppImage is the one place its path does not stay put.)
    MCPClientConfig::InterpreterInputs inputs;
    inputs.bundled       = "/app/python3";
    inputs.in_appimage   = false;
    inputs.system_python = "/usr/bin/python3";
    inputs.system_major  = 3;
    inputs.system_minor  = 12;

    const auto choice = MCPClientConfig::choose_interpreter(inputs);
    CHECK(choice.path == "/app/python3");
    CHECK(choice.error.empty());
}

TEST_CASE("inside an AppImage the system's Python 3.9 or newer is chosen", "[McpClientConfig]")
{
    // The AppImage mounts its own runtime at a path that changes every launch, so its entries name the
    // system's python3 -- when it is new enough for the bridge.
    MCPClientConfig::InterpreterInputs inputs;
    inputs.bundled       = "/app/python3";
    inputs.in_appimage   = true;
    inputs.system_python = "/usr/bin/python3";
    inputs.system_major  = 3;
    inputs.system_minor  = 9; // the version the bridge holds itself to

    const auto taken = MCPClientConfig::choose_interpreter(inputs);
    CHECK(taken.path == "/usr/bin/python3");
    CHECK(taken.error.empty());

    inputs.system_minor = 8;
    const auto old = MCPClientConfig::choose_interpreter(inputs);
    CHECK(old.path.empty());
    CHECK(old.error == NO_PYTHON);

    // No system Python at all: the same words.
    inputs.system_python.clear();
    inputs.system_minor = 12;
    const auto none = MCPClientConfig::choose_interpreter(inputs);
    CHECK(none.path.empty());
    CHECK(none.error == NO_PYTHON);

    // No bundled runtime outside an AppImage: the system's own is taken.
    MCPClientConfig::InterpreterInputs installed;
    installed.system_python = "/usr/bin/python3";
    installed.system_major  = 3;
    installed.system_minor  = 12;
    const auto from_system = MCPClientConfig::choose_interpreter(installed);
    CHECK(from_system.path == "/usr/bin/python3");
    CHECK(from_system.error.empty());
}

TEST_CASE("an install without Python says so without naming the AppImage", "[McpClientConfig]")
{
    // A Windows or macOS install whose bundled runtime is missing: the fix is reinstalling OrcaMCP or
    // installing Python, and the message must not speak of an AppImage the user has never seen.
    const auto choice = MCPClientConfig::choose_interpreter(MCPClientConfig::InterpreterInputs{});
    CHECK(choice.path.empty());
    CHECK(choice.error == "Connecting needs Python 3.9 or newer, which this OrcaMCP install does not include: "
                          "reinstall OrcaMCP, or install Python 3.9 or newer, then press Connect again.");
}

TEST_CASE("Connect replaces Codex's old python3 table", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-codex-write");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    const std::string   path  = codex.config_path;
    write(path, "# my Codex config\n"
                "\n"
                "[mcp_servers.other]\n"
                "command = \"keep\"\n"
                "\n"
                "[mcp_servers.orca-slicer]\n"
                "command = \"python3\"\n"
                "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n"
                "startup_timeout_sec = 5\n");

    const auto command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    CHECK(error.empty());

    // The comment and the other server survive; our table is exactly what Connect builds now, and the
    // bare "python3" the old one ran is gone.
    const std::string content = read_file(path);
    CHECK(boost::algorithm::starts_with(content, "# my Codex config\n"));
    CHECK(content.find("[mcp_servers.other]\ncommand = \"keep\"") != std::string::npos);
    CHECK(MCPClientConfig::toml_server_table_of(content) == MCPClientConfig::toml_server_table(command));
    CHECK(content.find("python3") == std::string::npos);
}

TEST_CASE("Connect never rewrites a config it cannot parse", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-unparsable");
    const MCPClientInfo client = json_client(dir.path(), "config.json");
    const std::string   broken = "{ not json";
    write(client.config_path, broken);

    const auto command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    CHECK_FALSE(MCPClientConfig::write_client_entry(client, command, error));
    CHECK_FALSE(error.empty());
    CHECK(read_file(client.config_path) == broken); // byte for byte
}

// An entry is updated, never replaced: the docs tell users to put bridge options in the entry's env
// (ORCAMCP_TIMEOUT, ORCAMCP_PORT) and Codex users to raise its timeouts, and the first startup refresh
// of the new version must not silently delete what they set.

TEST_CASE("Connect keeps an entry's own settings", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-keep-env");
    const MCPClientInfo claude = json_client(dir.path(), "claude_desktop_config.json");
    write(claude.config_path, R"({"mcpServers": {"orca-slicer": {"command": "python3",)"
                              R"("args": ["/h/.orcamcp/orcamcp-bridge.py"],)"
                              R"("env": {"ORCAMCP_TIMEOUT": "300"}}}})");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(claude, command, error));

    const auto entry = nlohmann::json::parse(read_file(claude.config_path))["mcpServers"]["orca-slicer"];
    CHECK(entry["command"] == "/opt/orcamcp/python");
    CHECK(entry["args"] == nlohmann::json::array({"-I", "/h/.orcamcp/orcamcp-bridge.py"}));
    CHECK(entry["env"] == nlohmann::json{{"ORCAMCP_TIMEOUT", "300"}}); // the user's setting survives

    // What Connect just wrote is current: the next startup writes nothing.
    CHECK(MCPClientConfig::refresh_client_entry(claude, command, error) == EntryRefresh::Current);
}

TEST_CASE("Codex keeps its own settings and longer timeouts", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-keep-codex");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "command = \"python3\"\n"
                             "args = [\n"
                             "  \"/h/.orcamcp/orcamcp-bridge.py\",\n"
                             "]\n"
                             "startup_timeout_sec = 5\n"
                             "tool_timeout_sec = 600\n"
                             "enabled = true\n"
                             "\n"
                             "[mcp_servers.orca-slicer.env]\n"
                             "ORCAMCP_TIMEOUT = \"300\"\n"
                             "\n"
                             "[mcp_servers.other]\n"
                             "command = \"keep\"\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string content = read_file(codex.config_path);

    // Our command and args; the timeouts grow to what the bridge needs but never shrink what the user
    // asked for (the 5 becomes 20, the raised 600 stays); the user's own key and env subtable survive;
    // the multi-line args array is gone whole, not half-removed.
    CHECK(content.find("command = \"/opt/orcamcp/python\"") != std::string::npos);
    CHECK(content.find("args = [\"-I\", \"/h/.orcamcp/orcamcp-bridge.py\"]") != std::string::npos);
    CHECK(content.find("startup_timeout_sec = 20\n") != std::string::npos);
    CHECK(content.find("tool_timeout_sec = 600\n") != std::string::npos);
    CHECK(content.find("enabled = true\n") != std::string::npos);
    CHECK(content.find("[mcp_servers.orca-slicer.env]\nORCAMCP_TIMEOUT = \"300\"\n") != std::string::npos);
    CHECK(content.find("python3") == std::string::npos);
    CHECK(count_of(content, "args") == 1);

    // Exactly one command line and one args line survive in our main table, not two of either.
    const std::string ours = MCPClientConfig::toml_server_table_of(content);
    CHECK(count_toml_key_lines(ours, "command") == 1);
    CHECK(count_toml_key_lines(ours, "args") == 1);

    // The table keeps its position: ours, with its env, still stands before [mcp_servers.other].
    CHECK(boost::algorithm::starts_with(content, "[mcp_servers.orca-slicer]"));
    CHECK(content.find("[mcp_servers.orca-slicer.env]") < content.find("[mcp_servers.other]\ncommand = \"keep\"\n"));
    CHECK(content.find("[mcp_servers.other]\ncommand = \"keep\"\n") != std::string::npos);

    // Updated, and now current: the startup refresh stops here.
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
}

TEST_CASE("the startup refresh rewrites only entries that are ours", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-refresh");
    const auto command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;

    // A user's Claude Desktop config holding the old python3 entry of ours.
    const MCPClientInfo claude = json_client(dir.path(), "claude_desktop_config.json");
    write(claude.config_path, R"({"theme": "dark", "mcpServers": {)"
                              R"("keep": {"command": "x"}, )"
                              R"("orca-slicer": {"command": "python3", "args": ["/h/.orcamcp/orcamcp-bridge.py"]}}})");
    CHECK(MCPClientConfig::refresh_client_entry(claude, command, error) == EntryRefresh::Rewritten);

    const auto after = nlohmann::json::parse(read_file(claude.config_path));
    CHECK(after["theme"] == "dark"); // the user's own keys and servers stay
    CHECK(after["mcpServers"]["keep"] == nlohmann::json{{"command", "x"}});
    CHECK(after["mcpServers"]["orca-slicer"] == MCPClientConfig::server_entry(claude, command));

    // The entry is now exactly what Connect would write: a second launch writes nothing at all.
    const std::string rewritten = read_file(claude.config_path);
    CHECK(MCPClientConfig::refresh_client_entry(claude, command, error) == EntryRefresh::Current);
    CHECK(read_file(claude.config_path) == rewritten);

    // A developer's entry, running the repository's own script, is theirs: never touched.
    const MCPClientInfo dev  = json_client(dir.path(), "dev.json");
    const std::string   own  = R"({"mcpServers": {"orca-slicer": {"command": "python3", "args": ["./scripts/orcamcp-bridge.py"]}}})";
    write(dev.config_path, own);
    CHECK(MCPClientConfig::refresh_client_entry(dev, command, error) == EntryRefresh::NotOurs);
    CHECK(read_file(dev.config_path) == own);

    // No config: NoEntry, and none is created -- the user may not use this client at all.
    const MCPClientInfo fresh = json_client(dir.path(), "fresh.json");
    CHECK(MCPClientConfig::refresh_client_entry(fresh, command, error) == EntryRefresh::NoEntry);
    CHECK_FALSE(boost::filesystem::exists(fresh.config_path));

    // Codex's old table is ours and outdated: replaced, as at Connect.
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "command = \"python3\"\n"
                             "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n"
                             "startup_timeout_sec = 5\n");
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Rewritten);
    CHECK(MCPClientConfig::toml_server_table_of(read_file(codex.config_path)) == MCPClientConfig::toml_server_table(command));

    // A config that does not parse is reported and left as it is.
    const MCPClientInfo broken = json_client(dir.path(), "broken.json");
    write(broken.config_path, "{ not json");
    CHECK(MCPClientConfig::refresh_client_entry(broken, command, error) == EntryRefresh::Failed);
    CHECK(read_file(broken.config_path) == "{ not json");
}

// What an acceptance review of Connect found on inputs the first version guessed at (design items
// 3-7, docs/superpowers/specs/2026-11-18-connect-without-python-design.md): a timeout line read by
// TOML's rules, an unreadable file never written over, every spelling of our Codex table found and
// the un-editable ones refused, updates that cannot be done safely refused whole, Disconnect using
// the same span as Connect, ownership read from values not comments, and JSON configs that keep the
// order the user wrote them in.

TEST_CASE("a longer Codex timeout with a comment is kept", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-timeout-comment");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "command = \"python3\"\n"
                             "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n"
                             "tool_timeout_sec = 600 # slow machine\n"
                             "startup_timeout_sec = 60 # slow disk\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string content = read_file(codex.config_path);

    // Both values already cover what the bridge needs, so both lines stay exactly as the user wrote
    // them -- comment included -- and no line of ours joins either key.
    CHECK(content.find("tool_timeout_sec = 600 # slow machine\n") != std::string::npos);
    CHECK(content.find("startup_timeout_sec = 60 # slow disk\n") != std::string::npos);
    const std::string ours = MCPClientConfig::toml_server_table_of(content);
    CHECK(count_toml_key_lines(ours, "tool_timeout_sec") == 1);
    CHECK(count_toml_key_lines(ours, "startup_timeout_sec") == 1);
    CHECK(count_toml_key_lines(ours, "command") == 1);
    CHECK(count_toml_key_lines(ours, "args") == 1);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
}

TEST_CASE("1_000 in a Codex timeout counts as 1000", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-timeout-underscore");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "command = \"python3\"\n"
                             "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n"
                             "tool_timeout_sec = 1_000\n"
                             "startup_timeout_sec = 1_5\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string content = read_file(codex.config_path);

    // 1_000 reads as a thousand -- at least ours, so the line stands untouched. 1_5 reads as fifteen
    // -- under our twenty, so it is raised, not stopped at the underscore and not refused as
    // unreadable.
    CHECK(content.find("tool_timeout_sec = 1_000\n") != std::string::npos);
    CHECK(content.find("startup_timeout_sec = 1_5") == std::string::npos);
    CHECK(content.find("startup_timeout_sec = 20\n") != std::string::npos);
    const std::string ours = MCPClientConfig::toml_server_table_of(content);
    CHECK(count_toml_key_lines(ours, "tool_timeout_sec") == 1);
    CHECK(count_toml_key_lines(ours, "startup_timeout_sec") == 1);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
}

TEST_CASE("600.0 is kept as written", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-timeout-unreadable");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "command = \"python3\"\n"
                             "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n"
                             "tool_timeout_sec = 600.0\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string content = read_file(codex.config_path);

    // Not a plain integer, so not ours to read: the user's line survives byte for byte and the 140
    // line is not added for that key. The absent startup key still gets our minimum.
    CHECK(content.find("tool_timeout_sec = 600.0\n") != std::string::npos);
    CHECK(content.find("tool_timeout_sec = 140") == std::string::npos);
    CHECK(content.find("startup_timeout_sec = 20\n") != std::string::npos);
    const std::string ours = MCPClientConfig::toml_server_table_of(content);
    CHECK(count_toml_key_lines(ours, "tool_timeout_sec") == 1);
    CHECK(count_toml_key_lines(ours, "startup_timeout_sec") == 1);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
}

TEST_CASE("Codex's table is found however its key is quoted", "[McpClientConfig]")
{
    const auto command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");

    // Basic quotes, literal quotes and spaces around the dotted key all name the same table, which
    // Connect replaces in place -- header kept as written, exactly one table after.
    for (const char* header : {"[mcp_servers.'orca-slicer']", "[mcp_servers.\"orca-slicer\"]", "[ mcp_servers . orca-slicer ]"}) {
        ScopedTemporaryDir dir("orcamcp-quoted-key");
        const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
        write(codex.config_path, std::string(header) + "\n"
                                 "command = \"python3\"\n"
                                 "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n");
        std::string error;
        INFO(header);
        REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
        const std::string content = read_file(codex.config_path);
        CHECK(count_of(content, "orca-slicer") == 1); // replaced in place: no second table appeared
        CHECK(content.find("python3") == std::string::npos);
        CHECK(content.find("command = \"/opt/orcamcp/python\"") != std::string::npos);
        const std::string ours = MCPClientConfig::toml_server_table_of(content);
        CHECK(boost::algorithm::starts_with(ours, header));
        CHECK(count_toml_key_lines(ours, "command") == 1);
        CHECK(count_toml_key_lines(ours, "args") == 1);
        CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
    }

    // A UTF-8 BOM before a quoted header: the table is still found, and the file's first bytes stay.
    ScopedTemporaryDir dir("orcamcp-bom");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    const std::string   bom   = utf8_bom();
    write(codex.config_path, bom + "[mcp_servers.\"orca-slicer\"]\n"
                                   "command = \"python3\"\n"
                                   "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string content = read_file(codex.config_path);
    CHECK(boost::algorithm::starts_with(content, bom)); // the BOM is not part of the edited span
    CHECK(count_of(content, "orca-slicer") == 1);
    CHECK(content.find("command = \"/opt/orcamcp/python\"") != std::string::npos);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
}

TEST_CASE("Connect refuses an inline-table or dotted-key entry", "[McpClientConfig]")
{
    const auto command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");

    // Every form that defines our server without a [mcp_servers.orca-slicer] table: TOML this code will
    // not rewrite. Appending a table to any of them would define the server twice, which Codex rejects.
    for (const char* before : {"[mcp_servers]\nother = { command = \"keep\" }\norca-slicer = { command = \"python3\" }\n",
                               "mcp_servers.orca-slicer.command = \"python3\"\n"
                               "mcp_servers.orca-slicer.args = [\"-I\", \"/h/.orcamcp/orcamcp-bridge.py\"]\n",
                               "mcp_servers.\"orca-slicer\".command = \"python3\"\n",
                               "[mcp_servers]\norca-slicer.command = \"python3\"\n",
                               "mcp_servers = { orca-slicer = { command = \"python3\" } }\n",
                               "[[mcp_servers.orca-slicer]]\ncommand = \"python3\"\n"}) {
        ScopedTemporaryDir dir("orcamcp-undefined-form");
        const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
        write(codex.config_path, before);
        INFO(before);
        CHECK(MCPClientConfig::toml_names_server_elsewhere(before));

        std::string error;
        CHECK_FALSE(MCPClientConfig::write_client_entry(codex, command, error));
        CHECK(error.find("cannot edit") != std::string::npos);
        CHECK(read_file(codex.config_path) == before); // byte for byte: refused means untouched
        CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::NotOurs);
    }

    // A config that names no server of ours stays a config Connect may append to -- and the editable
    // table form, comments and quoted strings aside, is not "elsewhere".
    CHECK_FALSE(MCPClientConfig::toml_names_server_elsewhere("[mcp_servers]\nother = { command = \"x\" }\n"));
    CHECK_FALSE(MCPClientConfig::toml_names_server_elsewhere("[mcp_servers.orca-slicer]\ncommand = \"x\"\n"));
    CHECK_FALSE(MCPClientConfig::toml_names_server_elsewhere("# orca-slicer = { command = \"x\" }\n"
                                                             "note = \"orca-slicer = 1\" # not mcp_servers.orca-slicer.x\n"
                                                             "other = true\n"));
}

TEST_CASE("a multi-line args with a bracket in a path or comment is replaced whole", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-multiline-args");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "command = \"python3\"\n"
                             "args = [ # the bridge [here]\n"
                             "  \"/h/x]y/.orcamcp/orcamcp-bridge.py\",\n"
                             "]\n"
                             "enabled = true\n"
                             "\n"
                             "[mcp_servers.orca-slicer.env]\n"
                             "BRIDGE = \"/h/.orcamcp/orcamcp-bridge.py\"\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string content = read_file(codex.config_path);

    // The ']' inside the comment and the one inside the path did not close the array: it was removed
    // through its real closing bracket, whole, and the one line Connect writes took its place.
    CHECK(content.find("args = [\"-I\", \"/h/.orcamcp/orcamcp-bridge.py\"]\n") != std::string::npos);
    CHECK(content.find("[here]") == std::string::npos);
    CHECK(content.find("x]y") == std::string::npos);
    CHECK(content.find("enabled = true\n") != std::string::npos);
    CHECK(content.find("[mcp_servers.orca-slicer.env]\nBRIDGE = \"/h/.orcamcp/orcamcp-bridge.py\"\n") != std::string::npos);
    const std::string ours = MCPClientConfig::toml_server_table_of(content);
    CHECK(count_toml_key_lines(ours, "command") == 1);
    CHECK(count_toml_key_lines(ours, "args") == 1);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
}

TEST_CASE("an escaped quote in a path does not end the string early", "[McpClientConfig]")
{
    // A basic string's \" is part of the string: read as its end, the ']' after it would close the
    // array early and leave the rest of the path behind as text TOML cannot parse.
    ScopedTemporaryDir dir("orcamcp-escaped-quote");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "command = \"python3\"\n"
                             "args = [\n"
                             "  \"/h/we\\\"ird]/.orcamcp/orcamcp-bridge.py\",\n"
                             "]\n"
                             "enabled = true\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string content = read_file(codex.config_path);
    CHECK(content.find("ird]") == std::string::npos);
    CHECK(content.find("\n]\n") == std::string::npos); // no stray closing bracket of the old array
    CHECK(content.find("enabled = true\n") != std::string::npos);
    const std::string ours = MCPClientConfig::toml_server_table_of(content);
    CHECK(count_toml_key_lines(ours, "args") == 1);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
}

TEST_CASE("a quoted command key is replaced, not duplicated", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-quoted-keys");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "\"command\" = \"python3\"\n"
                             "'args' = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string content = read_file(codex.config_path);

    // Both quoted lines were recognized as the keys they spell and replaced, leaving one line each.
    CHECK(content.find("\"command\"") == std::string::npos);
    CHECK(content.find("'args'") == std::string::npos);
    const std::string ours = MCPClientConfig::toml_server_table_of(content);
    CHECK(count_toml_key_lines(ours, "command") == 1);
    CHECK(count_toml_key_lines(ours, "args") == 1);
    CHECK(content.find("command = \"/opt/orcamcp/python\"") != std::string::npos);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
}

TEST_CASE("a multi-line string command is replaced whole", "[McpClientConfig]")
{
    // The parser knows where the string ends, so the statement goes whole, its lines included.
    ScopedTemporaryDir dir("orcamcp-multiline-command");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "command = \"\"\"\n"
                             "python3\n"
                             "\"\"\"\n"
                             "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n"
                             "startup_timeout_sec = 5\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    CHECK(read_file(codex.config_path) == MCPClientConfig::toml_server_table(command));
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);

    // Args written as a string, multi-line or not, is no list of arguments: Codex runs nothing from it,
    // so the table names no bridge of ours, and the startup refresh leaves it alone.
    const std::string not_a_list = "[mcp_servers.orca-slicer]\n"
                                   "command = \"python3\"\n"
                                   "args = \"\"\"\n/h/.orcamcp/orcamcp-bridge.py\n\"\"\"\n";
    write(codex.config_path, not_a_list);
    CHECK_FALSE(MCPClientConfig::is_our_toml_table(not_a_list));
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::NotOurs);
    CHECK(read_file(codex.config_path) == not_a_list);
}

TEST_CASE("Disconnect removes Codex's table and nothing else", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-codex-disconnect");
    const auto         path = dir.path() / "config.toml";
    write(path, "[mcp_servers.other]\n"
                "command = \"keep left\"\n"
                "\n"
                "[mcp_servers.orca-slicer]\n"
                "command = \"/opt/orcamcp/python\"\n"
                "args = [\"-I\", \"/h/.orcamcp/orcamcp-bridge.py\"]\n"
                "startup_timeout_sec = 20\n"
                "tool_timeout_sec = 140\n"
                "\n"
                "[mcp_servers.orca-slicer.env]\n"
                "ORCAMCP_TIMEOUT = \"300\"\n"
                "\n"
                "[mcp_servers.another]\n" // a table is defined once: Codex rejects a second [mcp_servers.other]
                "command = \"keep right\"\n");

    std::string error;
    REQUIRE(MCPClientConfig::remove_from_toml_config(path.string(), error));
    const std::string after = read_file(path);

    // Our table and its env subtable went, by the span Connect itself edits; the neighbour before
    // and the neighbour after stayed whole.
    CHECK(after.find("orca-slicer") == std::string::npos); // main table and subtable, with their keys
    CHECK(after.find("args") == std::string::npos);
    CHECK(after.find("startup_timeout_sec") == std::string::npos);
    CHECK(after.find("tool_timeout_sec") == std::string::npos);
    CHECK(after.find("[mcp_servers.other]\ncommand = \"keep left\"\n") != std::string::npos);
    CHECK(after.find("[mcp_servers.another]\ncommand = \"keep right\"\n") != std::string::npos);
    CHECK(count_of(after, "[mcp_servers.") == 2);

    // An entry in a form Disconnect cannot remove by span: refused, naming what to change, untouched.
    const auto inline_path = dir.path() / "inline.toml";
    write(inline_path, "[mcp_servers]\norca-slicer = { command = \"python3\" }\n");
    CHECK_FALSE(MCPClientConfig::remove_from_toml_config(inline_path.string(), error));
    CHECK(error.find("cannot edit") != std::string::npos);
    CHECK(read_file(inline_path) == "[mcp_servers]\norca-slicer = { command = \"python3\" }\n");

    // And nothing of ours at all: reported, file untouched.
    const auto none_path = dir.path() / "none.toml";
    write(none_path, "[mcp_servers.other]\ncommand = \"x\"\n");
    CHECK_FALSE(MCPClientConfig::remove_from_toml_config(none_path.string(), error));
    CHECK(error.find("not found") != std::string::npos);
    CHECK(read_file(none_path) == "[mcp_servers.other]\ncommand = \"x\"\n");
}

TEST_CASE("a developer's table is not ours because of a comment", "[McpClientConfig]")
{
    // Ownership reads the main table's args value, not the table's text: a comment that mentions the
    // shared path does not make the table ours.
    CHECK_FALSE(MCPClientConfig::is_our_toml_table("[mcp_servers.orca-slicer]\n"
                                                   "command = \"python3\"\n"
                                                   "args = [\"./scripts/orcamcp-bridge.py\"]  # not ~/.orcamcp/orcamcp-bridge.py\n"));

    // The shared bridge in the args value itself still says so, comment or not.
    CHECK(MCPClientConfig::is_our_toml_table("[mcp_servers.orca-slicer]\n"
                                             "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]  # runs my bridge\n"));

    // A path that stands only in a comment, or only in a subtable, names no entry of ours.
    CHECK_FALSE(MCPClientConfig::is_our_toml_table("[mcp_servers.orca-slicer]\n"
                                                   "command = \"python3\"  # runs ~/.orcamcp/orcamcp-bridge.py\n"));
    CHECK_FALSE(MCPClientConfig::is_our_toml_table("[mcp_servers.orca-slicer]\n"
                                                   "command = \"python3\"\n"
                                                   "\n[mcp_servers.orca-slicer.env]\nBRIDGE = \"/h/.orcamcp/orcamcp-bridge.py\"\n"));
}

TEST_CASE("a rewritten JSON config keeps its key order", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-json-order");
    const MCPClientInfo client = json_client(dir.path(), "config.json");
    write(client.config_path, R"({"zeta": 1, "alpha": "two", "mcpServers": {)"
                              R"("keep": {"command": "x"}, )"
                              R"("orca-slicer": {"command": "python3", "args": ["/h/.orcamcp/orcamcp-bridge.py"]}}})");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    CHECK(MCPClientConfig::refresh_client_entry(client, command, error) == EntryRefresh::Rewritten);

    // The user's own keys keep the order the file wrote them in: the rewrite edits the entry, it does
    // not rebuild the document.
    const nlohmann::ordered_json after = nlohmann::ordered_json::parse(read_file(client.config_path));
    CHECK(keys_in_order(after) == std::vector<std::string>{"zeta", "alpha", "mcpServers"});
    CHECK(keys_in_order(after["mcpServers"]) == std::vector<std::string>{"keep", "orca-slicer"});
    CHECK(after["mcpServers"]["orca-slicer"]["command"] == "/opt/orcamcp/python");

    // An entry Connect would write now is current whatever the key order: a second launch writes
    // nothing, so a rewrite can never churn on every startup.
    CHECK(MCPClientConfig::refresh_client_entry(client, command, error) == EntryRefresh::Current);
}

TEST_CASE("Connect refuses a servers value that is not an object", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-json-servers-value");
    const MCPClientInfo client = json_client(dir.path(), "config.json");
    const std::string   before = "{\"mcpServers\": \"my servers\"}";
    write(client.config_path, before);

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    CHECK_FALSE(MCPClientConfig::write_client_entry(client, command, error));
    CHECK(error.find("\"mcpServers\" is not an object") != std::string::npos);
    CHECK(read_file(client.config_path) == before); // the user's value is theirs to fix, byte for byte

    // The startup refresh sees no entry of ours there and never replaces the value either.
    CHECK(MCPClientConfig::refresh_client_entry(client, command, error) == EntryRefresh::NoEntry);
    CHECK(read_file(client.config_path) == before);
}

#ifndef _WIN32
TEST_CASE("an unreadable Codex config is left alone", "[McpClientConfig]")
{
    if (geteuid() == 0)
        return; // root is not refused by a chmod 000: there would be nothing to test

    ScopedTemporaryDir dir("orcamcp-unreadable-codex");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    const std::string   before = "[mcp_servers.orca-slicer]\ncommand = \"python3\"\n";
    write(codex.config_path, before);
    REQUIRE(chmod(codex.config_path.c_str(), 0) == 0);

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    CHECK_FALSE(MCPClientConfig::write_client_entry(codex, command, error));
    CHECK_FALSE(error.empty()); // the refusal says which file could not be opened

    REQUIRE(chmod(codex.config_path.c_str(), 0600) == 0);
    CHECK(read_file(codex.config_path) == before); // nothing was written over the file we could not read
}
#endif

TEST_CASE("Connect finds the Python this build ships", "[McpClientConfig]")
{
    // Upstream's finder (BundledPython.hpp) through bridge_interpreter, against the runtime copied beside
    // this test program (tests/slic3rutils/CMakeLists.txt): on every CI platform, the interpreter a
    // Connect entry would name exists. A runtime that moved or stopped shipping fails here, not on a
    // user's machine.
    const MCPClientConfig::InterpreterChoice choice = MCPClientConfig::bridge_interpreter();
    INFO(choice.error);
    REQUIRE(choice.error.empty());
    CHECK(boost::filesystem::is_regular_file(choice.path));
}

TEST_CASE("a header spelled with an escape is our table", "[McpClientConfig]")
{
    // [mcp_servers."orca\u002Dslicer"] names orca-slicer: updated in place, never a second table beside it.
    ScopedTemporaryDir dir("orcamcp-escaped-header");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.\"orca\\u002Dslicer\"]\n"
                             "command = \"python3\"\n"
                             "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string content = read_file(codex.config_path);
    CHECK(count_of(content, "[mcp_servers") == 1);
    CHECK(content.find("command = \"/opt/orcamcp/python\"") != std::string::npos);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
}

TEST_CASE("a comment inside a multi-line args never makes a table ours", "[McpClientConfig]")
{
    // A developer's table that once ran the shared bridge, and says so in a comment inside its array.
    ScopedTemporaryDir dir("orcamcp-comment-in-args");
    const MCPClientInfo codex  = codex_client(dir.path(), "config.toml");
    const std::string   before = "[mcp_servers.orca-slicer]\n"
                                 "command = \"python3\"\n"
                                 "args = [\n"
                                 "  \"./scripts/orcamcp-bridge.py\", # was /Users/u/.orcamcp/orcamcp-bridge.py\n"
                                 "]\n";
    write(codex.config_path, before);
    CHECK_FALSE(MCPClientConfig::is_our_toml_table(before));

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::NotOurs);
    CHECK(read_file(codex.config_path) == before);
}

TEST_CASE("an array of arrays in our table is kept and removed whole", "[McpClientConfig]")
{
    // Its inner arrays' lines start with '[' but are no headers: the table runs on past them.
    ScopedTemporaryDir dir("orcamcp-array-of-arrays");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    write(codex.config_path, "[mcp_servers.orca-slicer]\n"
                             "command = \"python3\"\n"
                             "args = [\"/h/.orcamcp/orcamcp-bridge.py\"]\n"
                             "matrix = [\n"
                             "  [1, 2],\n"
                             "  [3, 4],\n"
                             "]\n"
                             "\n"
                             "[other]\n"
                             "k = 1\n");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    const std::string connected = read_file(codex.config_path);
    CHECK(connected.find("matrix = [\n  [1, 2],\n  [3, 4],\n]\n") != std::string::npos);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);

    REQUIRE(MCPClientConfig::remove_from_toml_config(codex.config_path, error));
    CHECK(read_file(codex.config_path) == "[other]\nk = 1\n");
}

TEST_CASE("a header inside a multi-line string is text, not a table", "[McpClientConfig]")
{
    // Another server's description quotes our header: the string is theirs, and our table is still to add.
    ScopedTemporaryDir dir("orcamcp-header-in-string");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    const std::string   theirs = "[notes]\n"
                                 "text = \"\"\"\n"
                                 "[mcp_servers.orca-slicer]\n"
                                 "command = \"not this\"\n"
                                 "\"\"\"\n";
    write(codex.config_path, theirs);
    CHECK_FALSE(MCPClientConfig::toml_names_server_elsewhere(theirs));
    CHECK(MCPClientConfig::toml_server_table_of(theirs).empty());

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    REQUIRE(MCPClientConfig::write_client_entry(codex, command, error));
    CHECK(read_file(codex.config_path) == theirs + "\n" + MCPClientConfig::toml_server_table(command));

    REQUIRE(MCPClientConfig::remove_from_toml_config(codex.config_path, error));
    CHECK(read_file(codex.config_path) == theirs);
}

TEST_CASE("Disconnect removes a subtable that stands apart from our table", "[McpClientConfig]")
{
    // Left behind, the env subtable would define a server with no command, which Codex rejects.
    ScopedTemporaryDir dir("orcamcp-apart-subtable");
    const auto         path = dir.path() / "config.toml";
    write(path, "[mcp_servers.orca-slicer]\n"
                "command = \"python3\"\n"
                "\n"
                "[other]\n"
                "k = 1\n"
                "\n"
                "[mcp_servers.orca-slicer.env]\n"
                "ORCAMCP_TIMEOUT = \"300\"\n");

    std::string error;
    REQUIRE(MCPClientConfig::remove_from_toml_config(path.string(), error));
    CHECK(read_file(path) == "[other]\nk = 1\n");
}

TEST_CASE("a Codex config that is not TOML is refused and left alone", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-not-toml");
    const MCPClientInfo codex  = codex_client(dir.path(), "config.toml");
    const std::string   before = "[mcp_servers.orca-slicer]\n"
                                 "command = \"python3\"\n"
                                 "args = [\"/h/.orcamcp/orcamcp-bridge.py\"\n"; // the array never closes
    write(codex.config_path, before);

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    CHECK_FALSE(MCPClientConfig::write_client_entry(codex, command, error));
    CHECK(error.find("is not valid TOML (line ") != std::string::npos);
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Failed);
    CHECK(error.find("is not valid TOML") != std::string::npos);
    CHECK_FALSE(MCPClientConfig::remove_from_toml_config(codex.config_path, error));
    CHECK(error.find("is not valid TOML") != std::string::npos);
    CHECK(read_file(codex.config_path) == before);
}

TEST_CASE("the check after an edit finds any change but Connect's", "[McpClientConfig]")
{
    using Slic3r::GUI::OrcaMCP::codex_connect_difference;
    using Slic3r::GUI::OrcaMCP::codex_disconnect_difference;
    const Slic3r::GUI::OrcaMCP::CodexTimeouts least{20, 140};
    const std::vector<std::string>            args{"-I", "/h/.orcamcp/orcamcp-bridge.py"};
    const std::string before = "[other]\nk = 1 # one\nf = nan\n\n"
                               "[mcp_servers.orca-slicer]\ncommand = \"python3\"\ntool_timeout_sec = 600\nenabled = true\n";
    const std::string ours   = "command = \"/py\"\nargs = [\"-I\", \"/h/.orcamcp/orcamcp-bridge.py\"]\nstartup_timeout_sec = 20\n";

    // What Connect writes, laid out any way, comments gone: the same file.
    CHECK(codex_connect_difference(before, "[other]\nf = nan\nk = 1\n[mcp_servers.orca-slicer]\n" + ours +
                                               "enabled = true\ntool_timeout_sec = 600\n",
                                   "orca-slicer", "/py", args, least) == "");
    // Another value changed, a key of ours lost, a timeout lowered, text that does not parse.
    CHECK(codex_connect_difference(before, "[other]\nk = 2\nf = nan\n[mcp_servers.orca-slicer]\n" + ours +
                                               "enabled = true\ntool_timeout_sec = 600\n",
                                   "orca-slicer", "/py", args, least) == "other.k would change too");
    CHECK(codex_connect_difference(before, "[other]\nk = 1\nf = nan\n[mcp_servers.orca-slicer]\n" + ours +
                                               "tool_timeout_sec = 600\n",
                                   "orca-slicer", "/py", args, least) == "mcp_servers.orca-slicer.enabled would change too");
    CHECK(codex_connect_difference(before, "[other]\nk = 1\nf = nan\n[mcp_servers.orca-slicer]\n" + ours +
                                               "enabled = true\ntool_timeout_sec = 140\n",
                                   "orca-slicer", "/py", args, least) == "mcp_servers.orca-slicer.tool_timeout_sec would not be 600");
    CHECK(codex_connect_difference(before, before + "[mcp_servers.orca-slicer]\n", "orca-slicer", "/py", args, least)
              .find("the edited file would not parse (line ") == 0);

    // Disconnect: our server gone, and the [mcp_servers] table only it made; nothing else.
    CHECK(codex_disconnect_difference(before, "[other]\nk = 1\nf = nan\n", "orca-slicer") == "");
    CHECK(codex_disconnect_difference(before, "[other]\nk = 1\n", "orca-slicer") == "other.f would change too");
    CHECK(codex_disconnect_difference(before, before, "orca-slicer") == "mcp_servers.orca-slicer would still be defined");
}

TEST_CASE("a rewritten JSON entry keeps its own keys in order", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-json-entry-order");
    const MCPClientInfo client = json_client(dir.path(), "config.json");
    write(client.config_path, R"({"mcpServers": {"orca-slicer": {"env": {"ZED": "1", "ORCAMCP_TIMEOUT": "300"}, )"
                              R"("command": "python3", "args": ["/h/.orcamcp/orcamcp-bridge.py"], "cwd": "/h"}}})");

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    CHECK(MCPClientConfig::refresh_client_entry(client, command, error) == EntryRefresh::Rewritten);

    const nlohmann::ordered_json after = nlohmann::ordered_json::parse(read_file(client.config_path));
    const nlohmann::ordered_json& entry = after["mcpServers"]["orca-slicer"];
    CHECK(keys_in_order(entry) == std::vector<std::string>{"env", "command", "args", "cwd"});
    CHECK(keys_in_order(entry["env"]) == std::vector<std::string>{"ZED", "ORCAMCP_TIMEOUT"});
    CHECK(entry["command"] == "/opt/orcamcp/python");
}

// A corpus of awkward but valid Codex configs, of the shapes three generations of hand-written line
// scanning each guessed at and got wrong (docs/superpowers/specs/2026-11-18-connect-without-python-design.md,
// OrcaMCPCodexToml.hpp): quoted, spaced and escaped headers, BOMs and CRLF, comments and multi-line
// values that hold header-like text, arrays of arrays, subtables that stand apart, every TOML value
// type elsewhere. Every one is driven through Connect, the startup refresh and Disconnect, and each
// result must parse, hold exactly one of our tables with the bridge's args, and keep the user's own
// marker line outside our table. The refused forms must be left byte for byte, and a file that is not
// TOML at all must be refused by all three.

namespace {

struct CodexCorpusEntry
{
    std::string text;          // the config.toml as it starts
    std::string marker;        // a distinctive string outside our table; empty when the entry has no content to keep
    std::string after_connect; // the user's own line inside our table that Connect must keep; empty for none
    std::string after_remove;  // content outside our table that must survive Disconnect; empty for none
};

// The shared bridge path an old table might already run, so a corpus table is one of ours.
const char* const CORPUS_BRIDGE = "/h/.orcamcp/orcamcp-bridge.py";

// A plain table of ours, LF-terminated, as a user of an earlier OrcaMCP would have left it.
std::string corpus_table()
{
    return "[mcp_servers.orca-slicer]\n"
           "command = \"python3\"\n"
           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n";
}

const std::vector<CodexCorpusEntry>& codex_valid_corpus()
{
    static const std::vector<CodexCorpusEntry> corpus = [] {
        std::vector<CodexCorpusEntry> entries;
        // Nearly nothing: nothing outside the table to keep, so no marker.
        entries.push_back({"", "", "", ""});
        entries.push_back({std::string("keep_me_1 = 1\n"), "keep_me_1 = 1", "", ""});
        entries.push_back({utf8_bom(), "", "", ""});
        entries.push_back({utf8_bom() + corpus_table(), "", "", ""});
        // A comment line can carry the marker: it is not ours, and must survive every edit.
        entries.push_back({"# keep_me_2 = 1\n", "keep_me_2 = 1", "", ""});
        // BOM around a table of ours, and a root key under the BOM.
        entries.push_back({utf8_bom() + "keep_me_3 = 1\n" + corpus_table(), "keep_me_3 = 1", "", ""});
        // CRLF everywhere, with and without our table.
        entries.push_back({"keep_me_4 = 1\r\n[other]\r\nk = 1\r\n", "keep_me_4 = 1", "", "[other]"});
        entries.push_back({"keep_me_5 = 1\r\n[mcp_servers.orca-slicer]\r\ncommand = \"python3\"\r\nargs = [\"" +
                               std::string(CORPUS_BRIDGE) + "\"]\r\n",
                           "keep_me_5 = 1", "", ""});
        // Tabs and spaces around the equals signs and inside the header.
        entries.push_back({"keep_me_6\t=\t1\n"
                           "[\tmcp_servers\t.\torca-slicer\t]\n"
                           "command\t=\t\"python3\"\n"
                           "args\t=\t[\"" + std::string(CORPUS_BRIDGE) + "\"]\n",
                           "keep_me_6", "", ""});
        // Quoted and escaped spellings of our header.
        entries.push_back({"keep_me_7 = 1\n"
                           "[mcp_servers.\"orca-slicer\"]\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n",
                           "keep_me_7 = 1", "", ""});
        entries.push_back({"keep_me_8 = 1\n"
                           "[mcp_servers.'orca-slicer']\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n",
                           "keep_me_8 = 1", "", ""});
        entries.push_back({"keep_me_9 = 1\n"
                           "[mcp_servers.\"orca\\u002Dslicer\"]\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n",
                           "keep_me_9 = 1", "", ""});
        // Comments on the header line, after our keys, and inside a multi-line args array.
        entries.push_back({"keep_me_10 = 1\n"
                           "[mcp_servers.orca-slicer] # added by hand\n"
                           "command = \"python3\" # the old bridge\n"
                           "args = [ # over several lines\n"
                           "  \"" + std::string(CORPUS_BRIDGE) + "\", # the bridge\n"
                           "] # end\n"
                           "enabled = true\n",
                           "keep_me_10 = 1", "enabled = true", ""});
        // An env subtable right after our table, and one separated from it by another table.
        entries.push_back({"keep_me_11 = 1\n" + corpus_table() +
                               "\n"
                               "[mcp_servers.orca-slicer.env]\n"
                               "ORCAMCP_TIMEOUT = \"300\"\n",
                           "keep_me_11 = 1", "ORCAMCP_TIMEOUT = \"300\"", ""});
        entries.push_back({"keep_me_12 = 1\n" + corpus_table() +
                               "\n"
                               "[other]\nk = 1\n"
                               "\n"
                               "[mcp_servers.orca-slicer.env]\n"
                               "ORCAMCP_PORT = \"13625\"\n",
                           "keep_me_12 = 1", "", "[other]\nk = 1"});
        // env written as dotted keys inside our table: the table is still an editable table.
        entries.push_back({"keep_me_13 = 1\n"
                           "[mcp_servers.orca-slicer]\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n"
                           "env.X = \"1\"\n",
                           "keep_me_13 = 1", "env.X = \"1\"", ""});
        // The user's own keys and timeouts: kept by Connect, never shrunk, re-spelled or re-typed.
        entries.push_back({"keep_me_14 = 1\n"
                           "[mcp_servers.orca-slicer]\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n"
                           "enabled = true\n"
                           "cwd = \"/x\"\n"
                           "tool_timeout_sec = 600\n",
                           "keep_me_14 = 1", "cwd = \"/x\"\ntool_timeout_sec = 600", ""});
        entries.push_back({"keep_me_15 = 1\n"
                           "[mcp_servers.orca-slicer]\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n"
                           "startup_timeout_sec = 1_000\n",
                           "keep_me_15 = 1", "startup_timeout_sec = 1_000", ""});
        entries.push_back({"keep_me_16 = 1\n"
                           "[mcp_servers.orca-slicer]\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n"
                           "tool_timeout_sec = 600.0\n",
                           "keep_me_16 = 1", "tool_timeout_sec = 600.0", ""});
        entries.push_back({"keep_me_17 = 1\n"
                           "[mcp_servers.orca-slicer]\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n"
                           "tool_timeout_sec = \"30\"\n",
                           "keep_me_17 = 1", "tool_timeout_sec = \"30\"", ""});
        // Header-like and key-like text inside multi-line strings: theirs, not tables of ours.
        entries.push_back({"keep_me_18 = 1\n"
                           "[notes]\n"
                           "basic = \"\"\"\n"
                           "[mcp_servers.orca-slicer]\n"
                           "command = \"x\"\n"
                           "\"\"\"\n",
                           "keep_me_18 = 1", "", "[notes]\nbasic = \"\"\""});
        entries.push_back({"keep_me_19 = 1\n"
                           "[notes]\n"
                           "literal = '''\n"
                           "[mcp_servers.orca-slicer]\n"
                           "command = \"x\"\n"
                           "'''\n",
                           "keep_me_19 = 1", "", "[notes]\nliteral = '''"});
        // Arrays of arrays in our table and in another: inner lines starting with '[' are no headers.
        entries.push_back({"keep_me_20 = 1\n"
                           "[mcp_servers.orca-slicer]\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n"
                           "matrix = [\n"
                           "  [1, 2],\n"
                           "  [3, 4],\n"
                           "]\n"
                           "\n"
                           "[other]\n"
                           "grid = [[0]]\n",
                           "keep_me_20 = 1", "matrix = [\n  [1, 2],\n  [3, 4],\n]", "[other]\ngrid = [[0]]"});
        // Arrays of tables before and after our table, and a nested array of tables under another server.
        entries.push_back({"keep_me_21 = 1\n"
                           "[[other]]\nn = 1\n"
                           "\n" +
                               corpus_table() +
                               "\n"
                               "[[mcp_servers.other.tools]]\nname = \"t\"\n"
                               "\n"
                               "[[other]]\nn = 2\n",
                           "keep_me_21 = 1", "", "[[mcp_servers.other.tools]]"});
        // An explicit empty [mcp_servers] and other servers around our table.
        entries.push_back({"keep_me_22 = 1\n"
                           "[mcp_servers]\n"
                           "[mcp_servers.a]\n"
                           "command = \"a\"\n"
                           "\n" +
                               corpus_table() +
                               "\n"
                               "[mcp_servers.b]\n"
                               "command = \"b\"\n",
                           "keep_me_22 = 1", "", "[mcp_servers.a]\ncommand = \"a\""});
        // Our table with nothing in it, and one holding only a timeout: Connect fills what is missing.
        entries.push_back({"keep_me_23 = 1\n[mcp_servers.orca-slicer]\n", "keep_me_23 = 1", "", ""});
        entries.push_back({"keep_me_24 = 1\n"
                           "[mcp_servers.orca-slicer]\n"
                           "tool_timeout_sec = 600\n",
                           "keep_me_24 = 1", "tool_timeout_sec = 600", ""});
        // The last line without a trailing newline, with and without our table.
        entries.push_back({"keep_me_25 = 1", "keep_me_25 = 1", "", ""});
        entries.push_back({"keep_me_26 = 1\n"
                           "[mcp_servers.orca-slicer]\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]",
                           "keep_me_26 = 1", "", ""});
        // Every kind of value elsewhere: the parser reads them, and an edit must not move one.
        // "ключ" and "é" = "ü" written as UTF-8 byte escapes, so the source stays plain ASCII.
        entries.push_back({"keep_me_27 = 1\n"
                           "[kinds]\n"
                           "n = nan\n"
                           "p = inf\n"
                           "m = -inf\n"
                           "h = 0x1F\n"
                           "e = 1e5\n"
                           "d = 1979-05-27\n"
                           "dt = 1979-05-27T07:32:00Z\n"
                           "t = 07:32:00\n"
                           "big = 9223372036854775807\n"
                           "\"\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\" = 1\n"
                           "\"\xC3\xA9\" = \"\xC3\xBC\"\n",
                           "keep_me_27 = 1", "", "big = 9223372036854775807"});
        // A neighbour whose name only starts with ours: never touched.
        entries.push_back({"keep_me_28 = 1\n"
                           "[mcp_servers.orca-slicer-old]\n"
                           "command = \"not ours\"\n"
                           "\n" +
                               corpus_table(),
                           "keep_me_28 = 1", "", "[mcp_servers.orca-slicer-old]\ncommand = \"not ours\""});
        // Quoted key spellings of our own keys, and a quoted subtable header.
        entries.push_back({"keep_me_29 = 1\n"
                           "[mcp_servers.orca-slicer]\n"
                           "\"command\" = \"python3\"\n"
                           "'args' = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n",
                           "keep_me_29 = 1", "", ""});
        entries.push_back({"keep_me_30 = 1\n"
                           "[mcp_servers.\"orca-slicer\"] # quoted, with env\n"
                           "command = \"python3\"\n"
                           "args = [\"" + std::string(CORPUS_BRIDGE) + "\"]\n"
                           "[mcp_servers.\"orca-slicer\".env]\n"
                           "A = \"b\"\n",
                           "keep_me_30 = 1", "A = \"b\"", ""});
        // An inline table elsewhere: its braces are values, never a header or a statement of ours.
        entries.push_back({"keep_me_31 = 1\n"
                           "[other]\n"
                           "conf = { a = 1, b = [1, 2] }\n"
                           "\n" +
                               corpus_table(),
                           "keep_me_31 = 1", "", "conf = { a = 1, b = [1, 2] }"});
        return entries;
    }();
    return corpus;
}

// Valid TOML that defines our server in a form this code will not guess at: Connect refuses it,
// Disconnect refuses it, and the file is never touched.
const std::vector<std::string>& codex_refused_corpus()
{
    static const std::vector<std::string> corpus = {
        "keep_me_r1 = 1\n[mcp_servers]\norca-slicer.command = \"x\"\n",
        "keep_me_r2 = 1\nmcp_servers = { orca-slicer = { command = \"x\" } }\n",
        "keep_me_r3 = 1\n[[mcp_servers.orca-slicer]]\ncommand = \"x\"\n",
        "keep_me_r4 = 1\nmcp_servers.orca-slicer.command = \"x\"\n",
    };
    return corpus;
}

// Not TOML at all: Codex could not read these either, so all three paths refuse and touch nothing.
const std::vector<std::string>& codex_invalid_corpus()
{
    static const std::vector<std::string> corpus = {
        "[mcp_servers.orca-slicer]\ncommand = \"python3\"\nargs = [\"" + std::string(CORPUS_BRIDGE) + "\"\n", // the array never closes
        "[a]\nk = 1\n\n[a]\nj = 2\n",                                                                        // a table defined twice
        "keep_me_bad = 1\nkey = \n",                                                                         // no value
        "note = \"\"\" an unclosed string\n",                                                                // the string never closes
        "[mcp_servers.orca-slicer]\ncommand = \"a\"\ncommand = \"b\"\n",                                     // a key twice in our table
        "[mcp_servers.orca-slicer]\nk = 1\n\n[mcp_servers.orca-slicer]\nj = 2\n",                            // our table defined twice
    };
    return corpus;
}

} // namespace

TEST_CASE("every Codex config in the corpus is edited into valid TOML or left byte for byte", "[McpClientConfig]")
{
    using Slic3r::GUI::OrcaMCP::read_codex_server;
    const auto command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");

    std::vector<std::string> refused_though_valid;

    for (const CodexCorpusEntry& entry : codex_valid_corpus()) {
        ScopedTemporaryDir dir("orcamcp-codex-corpus");
        const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
        write(codex.config_path, entry.text);
        INFO(entry.text);

        std::string error;
        const bool  written = MCPClientConfig::write_client_entry(codex, command, error);
        if (written) {
            // What Connect wrote is TOML Codex can read, holds our table once with the bridge's args,
            // and keeps the user's own content outside it. CHECK, never REQUIRE: one bad entry must not
            // hide the ones after it.
            const std::string                connected = read_file(codex.config_path);
            const OrcaMCP::CodexServerRead after_write = read_codex_server(connected, "orca-slicer");
            CHECK(after_write.parse_error.empty());
            CHECK(after_write.table.has_value());
            CHECK(after_write.args == command.args);
            if (!entry.marker.empty())
                CHECK(connected.find(entry.marker) != std::string::npos);
            // The user's own line inside our table survives Connect; Disconnect removes it with the
            // entry, which is what Disconnect is for, so it is checked here and not after removal.
            if (!entry.after_connect.empty())
                CHECK(connected.find(entry.after_connect) != std::string::npos);

            // What Connect just wrote is current: the next startup refresh writes nothing at all.
            CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Current);
            CHECK(read_file(codex.config_path) == connected);

            // Disconnect takes our table and its subtables out, and nothing else.
            CHECK(MCPClientConfig::remove_from_toml_config(codex.config_path, error));
            const std::string      removed = read_file(codex.config_path);
            const OrcaMCP::CodexServerRead after_remove = read_codex_server(removed, "orca-slicer");
            CHECK(after_remove.parse_error.empty());
            CHECK_FALSE(after_remove.named);
            if (!entry.marker.empty())
                CHECK(removed.find(entry.marker) != std::string::npos);
            if (!entry.after_remove.empty())
                CHECK(removed.find(entry.after_remove) != std::string::npos);
        } else {
            // A refusal of a valid config is itself the regression this corpus guards against -- the
            // list after the loop names it -- but a refusal must at least never touch the file.
            INFO(error);
            CHECK(read_file(codex.config_path) == entry.text);
            CHECK_FALSE(error.empty());
            refused_though_valid.push_back(entry.text);
        }
    }
    INFO("configs a valid corpus entry was refused editing: " << refused_though_valid.size());
    CHECK(refused_though_valid.empty());

    for (const std::string& before : codex_refused_corpus()) {
        ScopedTemporaryDir dir("orcamcp-codex-refused");
        const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
        write(codex.config_path, before);
        INFO(before);

        std::string error;
        CHECK_FALSE(MCPClientConfig::write_client_entry(codex, command, error));
        CHECK_FALSE(error.empty());
        CHECK(read_file(codex.config_path) == before); // refused means untouched

        CHECK_FALSE(MCPClientConfig::remove_from_toml_config(codex.config_path, error));
        CHECK_FALSE(error.empty());
        CHECK(read_file(codex.config_path) == before);
    }

    for (const std::string& broken : codex_invalid_corpus()) {
        ScopedTemporaryDir dir("orcamcp-codex-invalid");
        const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
        write(codex.config_path, broken);
        INFO(broken);

        std::string error;
        CHECK_FALSE(MCPClientConfig::write_client_entry(codex, command, error));
        CHECK(error.find("is not valid TOML") != std::string::npos);
        CHECK(read_file(codex.config_path) == broken);

        CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Failed);
        CHECK(error.find("is not valid TOML") != std::string::npos);

        CHECK_FALSE(MCPClientConfig::remove_from_toml_config(codex.config_path, error));
        CHECK(error.find("is not valid TOML") != std::string::npos);
        CHECK(read_file(codex.config_path) == broken); // byte for byte, through all three refusals
    }
}
