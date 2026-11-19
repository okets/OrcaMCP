// src/slic3r/GUI/OrcaMCP/OrcaMCPAtomicWrite.hpp
#pragma once
#include <boost/filesystem/path.hpp>
#include <string>

// One atomic file writer for everything OrcaMCP rewrites in the user's own files: the instance
// registry's entries, and the AI apps' config files the Connect button edits (Claude Desktop's,
// Claude Code's ~/.claude.json, Codex's TOML).
//
// A crash or a full disk mid-write must never leave a half-written config behind, so no file is ever
// written in place: the content goes into a temporary beside it and is renamed over it, which the
// operating system delivers whole to every reader. Two properties of the user's files survive it:
// a config that is a symbolic link (a dotfiles setup) keeps being one, and a private file's
// permissions (0600, as ~/.claude.json is) do not widen.
//
// Unit-tested in tests/slic3rutils/test_mcp_atomic_write.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

enum class WritePermissions { KeepExisting, OwnerOnly };

// Writes `content` to `target` so no reader ever sees half of it: into a temporary of its own name beside
// the file ("<file>.<random>.tmp"), given its final permissions before any content, then renamed over it.
// A symbolic link is followed: the file it points to is replaced and the link stays. KeepExisting gives
// the new file the permissions the old one had (none to copy: the defaults), and refuses a file its owner
// cannot write; OwnerOnly gives owner read and write only. Returns "" or what went wrong; on failure the
// temporary is removed and the target is unchanged.
std::string write_file_atomically(const boost::filesystem::path& target, const std::string& content,
                                  WritePermissions permissions);

}}} // namespace Slic3r::GUI::OrcaMCP
