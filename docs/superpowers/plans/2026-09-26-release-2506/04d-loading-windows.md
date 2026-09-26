# 04d — A "Loading..." window is left behind after every file load

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`.

This is prompt 04d of the v2.5.0.6-dev release, added by the orchestrator on 2026-09-27. Prompt
04c's agent found it during live checks and diagnosed it read-only.

## What is known (verify)

- Each file load leaves one small, visible "Loading..." progress window next to the main window.
  After six loads there were six.
- It is created in `Plater::priv::load_files`, in `src/slic3r/GUI/Plater.cpp`: a progress dialog
  shown while loading.
- It happens on the installed release `/Applications/OrcaMCP.app` (v2.5.0.5-dev), and also when
  opening a file from Finder. So it predates this release and is not MCP-specific.
- The dialog code is identical to upstream.
- 04c's unconfirmed guess: the way wx 3.3 on macOS closes app-modal progress dialogs, e.g. a
  `wxProgressDialog` destroyed or hidden without its window actually closing, or one kept alive by a
  reference.
- Also check the MCP path: under MCP dialog suppression, is the progress dialog created and then
  never updated or finished? The suppression may skip the code that normally closes it.

## Your task

1. **Reproduce and measure.**
   - On the dev build with a data-dir copy: load a small model through `load_model` several times,
     and count the leftover windows. Screenshots of OrcaSlicer's own window are allowed this session.
     You can also count windows from outside, e.g. `osascript -e 'tell application "System Events"
     to count windows of process "OrcaSlicer"'` if permitted; Accessibility may be denied.
   - Do the same on `/Applications/OrcaMCP.app` with its own data-dir copy.
   - Find out whether every load leaks one, or only some kinds (3MF, STL, project).
2. **Find the root cause in code.** Trace the progress dialog's lifetime in `load_files`, and every
   path that should destroy or close it, including exception and early-return paths and the MCP
   suppression path. Compare with `git show upstream/main:src/slic3r/GUI/Plater.cpp`, and with the
   wx 3.3 progress dialog implementation on macOS in `deps/build/.../include/wx-3.3/`.
3. **Fix it at the source,** with the smallest change. Probably the dialog's lifetime or ownership
   (scoped object, explicit `Destroy()`, or the right close call). Add a probe line if you change
   upstream code: check CLAUDE.md's probe block for the next free letter. T onward is reserved for
   prompt 09, so ask the orchestrator for a letter.
4. **Tests.** If the lifetime rule can be pulled into a testable helper, test it. Otherwise the live
   window count is the check.

## How to work

1. **Verify.** Reproduce first.
2. **Design, then stop.** Present the root cause (file:line), the fix and how you'll check it. Wait
   for an explicit yes. If you are a subagent, end your turn with the design; you'll be resumed.
3. **Branch.** Create `rel2506/04d-loading-windows` off the latest `mcp`, in the main checkout.
4. **Implement,** test-first where possible, and commit.
5. **Check live.** Ten loads leave zero extra windows, on the dev build. Then run the full suite and
   the Python tests.
6. **Report back.**

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

- Repeated loads leave no stray "Loading..." windows, through MCP and through the GUI.
- The cause is explained with file:line, including whether upstream has it.

## Report back

- **Root cause.**
- **Change list and commits.**
- **Window counts, before and after, per load type.**
- **Test counts.**
- **Bugs found.**
