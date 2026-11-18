#include "slic3r/GUI/OrcaMCP/MCPClientConfig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/fstream.hpp>

#include <sstream>
#include <string>

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
