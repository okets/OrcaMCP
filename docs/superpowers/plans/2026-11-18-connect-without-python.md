# Connect without Python Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect in Preferences writes client entries that run the bridge with the app's own Python, and the app keeps its entries current at startup, so no user installs Python (except on the Linux AppImage).

**Architecture:** Pure decisions (entry contents, TOML table replacement, ownership, staleness, interpreter choice) become static functions of `MCPClientConfig`, unit-tested without the app. The app side gathers inputs (upstream's bundled-Python finder, `APPIMAGE`, the system `python3`) and writes configs through one atomic writer shared with the instance registry.

**Tech Stack:** C++17, Boost.Filesystem, boost::process v1 via `slic3r/Utils/BoostProcessCompat.hpp`, nlohmann::json, Catch2 (`slic3rutils_tests`), Python unittest for the bridge.

**Spec:** `docs/superpowers/specs/2026-11-18-connect-without-python-design.md`

## Global Constraints

- Entry args are exactly `["-I", "<shared dir>/orcamcp-bridge.py"]`; `<shared dir>` is `MCPClientConfig::get_shared_scripts_dir()`.
- Codex table carries `startup_timeout_sec = 20` and `tool_timeout_sec = 140`.
- An entry is *ours* only when it is named `orca-slicer` and one of its args, with `\` read as `/`, ends with `/.orcamcp/orcamcp-bridge.py`.
- The startup refresh runs only when `bridge_copy_decision(...).copy`'s gate inputs say not a test launch and not a build tree (the same `own_data_folder` / `build_tree` inputs; content difference is not required).
- Linux system Python must be 3.9 or newer. Error text for none: `Connecting needs Python 3.9 or newer, which this AppImage does not include: install your distribution's python3 package, then press Connect again.`
- Windows-only breakage precheck before pushing: no identifiers named `near`, `far`, `small`, `min`, `max`, `ERROR`, `DELETE`, `IN`, `OUT`; no `/`-based path surgery on Windows paths.

## Review Focus

1. A client config that is a symbolic link (dotfiles setups): the write must replace the link's target file, leaving the link. (Task 2 test.)
2. A private config file (`~/.claude.json` is 0600): the rewritten file keeps the original's permissions. (Task 2 test, POSIX only.)
3. A Codex config with comments, other servers and an `[mcp_servers.orca-slicer.env]` subtable: only our table and its subtables change. (Task 1 test.)
4. A path holding `"`, `\` or `'` (Windows paths, a user named O'Brien): TOML and JSON both round-trip it. (Task 1 test.)
5. A config that does not parse: never rewritten, at Connect or startup; the error is reported or logged. (Task 3 test.)

---

### Task 1: Entry decisions apart from the app

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/MCPClientConfig.hpp`, `src/slic3r/GUI/OrcaMCP/MCPClientConfig.cpp`
- Test: `tests/slic3rutils/test_mcp_client_config.cpp`

**Interfaces:**
- Produces (public statics of `MCPClientConfig`):
  - `struct BridgeCommand { std::string command; std::vector<std::string> args; };`
  - `static BridgeCommand bridge_command(const std::string& interpreter, const std::string& bridge_script);` -> `{interpreter, {"-I", bridge_script}}`
  - `static nlohmann::json server_entry(const MCPClientInfo& info, const BridgeCommand& command);` -> `command`, `args`, plus `"type": "stdio"` when `requires_type_field`, `"tools": ["*"]` when `requires_tools_field`
  - `static std::string toml_basic_string(const std::string& value);` -> `"..."` with `\` -> `\\`, `"` -> `\"`, control characters as `\uXXXX`
  - `static std::string toml_server_table(const BridgeCommand& command);` -> exactly `[mcp_servers.orca-slicer]\ncommand = <basic>\nargs = [<basic>, <basic>]\nstartup_timeout_sec = 20\ntool_timeout_sec = 140\n`
  - `static std::string toml_server_table_of(const std::string& content);` -> the text of `[mcp_servers.orca-slicer]` (bare or `"orca-slicer"` quoted header) and every following `[mcp_servers.orca-slicer.*]` subtable, up to the next other table header; `""` when absent
  - `static std::string replace_toml_server_table(const std::string& content, const std::string& table);` -> `content` without `toml_server_table_of`'s text, with `table` appended after one blank line
  - `static bool is_our_entry(const nlohmann::json& entry);` and `static bool is_our_toml_table(const std::string& table_text);` (Global Constraints rule; TOML reads `\\` and `\` as `/`)
- Replaces `generate_server_entry` (removed) and the TOML text in `add_to_toml_config`.

- [ ] **Step 1: Write the failing tests** (tag `[McpClientConfig]`):
  - `"an entry runs the bridge with the given interpreter, isolated"`: `server_entry(claude_code_info, bridge_command("/A/python3", "/h/.orcamcp/orcamcp-bridge.py"))` equals `{"command":"/A/python3","args":["-I","/h/.orcamcp/orcamcp-bridge.py"],"type":"stdio"}`; Copilot's has `"tools":["*"]` and no `type`.
  - `"Codex's table gives the bridge time to start and to answer"`: `toml_server_table` of `{"C:\\Program Files\\OrcaMCP\\python\\python.exe", {"-I","C:\\Users\\O'Brien\\.orcamcp\\orcamcp-bridge.py"}}` equals the exact text, with `\\` doubled and `O'Brien` unchanged.
  - `"a quote in a path is escaped"`: `toml_basic_string("a\"b")` == `"\"a\\\"b\""`.
  - `"replacing Codex's table keeps everything else"`: content with a leading comment, `[mcp_servers.other]` before and after, our table with an `[mcp_servers.orca-slicer.env]` subtable in the middle; result keeps the comment and both `other` tables verbatim, has no `orca-slicer.env`, and ends with the new table. Also the quoted header form `[mcp_servers."orca-slicer"]` is replaced.
  - `"an entry is ours only when it runs the shared bridge"`: true for args `["/Users/x/.orcamcp/orcamcp-bridge.py"]` (old python3 form), `["-I","C:\\Users\\x\\.orcamcp\\orcamcp-bridge.py"]`; false for `["./scripts/orcamcp-bridge.py"]` and for an entry with no `args`. TOML: true for the old `args = ["C:/Users/x/.orcamcp/orcamcp-bridge.py"]` table, false for `""`.
- [ ] **Step 2: Build and run** `slic3rutils_tests "[McpClientConfig]"`; expected: compile failure on the missing names.
- [ ] **Step 3: Implement** the functions above. `toml_server_table_of` scans line by line; a header is a line whose first non-blank character is `[`.
- [ ] **Step 4: Run** `slic3rutils_tests "[McpClientConfig]"`; expected: all pass.
- [ ] **Step 5: Commit.**

### Task 2: One atomic writer

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPAtomicWrite.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPAtomicWrite.cpp` (add both to `src/slic3r/CMakeLists.txt` beside the other OrcaMCP files)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPInstanceRegistry.cpp` (`write_entry` uses it)
- Test: `tests/slic3rutils/test_mcp_client_config.cpp` (`[McpAtomicWrite]`)

**Interfaces:**
- Produces: `namespace Slic3r::GUI::OrcaMCP { enum class WritePermissions { KeepExisting, OwnerOnly }; std::string write_file_atomically(const boost::filesystem::path& target, const std::string& content, WritePermissions permissions); }` -> `""` or the error. Resolves a symbolic link to its target first; writes `<resolved>.tmp` in binary mode; applies the original's permissions (`KeepExisting`, when the file exists) or owner read/write (`OwnerOnly`); renames over the resolved path; removes the temporary on failure.

- [ ] **Step 1: Write the failing tests:** `"an atomic write replaces the content"`; `"a write through a symbolic link changes the file it points to and keeps the link"` (POSIX only, `#ifndef _WIN32`: `is_symlink` still true, target holds the new content); `"a rewritten private file stays private"` (POSIX: create 0600, write with `KeepExisting`, permissions still 0600); `"a write into a missing folder reports an error and leaves no temporary"`.
- [ ] **Step 2: Run** `slic3rutils_tests "[McpAtomicWrite]"`; expected: compile failure.
- [ ] **Step 3: Implement**; switch `InstanceRegistry::write_entry` to `write_file_atomically(target, to_json(identity).dump(2), WritePermissions::OwnerOnly)`, keeping its error text.
- [ ] **Step 4: Run** `slic3rutils_tests "[McpAtomicWrite],[McpClientConfig],[InstanceRegistry]"`; expected: pass.
- [ ] **Step 5: Commit.**

### Task 3: The interpreter, and Connect writing every client

**Files:**
- Create: `src/slic3r/plugin/BundledPython.hpp` (declares `find_bundled_python_home` and `find_python_executable` in `Slic3r`; a header of their own, so MCPClientConfig does not include Python's C headers through PythonInterpreter.hpp)
- Modify: `src/slic3r/plugin/PythonInterpreter.cpp` (the two functions move out of the anonymous namespace into `Slic3r`; bodies unchanged)
- Modify: `MCPClientConfig.hpp/.cpp`, `CLAUDE.md` (carried item CI and its probe)
- Test: `tests/slic3rutils/test_mcp_client_config.cpp`

**Interfaces:**
- Consumes: Task 1's builders, Task 2's writer.
- Produces:
  - `struct InterpreterInputs { std::string bundled; bool in_appimage = false; std::string system_python; int system_major = 0, system_minor = 0; };`
  - `struct InterpreterChoice { std::string path; std::string error; };`
  - `static InterpreterChoice choose_interpreter(const InterpreterInputs& inputs);` -> `bundled` unless empty or `in_appimage`; else `system_python` when it is 3.9 or newer; else the Global Constraints error.
  - `static InterpreterChoice bridge_interpreter();` (app side: bundled = `find_python_executable(find_bundled_python_home())`; `in_appimage` = Linux and `APPIMAGE` set; system Python looked up and run (`python3 -I -c "import sys; print(*sys.version_info[:2])"`, 5 s limit) only when the bundled one is not taken)
  - `static bool write_client_entry(const MCPClientInfo& info, const BridgeCommand& command, std::string& error);` -> JSON: read (refuse an unparsable file), set `servers_key.orca-slicer`, write atomically (`KeepExisting`); TOML: `replace_toml_server_table`, write atomically.
- `connect_client` = copy the bridge, `bridge_interpreter()` (its error is Connect's error), `write_client_entry`. `get_mcp_server_json` = the `"orca-slicer": ...` text of `server_entry` for a client without extra fields (indent 2), from `bridge_interpreter()`; `python3` only when it errors.

- [ ] **Step 1: Write the failing tests:** `"the app's own Python is chosen wherever its path stays put"`; `"inside an AppImage the system's Python 3.9 or newer is chosen"` (3.9 taken, 3.8 refused with the exact error, none refused); `"Connect replaces Codex's old python3 table"` and `"Connect never rewrites a config it cannot parse"` (both through `write_client_entry` on files in a temporary folder; the unparsable JSON file is byte-identical after).
- [ ] **Step 2: Run**; expected: compile failure.
- [ ] **Step 3: Implement**; add carried item CI to CLAUDE.md's probe list:
  `echo "CI upstream's bundled Python finder is file-private, which ours declares in BundledPython.hpp for MCP's Connect (a move: 0 = expected, keep ours) / upstream still ships the runtime (non-zero = expected; 0 = Connect has no Python to name): $(U src/slic3r/plugin/PythonInterpreter.cpp | grep -c 'BundledPython.hpp') / $(U src/CMakeLists.txt | grep -c 'Copying Python runtime into the build tree')"`
  with a paragraph in the items' style saying what each count means.
- [ ] **Step 4: Run** `slic3rutils_tests "[McpClientConfig],[McpAtomicWrite]"`; expected: pass.
- [ ] **Step 5: Commit.**

### Task 4: Our entries kept current at startup

**Files:**
- Modify: `MCPClientConfig.hpp/.cpp`, `src/slic3r/GUI/GUI_App.cpp:1216`
- Test: `tests/slic3rutils/test_mcp_client_config.cpp`

**Interfaces:**
- Produces:
  - `enum class EntryRefresh { NoEntry, NotOurs, Current, Rewritten, Failed };`
  - `static EntryRefresh refresh_client_entry(const MCPClientInfo& info, const BridgeCommand& expected, std::string& error);` -> reads the config (missing file: `NoEntry`; unparsable: `Failed`, untouched), finds `orca-slicer`, `NotOurs` unless `is_our_entry` / `is_our_toml_table`, `Current` when equal to `server_entry` / `toml_server_table`, else `write_client_entry` -> `Rewritten` or `Failed`.
  - `static void refresh_client_entries_at_startup(bool own_data_folder, const std::string& executable);` -> returns at once for a test launch or build tree (log why, as `refresh_shared_bridge_at_startup` does); else `bridge_interpreter()` (an error is logged, nothing written) and `refresh_client_entry` for every client from `get_all_clients()`, logging `Rewritten` and `Failed` at info / warning.
- GUI_App calls it right after `refresh_shared_bridge_at_startup`, same arguments.

- [ ] **Step 1: Write the failing tests** on files in a temporary folder: a Claude Desktop config with the old `python3` entry -> `Rewritten`, other servers and top-level keys kept; the same config again -> `Current` and its bytes unchanged; an `orca-slicer` entry running `./scripts/orcamcp-bridge.py` -> `NotOurs`, unchanged; no file -> `NoEntry`, no file created; a Codex config with the old table -> `Rewritten`.
- [ ] **Step 2: Run**; expected: compile failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `slic3rutils_tests "[McpClientConfig]"`; expected: pass.
- [ ] **Step 5: Commit.**

### Task 5: The bridge on Python 3.9, and the docs

**Files:**
- Modify: `scripts/orcamcp-bridge.py` (add `from __future__ import annotations` after the module docstring), `scripts/tests/test_bridge_environment.py` (or a new `test_bridge_python_version.py`)
- Modify: `README.md`, `docs/setup/configuration.md`, `docs/setup/troubleshooting.md`, `CLAUDE.md` ("Where `~/.orcamcp` comes from")

- [ ] **Step 1: Write the failing test** `test_the_bridge_loads_on_python_3_9`: the source contains `from __future__ import annotations` before its first `def`, with a docstring saying macOS's own Python is 3.9 and Linux systems may be.
- [ ] **Step 2: Run** `python3 -m unittest discover -s scripts/tests -t scripts`; expected: that test fails.
- [ ] **Step 3: Add the import**; check by hand: `echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | /usr/bin/python3 scripts/orcamcp-bridge.py` answers (macOS).
- [ ] **Step 4: Docs:** Connect needs no Python except on the Linux AppImage; entries run the app's Python with `-I`; the app updates its own entries at startup (ours only, by the Global Constraints rule); Codex's timeouts. Run the full Python suite; expected: pass.
- [ ] **Step 5: Commit.**

### Task 6: Live check (main session, not delegated)

- [ ] Build `OrcaSlicer` and `slic3rutils_tests`; run `slic3rutils_tests "[McpClientConfig],[McpAtomicWrite],[InstanceRegistry]"`.
- [ ] Run the dev build's bundled interpreter (`build/arm64/src/RelWithDebInfo/OrcaSlicer.app/Contents/MacOS/python/bin/python3 -I scripts/orcamcp-bridge.py`) with an `initialize` and a `tools/list` on stdin: both answer.
- [ ] The startup refresh is gated off for build trees by design, so its live check is the installed app after the release: launch it once; its log says which client entries it rewrote, and Claude Desktop's and Codex's configs then run the app's Python with `-I`.
