#include "slic3r/GUI/OrcaMCP/MCPClientConfig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/fstream.hpp>

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
