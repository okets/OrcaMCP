#include "slic3r/GUI/OrcaMCP/MCPClientConfig.hpp"

#include <catch2/catch_test_macros.hpp>

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
