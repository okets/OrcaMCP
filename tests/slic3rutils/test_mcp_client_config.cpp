#include "slic3r/GUI/OrcaMCP/MCPClientConfig.hpp"

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
        "[mcp_servers.other]\n"
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
    CHECK(replaced.find("[mcp_servers.other]\ncommand = \"right\"\n") != std::string::npos);
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
    CHECK(MCPClientConfig::is_our_toml_table(R"(command = "python3"
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

    // Both forms define our server in TOML this code will not guess at.
    for (const char* before : {"[mcp_servers]\nother = { command = \"keep\" }\norca-slicer = { command = \"python3\" }\n",
                               "mcp_servers.orca-slicer.command = \"python3\"\n"
                               "mcp_servers.orca-slicer.args = [\"-I\", \"/h/.orcamcp/orcamcp-bridge.py\"]\n",
                               "mcp_servers.\"orca-slicer\".command = \"python3\"\n"}) {
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

TEST_CASE("a multi-line string command is refused untouched", "[McpClientConfig]")
{
    ScopedTemporaryDir dir("orcamcp-multiline-command");
    const MCPClientInfo codex = codex_client(dir.path(), "config.toml");
    const std::string   before = "[mcp_servers.orca-slicer]\n"
                                 "command = \"\"\"\n"
                                 "python3\n"
                                 "\"\"\"\n"
                                 "startup_timeout_sec = 5\n";
    write(codex.config_path, before);

    const auto  command = MCPClientConfig::bridge_command("/opt/orcamcp/python", "/h/.orcamcp/orcamcp-bridge.py");
    std::string error;
    CHECK_FALSE(MCPClientConfig::write_client_entry(codex, command, error));
    CHECK(error.find("cannot update") != std::string::npos);
    CHECK(read_file(codex.config_path) == before);
    CHECK_FALSE(MCPClientConfig::updated_toml_server_table(before, command).has_value());

    // The same with the bridge in a multi-line string args -- a table whose args say it is ours, and
    // whose value this code still cannot rewrite: Failed, and the startup refresh leaves it alone.
    const std::string ours = "[mcp_servers.orca-slicer]\n"
                             "command = \"python3\"\n"
                             "args = \"\"\"\n/h/.orcamcp/orcamcp-bridge.py\n\"\"\"\n";
    write(codex.config_path, ours);
    CHECK(MCPClientConfig::is_our_toml_table(ours));
    CHECK(MCPClientConfig::refresh_client_entry(codex, command, error) == EntryRefresh::Failed);
    CHECK(error.find("cannot update") != std::string::npos);
    CHECK(read_file(codex.config_path) == ours);
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
                "[mcp_servers.other]\n"
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
    CHECK(after.find("[mcp_servers.other]\ncommand = \"keep right\"\n") != std::string::npos);
    CHECK(count_of(after, "[mcp_servers.other]") == 2);

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
