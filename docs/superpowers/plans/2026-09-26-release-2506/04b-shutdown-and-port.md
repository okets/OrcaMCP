# 04b — Quitting never deadlocks, and a second instance never crashes

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. The server's C++ lives in
`src/slic3r/GUI/OrcaMCP/`; the HTTP server is `src/slic3r/GUI/HttpServer.{hpp,cpp}`, started and
stopped from `GUI_App.cpp`. Tool handlers do GUI work through `run_on_main_thread`, which blocks the
HTTP thread until the main thread has run the work (CLAUDE.md, "Threading Model").

This is prompt 04b of the v2.5.0.6-dev release, added by the orchestrator on 2026-09-26 after two
bugs showed up during testing. Prompts 01–04 are merged first.

## What happened

**1. `quit_app` deadlocked the app.**

- A test instance was quit with the `quit_app` MCP tool while a script was polling
  `get_slicing_status` about once a second.
- `quit_app` answered `{"status":"quitting"}`, but the process never exited. For 15+ minutes port
  13618 kept listening, every request got no answer, and only a SIGTERM ended it.
- The `sample` of the hung process is saved at
  `/private/tmp/claude-501/accept/sample_quit_deadlock_evidence.txt`:
  - **Main thread:** quit_app's `CallAfter` lambda (in `OrcaMCPServer::register_builtin_tools`) →
    the MainFrame close handler → `MainFrame::shutdown()` → `GUI_App::shutdown()` →
    `GUI_App::stop_http_server()` → `HttpServer::stop()` → `boost::thread::join()`.
  - **HTTP thread:** `HttpServer.cpp:220` `io_context::run` → `session::process_request()`
    (`HttpServer.cpp:114`) → `OrcaMCPServer::handle_request` → `handle_tools_call` → a tool
    handler that waits on the main thread.
- **The cause:** the main thread joins the HTTP thread, while the HTTP thread waits for the main
  thread. Any MCP request in flight when shutdown starts can hang the app forever:
  - `quit_app` alongside parallel tool calls (Claude Code sends those);
  - the user closing the window or pressing Cmd-Q while an agent polls;
  - a watchdog polling.

**2. A second instance crashes at startup.**

- With port 13618 already held by another OrcaSlicer, a freshly built instance died at once with an
  unhandled `bind: Address already in use` exception.
- Prompt 04's agent has the exact output; ask the orchestrator if you need it.
- A user who opens a second window-less copy by accident, or an agent that launches the app twice,
  loses the app instead of getting a clear message.

## Your task

1. **Find the root cause of each bug in code before changing anything.**
   - Read `HttpServer::start/stop`, `GUI_App::start_http_server/stop_http_server`, the shutdown
     order in `GUI_App::shutdown` and `MainFrame::shutdown`, and `run_on_main_thread`, and write
     down exactly how the waits interlock.
   - Check whether upstream OrcaSlicer has the same HttpServer code:
     `git show upstream/main:src/slic3r/GUI/HttpServer.cpp`. If the bug is upstream's, add a probe
     line to CLAUDE.md's "Carried upstream fixes" block.
2. **Shutdown must never wait on something that waits on the main thread.** One possible
   direction, which your design decides:
   - Before stopping the server, mark the MCP layer as shutting down, so `run_on_main_thread`
     refuses new work and wakes every waiter with an error (e.g. "OrcaMCP is shutting down").
   - Stop accepting connections.
   - Then stop the io context and join. Bound the join, so a stuck handler can never hang the
     app's exit.
   - It must hold for both `quit_app` and a GUI quit.
   - An in-flight request should get a proper error response, or a closed connection, not silence.
3. **A busy port is a clear message, not a crash.**
   - Catch the bind failure where the server starts. The app keeps running without the MCP server.
   - Log a warning naming the port and the likely cause (another OrcaSlicer/OrcaMCP is running).
   - If cheap, show a GUI notification too.
   - Check what the bridge's `start_orca` does when the port is held by another instance, and make
     its message say so.
4. **Quitting while a slice is running.**
   - Check that `quit_app` during a long slice exits in bounded time. Use a tree-support slice on
     the dev build: it is -O0, so slices take minutes.
   - If cancelling the slice holds up exit, report how long it takes and whether that is upstream
     behaviour.
5. **Tests.**
   - Unit-test the shutdown and wait logic without the GUI. For example, a fake main-thread queue
     that never runs its work, plus a shutdown signal: the waiter must return an error within a bound.
   - If `HttpServer` can be started on a free port in a test, test that a second bind on the same
     port fails cleanly.
6. **Docs.**
   - CLAUDE.md: the threading and shutdown rules.
   - `docs/setup/troubleshooting.md`: what a second instance now does.

**Out of scope:** everything else. Report other bugs, don't fix them.

## How to work

1. **Verify.** Read the code and the saved sample, and confirm the causes above. If a cause is
   wrong, say so.
2. **Design, then stop.** Present a short design: the root cause of each bug in your words with
   file:line, the approach, the files you'll touch and the tests. Then wait for an explicit yes
   before writing code. If you are a subagent, end your turn with the design as your report; you
   will be resumed with the answer.
3. **Branch.** Create `rel2506/04b-shutdown-and-port` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** failing test, change, passing test, commit.
5. **Check live, both bugs, through the `mcp__orca-slicer__*` tools** (see the ground rules):
   - **Deadlock:** start a background loop that calls `get_slicing_status` about every 0.2 s over
     HTTP. This is instrumentation, not the thing under test. Then call `quit_app`; the app must
     exit within a few seconds.
   - Repeat the deadlock check with a slice running.
   - **Busy port:** launch a second instance while the first holds the port. It must not crash, and
     the log names the cause.

   Then run the full `slic3rutils` suite and the Python tests.
6. **Report back** in the format at the end.

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

- `quit_app`, with MCP requests in flight, exits within a few seconds. A GUI quit behaves the same
  way.
- Quitting during a running slice exits in bounded time, or the report explains the upstream delay.
- A second instance with the port taken keeps running without MCP, logs the cause, and the bridge
  says what happened.
- The new unit tests pass, and fail on the old code.

## Report back

- **Root causes:** each one in your own words, with file:line.
- **Change list:** done, deferred (with reason) or won't fix (with reason).
- **Commits:** hash and subject.
- **Test results:** new tests, plus full-suite counts.
- **Live checks:** the exact steps and timings.
- **Upstream:** whether upstream has the same bugs.
- **Bugs found** outside your area.
