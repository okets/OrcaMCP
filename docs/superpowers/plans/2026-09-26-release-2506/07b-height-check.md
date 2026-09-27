# 07b — The G-code over-height check never fires on non-Bambu printers

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. Read CLAUDE.md first,
especially "Tool list", "Threading Model" and "Dialog Suppression". Also read tests/AGENTS.md
before writing tests. Prompts 01–07 are merged: slicing tools, busy states and the bridge's
`wait_for_slice` came in 06, the per-plate helpers in 03, and the dialog-suppression rules in 02 and 04c.

## The bug (verified by the orchestrator on 2026-09-27; re-check)

- **The writer:** `src/libslic3r/GCode.cpp:~5588` writes `"; Z_HEIGHT: %g"` only when
  `print.is_BBL_printer()`, and `";Z:%g"` otherwise.
- **The reader:** `src/libslic3r/GCode/GCodeProcessor.cpp:~4157` sets `m_print_z` only from
  `" Z_HEIGHT:"`.
- **The result:** on Flashforge, Klipper and every other non-Bambu printer, every move's `print_z` is 0,
  so the post-slice G-code checks never trip:
  - `max_print_z > plate_printable_height` (`GCodeProcessor.cpp:~2837`);
  - the per-extruder `printable_heights` check (`~2878`).
- **Where it shows:** the GUI shows these results (`GLCanvas3D.cpp:~10611`). The pre-slice object-height
  validation is separate and still works.
- **Upstream:** identical (`upstream/main` GCodeProcessor.cpp:4219, GCode.cpp:5638).

## Your task

1. **Audit every reader of `move.print_z` / `m_print_z`** in the processor, the G-code viewer
   (libvgcode convert, layer ranges), `custom_gcode_per_print_z`, `max_print_z_custom`, the CLI, and
   our MCP code (07's layer plan reads G-code moves; check what it uses). List what changes on
   non-Bambu printers once `print_z` is real.
2. **Design the fix.** Parse `;Z:` into `m_print_z` when no ` Z_HEIGHT:` tag is in use (or by printer
   type), with no change for Bambu output.
3. **Tests.**
   - Process G-code text in a unit test: `;Z:` sets print_z; the over-height bit is set when a move goes
     above printable_height on a non-BBL config; Bambu is unchanged.
   - A real non-BBL slice at normal height shows no change in the viewer data you audited.
4. **Probe line Y.** `GCodeProcessor.cpp` is upstream code.
5. **Report** whether the fix should also be offered upstream as an issue. The user decides whether
   to file it.

## How to work

Verify the bug, then design, then stop and present the audit and the design. Wait for a yes. After
that: create branch `rel2506/07b-height-check` off the latest `mcp`, implement test-first, check it
live (on a non-Bambu printer preset with an object or support near max height, the GUI or MCP shows
the over-height error), run the suites, and report back.

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
- **Scratch space.** Use a scratch folder named after your prompt, e.g. `/private/tmp/claude-501/rel2506-<id>/`.
  Never read from or copy out of another agent's folder. On 2026-09-27 an agent copied 29 stale files out of a
  shared `final/` folder over the working tree; it was caught and undone.
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

- On a non-Bambu printer, a toolpath above the printable height sets the G-code check's height error,
  as it does on Bambu printers.
- Nothing else changes for normal slices; the audit shows it.

## Report back

- **Audit:** every reader, and the effect on non-Bambu printers.
- **The fix.**
- **Tests.**
- **Live result.**
- **Upstream-issue recommendation.**
