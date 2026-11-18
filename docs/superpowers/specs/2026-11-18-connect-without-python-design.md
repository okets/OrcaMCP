# Connect without Python: the app's own Python runs the bridge

Date: 2026-11-18. Approved by the user in conversation ("yes to all. go.").

## Why

A normal user should press Connect in Preferences and have their AI app work: Claude Desktop, Codex desktop
(which reads the Codex CLI's `~/.codex/config.toml`), Claude Code, Cursor and the rest. Today it fails without a
word on most machines:

- Every entry Connect writes runs `python3`. The bridge needs Python 3.10 (`str | None` annotations, evaluated at
  import); macOS's own `/usr/bin/python3` is 3.9 and the bridge dies on import, and apps started from the Dock
  (Claude Desktop) see only that one. Windows' python.org installer provides `python` and `py`, never `python3`;
  without any Python, `python3` is Windows' Store placeholder.
- Codex's entry is never updated once written (Connect skips a file that has one), its `startup_timeout_sec` is 5
  (Codex's default is 10) and it leaves `tool_timeout_sec` at Codex's 60 s, under the bridge's own 120 s.
- Connect rewrites JSON configs in place: a crash mid-write corrupts, say, the user's whole Claude Desktop config.

The app already ships a full CPython 3.12 for upstream's plugin system (`deps/python3`, signed with the app): on
macOS `OrcaMCP.app/Contents/MacOS/python` (a link to `Contents/Resources/python`), on Windows `<install>\python`,
on a Linux install tree `lib/python`. Upstream finds it with `find_bundled_python_home` and
`find_python_executable` (`src/slic3r/plugin/PythonInterpreter.cpp`). Checked 2026-11-18: the installed app's
runs the bridge's modules (`ctypes`, `concurrent.futures`, `urllib`, `http.client`) in isolated mode.

## Approaches

1. **The app's own Python runs the Python bridge (chosen).** Hours of work; the bridge and its 2,400 lines of
   tests stay. Costs: the entry names a path inside the install (handled by the startup refresh below), the Linux
   AppImage's path changes every launch (it keeps the system's `python3`), and it depends on upstream keeping
   the runtime (a carried probe).
2. Rewrite the bridge as a native C++ program: two to three days for the same result. Rejected by the user.
3. Package the bridge as a self-contained executable (PyInstaller-style): Windows antivirus false alarms, slower
   start, a Python step in every platform's build. Rejected.
4. A direct Streamable HTTP entry: Claude Desktop cannot use one, and it loses `start_orca`, instance choice,
   the offline tool list and `wait_for_slice`. Not offered by Connect.

## Design

1. **What an entry runs** (`MCPClientConfig`). Command: the absolute path of the app's interpreter. Args:
   `["-I", "<home>/.orcamcp/orcamcp-bridge.py"]` (`-I`: no `PYTHON*` variables, no user site-packages, so a
   user's Python setup cannot break the bridge). The interpreter:
   - macOS, Windows, a Linux install tree: upstream's `find_bundled_python_home` + `find_python_executable`.
   - Linux inside an AppImage (`APPIMAGE` set: the runtime sits under a mount path that changes each launch), or
     no bundled runtime found: `python3` resolved on `PATH` to its absolute path, run once to check it is 3.9 or
     newer. None, or too old: Connect fails with a message saying to install the distribution's `python3`.
   - Upstream's two functions move out of the anonymous namespace and are declared in a header of their own, `plugin/BundledPython.hpp`
     (no other change): carried item CI, with probes for the move and for upstream still shipping the runtime.
2. **The bridge runs on Python 3.9** (`from __future__ import annotations`; nothing else needs 3.10), for Linux
   systems and hand-written setups. A Python test holds the import in place, saying why.
3. **Every client's entry is written, not skipped.** JSON clients already replace `orca-slicer`; Codex's TOML
   now replaces its `[mcp_servers.orca-slicer]` table and any `[mcp_servers.orca-slicer.*]` subtable, keeping
   every other line, comment and server. Strings are TOML basic strings with `\` and `"` escaped (a Windows path,
   a user name with an apostrophe). Codex gets `startup_timeout_sec = 20` and `tool_timeout_sec = 140`
   (`ORCAMCP_TIMEOUT`'s default 120 + 20).
4. **Writes are atomic.** Every config write goes to a temporary file in the same folder, then is renamed over
   the original. A file that does not parse is never rewritten (as today: the error is reported).
5. **The app keeps its own entries current at startup.** Under the gate the bridge copy already uses
   (`bridge_copy_decision`: not a test launch on its own data folder, not a build in a source tree), after
   `~/.orcamcp` is refreshed, each client config holding an entry that is *ours* -- named `orca-slicer`, its args
   naming `~/.orcamcp/orcamcp-bridge.py` -- is rewritten when that entry differs from what Connect would write
   now. An entry pointing anywhere else (a developer's repository script) is never touched, nor is a config that
   has no entry of ours. Each rewrite and each failure is logged; nothing is shown. This repairs every existing
   user's `python3` entry on the first launch of the new version, and an entry after the app moved.
6. **One builder for an entry.** Connect, the startup refresh and the "copy this snippet" text
   (`get_mcp_server_json`) all build it from the same function.

Decisions made apart from the app, so they are unit-tested without it (`tests/slic3rutils/test_mcp_client_config.cpp`):
the entry for each client, the TOML table replacement, whether an entry is ours, whether it is outdated, and the
interpreter choice from its inputs (bundled path, AppImage, system interpreter and its version).

7. **An entry is updated, never replaced** (found in review, 2026-11-19). Connect and the refresh set the
   entry's command and arguments (and a client's required `type` / `tools`); every other field -- `env` with
   `ORCAMCP_TIMEOUT` or `ORCAMCP_PORT`, which the docs tell users to set there, Codex's `env` subtable,
   `enabled` -- is kept, and each Codex timeout is the larger of the user's and ours.

## Non-goals and anti-goals

- Not the repository's own `.mcp.json` (developers have Python), not the MSIX Store package, not a native bridge.
- Never rewrite an entry that is not ours, never leave a half-written client config, never touch client configs
  from a test launch.

## Left to the builder

The exact function names and file split; where the system `python3` version probe lives; the Python test's
wording; the CLAUDE.md, README, `docs/setup/configuration.md` and troubleshooting text, which must say Connect
needs no Python except on the Linux AppImage.
