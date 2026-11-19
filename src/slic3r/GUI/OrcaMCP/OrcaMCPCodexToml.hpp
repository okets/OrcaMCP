// src/slic3r/GUI/OrcaMCP/OrcaMCPCodexToml.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Codex's config.toml as a TOML parser reads it (toml++, deps_src/tomlplusplus; this file's .cpp is the
// only one that compiles it). Connect edits the text where it stands, so the user's comments, spacing and
// order survive, but every position it edits comes from the parser: which lines are table headers (a line
// inside a multi-line string or array never is), what each header names however it is quoted or escaped,
// and where each statement ends. And every edit is checked before it is written: the edited text must
// parse, and read the same as before everywhere except what Connect writes. Three rounds of hand-written
// line scanning each missed legal spellings (a header inside a string, an array of arrays, dotted keys
// under [mcp_servers]) and wrote files Codex could not read; this replaced them.
//
// Unit-tested in tests/slic3rutils/test_mcp_client_config.cpp ([McpClientConfig]).

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A byte range of a text, [begin, end).
struct TomlSpan
{
    size_t begin = 0;
    size_t end   = 0;
};

// Where and how a config defines mcp_servers.<name>.
struct CodexServerRead
{
    std::string              parse_error;   // Not empty: the text is not TOML, so Codex cannot read it either
    bool                     named = false; // mcp_servers.<name> is defined, in any form
    std::optional<TomlSpan>  table;         // Its [mcp_servers.<name>] header line up to the next header line
    std::vector<TomlSpan>    regions;       // That one and every region under an [mcp_servers.<name>.*] or
                                            // [[mcp_servers.<name>...]] header, in file order
    std::vector<std::string> args;          // The entry's string arguments as the parser reads them, in any form
};

// `content` read for mcp_servers.<name>. A file that is not TOML gives only `parse_error`, naming the line
// and column. A leading UTF-8 BOM is never part of a region.
CodexServerRead read_codex_server(const std::string& content, const std::string& server_name);

// The least each Codex timeout must be.
struct CodexTimeouts
{
    int64_t startup_sec = 0;
    int64_t tool_sec    = 0;
};

// `table_text` -- a [mcp_servers.<name>] header line and what follows it, subtables included -- with the
// main table's command and args statements removed, however many lines their values take, and
// `command_lines` written right after the header line. A timeout the main table sets to an integer below
// its minimum is removed and the minimum written after the command lines; an absent one is written there
// too; an integer at least the minimum, or a value of another type (600.0, "30"), stays exactly as written.
// Every other byte stays as it is. nullopt when the text is not TOML or does not begin with that header.
std::optional<std::string> update_codex_table(const std::string& table_text, const std::string& server_name,
                                              const std::string& command_lines, const CodexTimeouts& minimum);

// "" when `after` reads, as TOML, the same as `before` everywhere except mcp_servers.<name>'s command,
// args and timeouts, and those hold what Connect writes: `command`, exactly `args`, and each timeout the
// larger of its integer value before and the minimum (a timeout that was not an integer unchanged).
// Otherwise what differs, in words that follow "because": "the edited file would not parse (line 3,
// column 7: ...)", "other.key would change too".
std::string codex_connect_difference(const std::string& before, const std::string& after,
                                     const std::string& server_name, const std::string& command,
                                     const std::vector<std::string>& args, const CodexTimeouts& minimum);

// "" when `after` reads, as TOML, the same as `before` without mcp_servers.<name> (and without an
// [mcp_servers] table only that entry made); otherwise what differs, as codex_connect_difference says it.
std::string codex_disconnect_difference(const std::string& before, const std::string& after,
                                        const std::string& server_name);

}}} // namespace Slic3r::GUI::OrcaMCP
