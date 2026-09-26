# 04c — Two crashes when quitting (or starting a new project) around a slice

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. Under MCP, modal dialogs are
suppressed and auto-answered; read CLAUDE.md, "Dialog Suppression for Automation". Tool handlers do
GUI work through `run_on_main_thread`; read CLAUDE.md, "Threading Model", as updated by prompt 04b.

This is prompt 04c of the v2.5.0.6-dev release. It was added by the orchestrator on 2026-09-26 after
prompt 04b's live testing found two crashes. **Both reproduce on the installed release
`/Applications/OrcaMCP.app` (v2.5.0.5-dev)**, so they predate this release's changes. Prompt 04b is
merged before you build.

## The crashes (as reported by 04b's agent; verify)

1. **Quit, or `new_project`, while a tree-support slice is running crashes the app.** It is a
   SIGSEGV on the slicing thread, on both the dev build and the release.
   - 04b's reading: upstream's `Plater::priv::reset` frees the plates' prints
     (`partplate_list.reinit()`) before stopping the slicing thread, and upstream has the same order.
   - `new_project` during a slice can therefore lose the user's session.
   - 04b added a "wait for the slice first" note to CLAUDE.md and `docs/tools/reference.md` as a
     stopgap; remove or adjust it once this is fixed.
2. **Quit after a support `apply_config` plus a slice crashes on the way out.**
   - It is a SIGABRT, "pointer being freed was not allocated", in `~Plater` → `DestroyChildren` →
     `~MessageDialog`.
   - It reproduces on the unmodified release. An idle quit doesn't crash.
   - Suspect, to confirm or refute: a message dialog object whose ownership is wrong, e.g. one created
     under MCP dialog suppression (`MsgDialog::ShowModal`'s suppression path, or the DPIDialog
     fallback from prompt 02), or a notification dialog parented to the plater and freed twice. Check
     whether upstream crashes the same way, by reasoning from `git show upstream/main:` code: there is
     no upstream binary here.

Crash reports may be in `~/Library/Logs/DiagnosticReports/` (OrcaSlicer-*.ips). Read them for the
exact stacks.

**The dev build is -O0**, so tree-support slices take minutes, which makes crash 1 easy to hit on
purpose. A small tree-support part: `/private/tmp/claude-501/orcamcp-03-fixtures/tall_cap.stl`
(70 mm), or make your own.

## Your task

1. **Reproduce both crashes** on the dev build (data-dir copy, through the `mcp__orca-slicer__*`
   tools), and read the crash reports. For crash 1, try both `quit_app` and `new_project`; also try
   `load_project` during a slice.
2. **Find the root cause of each**, with file:line and the full call chain, and say whether upstream
   has it.
3. **Fix each at its source.**
   - For crash 1: stop and join the background slicing process, and make sure it has let go of the
     print, before anything frees prints or plates, on every path that resets or tears down the plater
     (quit, new_project, load_project).
   - Prefer upstream's own stop/cancel APIs, and a small fork-local ordering change.
   - Add probe lines to CLAUDE.md's "Carried upstream fixes" block for upstream code you change.
     Letters M and N are taken by 04b; start at O. Prompt 09 then continues after yours; tell the
     orchestrator the last letter you used.
4. **Tests.** Crash 1 needs a real slice. If a headless test can drive `Print::process` on a
   background thread and then reset, do it; otherwise explain why not and rely on the live check.
   Crash 2: a unit test of the dialog ownership rule if the cause is in our suppression code.
5. **Docs.** Remove the "wait for the slice first" stopgap if it's no longer needed, and update CLAUDE.md.

## How to work

1. **Verify.** Reproduce and read the crash reports before reading fixes into the code.
2. **Design, then stop.** Present the root causes, with file:line and stacks, the fix per crash, the
   files and the tests. Wait for an explicit yes before writing code. If you are a subagent, end your
   turn with the design as your report; you'll be resumed with the answer.
3. **Branch.** Create `rel2506/04c-quit-crashes` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** failing test where possible, change, passing test, commit.
5. **Check live.** For each variant of crash 1, and for crash 2's sequence, show no crash and a clean
   exit or new project, three times each. Then run the full `slic3rutils` suite and the Python tests.
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

- `quit_app`, `new_project` and `load_project` during a tree-support slice never crash; they cancel
  the slice and proceed.
- Quitting after apply_config plus a slice exits cleanly.
- Each is shown three times live; tests where feasible.

## Report back

- **Root causes:** file:line, stacks, and whether upstream has them.
- **Change list, commits, test counts.**
- **Live results:** three runs per variant.
- **Probe letters used.**
- **Bugs found** outside your area.
