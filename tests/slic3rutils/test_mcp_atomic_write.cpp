#include "slic3r/GUI/OrcaMCP/OrcaMCPAtomicWrite.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <iterator>
#include <string>

#include "test_utils.hpp"

// The one atomic file writer (OrcaMCPAtomicWrite.hpp): what the instance registry writes through and
// what Connect will rewrite the AI apps' configs with. Every test works in a folder of its own.

using namespace Slic3r::GUI::OrcaMCP;
namespace fs = boost::filesystem;

namespace {

std::string read_file(const fs::path& path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::string& content)
{
    boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out << content;
}

fs::perms permissions_of(const fs::path& path)
{
    boost::system::error_code ec;
    return fs::status(path, ec).permissions() & fs::all_all;
}

bool path_exists(const fs::path& path)
{
    boost::system::error_code ec;
    return fs::exists(path, ec);
}

} // namespace

TEST_CASE("an atomic write replaces the content", "[McpAtomicWrite]")
{
    ScopedTemporaryDir dir("orcamcp-atomic");
    const fs::path file = dir.path() / "claude.json";
    write_file(file, "old");

    CHECK(write_file_atomically(file, "new", WritePermissions::KeepExisting).empty());
    CHECK(read_file(file) == "new");
    CHECK_FALSE(path_exists(fs::path(file.string() + ".tmp")));
}

TEST_CASE("a write into a missing folder reports an error and leaves no temporary", "[McpAtomicWrite]")
{
    ScopedTemporaryDir dir("orcamcp-atomic");
    const fs::path missing  = dir.path() / "no-such-folder";
    const fs::path file     = missing / "config.json";

    const std::string failure = write_file_atomically(file, "text", WritePermissions::KeepExisting);
    CHECK_FALSE(failure.empty());
    CHECK_FALSE(path_exists(missing));
    CHECK_FALSE(path_exists(file));
    CHECK_FALSE(path_exists(fs::path(file.string() + ".tmp")));
}

TEST_CASE("a file of the user's named like a temporary survives a write", "[McpAtomicWrite]")
{
    ScopedTemporaryDir dir("orcamcp-atomic");
    const fs::path file  = dir.path() / "config.toml";
    const fs::path theirs = dir.path() / "config.toml.tmp";
    write_file(file, "old");
    write_file(theirs, "the user's own notes");

    CHECK(write_file_atomically(file, "new", WritePermissions::KeepExisting).empty());
    CHECK(read_file(file) == "new");
    CHECK(read_file(theirs) == "the user's own notes");
    // Nothing of the write is left behind: the folder holds the two files it held.
    CHECK(std::distance(fs::directory_iterator(dir.path()), fs::directory_iterator()) == 2);
}

#ifndef _WIN32 // symlinks and permission bits, as the dotfiles and private-config setups are

TEST_CASE("a write through a symbolic link changes the file it points to and keeps the link", "[McpAtomicWrite]")
{
    ScopedTemporaryDir dir("orcamcp-atomic");
    const fs::path real = dir.path() / "real.json";
    const fs::path link = dir.path() / "claude.json";
    write_file(real, "old");
    boost::system::error_code ec;
    fs::create_symlink(real, link, ec);
    REQUIRE_FALSE(ec);

    CHECK(write_file_atomically(link, "new", WritePermissions::KeepExisting).empty());
    CHECK(fs::is_symlink(link));
    CHECK(read_file(real) == "new");
    CHECK_FALSE(path_exists(fs::path(real.string() + ".tmp")));
}

TEST_CASE("a write through a chain of links changes the last file and keeps every link", "[McpAtomicWrite]")
{
    ScopedTemporaryDir dir("orcamcp-atomic");
    fs::create_directory(dir.path() / "dotfiles");
    const fs::path real   = dir.path() / "dotfiles" / "config.toml";
    const fs::path middle = dir.path() / "middle.toml";
    const fs::path link   = dir.path() / "config.toml";
    write_file(real, "old");
    boost::system::error_code ec;
    fs::create_symlink("dotfiles/config.toml", middle, ec); // relative, as dotfiles managers write them
    REQUIRE_FALSE(ec);
    fs::create_symlink(middle, link, ec);
    REQUIRE_FALSE(ec);

    CHECK(write_file_atomically(link, "new", WritePermissions::KeepExisting).empty());
    CHECK(fs::is_symlink(link));
    CHECK(fs::is_symlink(middle));
    CHECK(read_file(real) == "new");
}

TEST_CASE("a rewritten private file stays private", "[McpAtomicWrite]")
{
    ScopedTemporaryDir dir("orcamcp-atomic");
    const fs::path file = dir.path() / "claude.json";
    write_file(file, "old");
    const fs::perms private_only = fs::owner_read | fs::owner_write;
    boost::system::error_code ec;
    fs::permissions(file, private_only, ec);
    REQUIRE_FALSE(ec);

    CHECK(write_file_atomically(file, "new", WritePermissions::KeepExisting).empty());
    CHECK(permissions_of(file) == private_only);
}

TEST_CASE("a config its owner made read-only is refused and left as it is", "[McpAtomicWrite]")
{
    ScopedTemporaryDir dir("orcamcp-atomic");
    const fs::path file = dir.path() / "config.toml";
    write_file(file, "old");
    const fs::perms read_only = fs::owner_read | fs::group_read | fs::others_read;
    boost::system::error_code ec;
    fs::permissions(file, read_only, ec);
    REQUIRE_FALSE(ec);

    const std::string failure = write_file_atomically(file, "new", WritePermissions::KeepExisting);
    CHECK(failure.find("read-only") != std::string::npos);
    CHECK(read_file(file) == "old");
    CHECK(permissions_of(file) == read_only);
    fs::permissions(file, fs::owner_read | fs::owner_write, ec); // let the folder's cleanup remove it
}

TEST_CASE("an owner-only write is private", "[McpAtomicWrite]")
{
    ScopedTemporaryDir dir("orcamcp-atomic");
    const fs::path file = dir.path() / "codex.toml";

    CHECK(write_file_atomically(file, "text", WritePermissions::OwnerOnly).empty());
    CHECK(permissions_of(file) == (fs::owner_read | fs::owner_write));
}

#endif
