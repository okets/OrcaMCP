# 09 — Several OrcaMCP instances at once, and an agent can switch between them

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`.

- The HTTP/MCP server is `src/slic3r/GUI/HttpServer.{hpp,cpp}`. It is started from `GUI_App.cpp`'s
  `post_init`, on port 13618.
- Claude Code talks to it through a stdio bridge, `scripts/orcamcp-bridge.py`. The bridge also serves
  its own "bridge-only" tools, such as `start_orca`. Their text lives in the C++ registry and the golden
  tools file; read CLAUDE.md's "Tool list" section.
- The app key and data directory are "OrcaMCP" (CLAUDE.md, "Where the app's data lives").

This is prompt 09 of the v2.5.0.6-dev release, added by the user on 2026-09-26. **Start only after
every other coding task in this release (01–08) is merged.** Present your design to the user and wait
for their yes before writing code.

## Why

"OrcaMCP crashes when a second instance is open." The user confirmed which crash: a second copy
launched while one already runs dies at startup.

Instead of just not crashing, the user wants several instances to work at once, and an agent to be
able to see them and switch between them: "can't we have the MCP have an endpoint that switches
instances?", "also have it list the open file."

## What is known (verify, don't trust)

- **The crash.** It was traced by prompt 04b's design:
  - `IOServer`'s constructor builds the acceptor with asio's endpoint constructor
    (`HttpServer.hpp:~170`), which throws `bind: Address already in use` on the main thread.
  - The call chain is `HttpServer::start` ← `post_init` (`GUI_App.cpp:~1063`) ← EVT_IDLE. Nothing
    catches it: `OnExceptionInMainLoop` → `generic_exception_handle` rethrows, and the app terminates.
  - Upstream OrcaSlicer has the same `start`, `stop` and `IOServer` code.
- **The observed log lines:**

  ```
  [error] …Uncaught exception: bind: Address already in use [system:48 at …/boost/asio/detail/reactive_socket_service.hpp:161:33 in function 'bind']
  [warning] …Error: OrcaSlicer got an unhandled exception: bind: Address already in use …
  ```
- **Windows.** asio sets SO_REUSEADDR by default. On Windows that can let a second process bind the
  held port instead of failing, so two instances would share it. Settle this on Windows CI or with a
  test.
- **Prompt 04b** (merged before you) makes shutdown safe and gives the cloud login its own
  `HttpServer`. It deliberately left the busy-port handling to you.
- **The single-instance option.** OrcaSlicer can pass files to an already-running instance
  (`InstanceCheck*`, the preference "single instance"). Know how it interacts with a second launch
  before you design.
- **Shared data directory.** Two instances on one data directory each rewrite `OrcaMCP.conf` and
  presets when they quit, and the last writer wins. This is upstream behaviour; the design must at
  least not make it worse, and must say what it does about it.

## Your task (design first)

1. **No crash.**
   - A second instance must never terminate because the MCP port is taken.
   - It binds the next free port in a small fixed range, e.g. 13618–13627. If none is free, it runs
     without MCP, logs a warning, and shows a notification naming the cause.
2. **An instance registry.**
   - Each running instance publishes one entry, e.g. `~/.orcamcp/instances/<pid>.json`, or under the
     data directory; decide which in your design. The entry holds:
     - port, pid, version, executable path, data directory, start time;
     - **the open project file**: its display name, full path (empty if unsaved/untitled), and
       whether it has unsaved changes.
   - Keep the project fields current as projects are opened, saved or renamed.
   - Remove the entry on exit.
   - Readers must tolerate stale entries from crashed instances: check the pid is alive and the port
     answers.
3. **Instance identity over HTTP.** `GET /mcp` (and, if useful, a small MCP tool) reports the same
   identity: pid, port, version, executable, and the open file. The bridge must be able to confirm who
   it is talking to.
4. **Bridge-only tools**, declared through the existing mechanism:
   - **`list_instances`:** every live instance with its port, pid, version, open file (name, path,
     unsaved), and which one this bridge is using now.
   - **`select_instance`:** by pid, port, or project name or path. It points this bridge session at
     that instance and confirms by identity.
   - **The default must stay simple.** With one instance nothing changes. With several, the bridge
     keeps the one it was using. On first use it picks 13618, or the only live instance.
   - **Tool list.** Switching must not change the tool list fingerprint when both instances run the
     same build. If they run different builds, the existing `listChanged` notice applies; say what the
     agent sees.
   - **`start_orca`** with instances already running: say what it does and what it reports. It should
     not launch yet another copy unless asked.
5. **Security.** The user decided on 2026-09-26: the server listens on this machine only (127.0.0.1).
   Prompt 04b implements that for 13618. Every fallback port must follow the same rule.
6. **Tests.**
   - C++: the port-fallback decision; the registry write, update and remove as a pure core; and a
     second bind returning cleanly (a test on a free port).
   - Python: the bridge's list and select against fake instances (small local HTTP servers), stale
     entries, and identity confirmation.
   - Live checks:
     - two instances, each with a different project open: `list_instances` shows both files;
       `select_instance` switches; `get_scene_info` confirms which one answers;
     - quit one: it drops off the list;
     - kill one with a signal: its stale entry is ignored.
7. **Docs.**
   - CLAUDE.md: the Architecture section, the tool table and count, the probe lines for upstream code
     changed, and the Known Limitations entry about the shared data directory.
   - `docs/tools/reference.md`.
   - `docs/setup/troubleshooting.md`.
   - Regenerate the golden tools file.

## Ground rules (shared by every prompt in this release)

- **Read CLAUDE.md first.** Its instructions override defaults.
- **Where to work.** In the main checkout, on your branch. **Not in a git worktree:** `build/arm64`
  is 31 GB, a worktree forces a full app rebuild, and the disk has about 80 GB free. Only one agent
  builds C++ at a time; the orchestrator sequences you.
- **Build.** `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests` for tests,
  `--target OrcaSlicer` for the app. If the tree is stale, the first build reconfigures.
- **Tests.**
  - C++: `build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[your-tag]"`,
    then the whole suite.
  - Python: `python3 -m unittest discover -s scripts/tests -t scripts`.
  - Python tests must be `unittest.TestCase`. C++ tests must not need a running app.
- **Tool text.** If you change any tool's name, description or schema, regenerate the golden tools
  file as CLAUDE.md's "Tool list" section describes (added by prompt 01). A test enforces it, and
  `get_server_info` follows automatically.
- **The app is yours to drive.** The user's work is saved, so you may quit OrcaSlicer, relaunch
  it, and change its scene freely for testing; no need to ask.
  - Launch your build with `open -a build/arm64/src/RelWithDebInfo/OrcaSlicer.app --env ORCAMCP_SKIP_CLOUD_LOGIN=1`.
    Retry on LaunchServices error -600.
  - Close it with the `quit_app` MCP tool, never AppleScript.
  - Load test files from outside `~/Documents`, `~/Downloads` and `~/Desktop` (macOS privacy
    prompts). A scratch directory under `/tmp/claude-501/` works.
  - Before trusting a result, check the running binary is the one you built: a stale instance can
    answer on port 13618.
- **Screenshots: allowed for this weekend session only** (user, 2026-09-26; they keep sensitive windows
  minimized). Use them only to check OrcaSlicer's own state. Capture just the OrcaSlicer window, or
  crop to it, when you can. Delete the images once the check is done, and never quote or describe
  other apps' content. Outside this session the default is no screenshots. Prefer MCP renders when
  they answer the question.
- **The dev build is unoptimized.** `RelWithDebInfo` compiles at `-O0` here (upstream
  `CMakeLists.txt:742-750`), so slicing, especially tree supports, runs 20-70x slower than the release.
  For live checks, slice small models without tree support. Never read a slow slice as a stall or a
  regression without first timing the same case on `/Applications/OrcaMCP.app` on a data-dir copy.
- **Test MCP behaviour through the `mcp__orca-slicer__*` tools**, not curl.
- **Never call `send_to_printer`**: on Flashforge it uploads *and starts* the print. Never call
  `printer_control` or `print_printer_file`.
- **Fixtures** are small and synthetic. Never use or commit the user's model files.
- **Don't delete the user's presets.** The user's preset library holds embedded Bambu presets from
  the 2026-09-26 session, e.g. `Bambu Lab A1 0.4 nozzle(Kuromi head.3mf)`. Don't delete them
  without asking.
- **Code.**
  - Single responsibility, DRY, small well-named functions. Match the surrounding code's idiom and
    comment density. snake_case functions, PascalCase classes.
  - Stay close to upstream: prefer a small fork-local check over rewriting upstream code. If you
    change an upstream file, add a probe line to CLAUDE.md's "Carried upstream fixes" block.
- **Bugs.** Fix every bug you find in your area, including related occurrences. Report bugs outside
  your area; don't fix them.
- **Commits.**
  - Small, one concern each, subject style `mcp: <what changed, from the user's view>`.
  - Docs-only commits end their subject with `[skip ci]`.
  - End every message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - **Do not push, tag or bump the version.**
- **Docs.** Update CLAUDE.md and `docs/tools/reference.md` in the same commits.

## Done when

- A second instance starts with MCP on its own port and never crashes.
- An agent can list the running instances, with each one's open file, and switch between them.
- Quitting or crashing an instance never leaves the bridge pointing at a dead one without saying so.
- The tests pass, and they fail on the old code where applicable.

## Report back

- **Design first:** the registry location and format, the port rule, the bridge behaviour, the data
  directory stance, and the security rule applied. Present it, then stop until the user says yes.
- **After implementing:**
  - the change list;
  - commits;
  - test counts;
  - the live-check transcript summary;
  - bugs found.
