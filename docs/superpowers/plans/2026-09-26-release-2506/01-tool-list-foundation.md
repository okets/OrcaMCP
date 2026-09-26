# 01 — Tool-list foundation: one source of truth for every tool's text

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. The server's C++ lives in
`src/slic3r/GUI/OrcaMCP/`. Claude Code talks to it through a stdio bridge, `scripts/orcamcp-bridge.py`,
which forwards to the app's HTTP server on port 13618.

This is prompt 01 of the v2.5.0.6-dev release. It goes first because it touches every tool
registration; every later prompt builds on it.

## The problem

There are three copies of the tool list's text, and they drift apart:

1. **The C++ registry.** 80 `register_tool(` calls: 53 in `OrcaMCPServer.cpp`, 11 in
   `OrcaMCPFilamentTools.cpp`, 10 in `OrcaMCPPrinterTools.cpp`, 6 in `OrcaMCPPaintTools.cpp`. The
   live server reports 79 tools. This is the truth.
2. **`get_server_info`'s hand-written catalogue** (`OrcaMCPServer.cpp:407` onwards;
   `tools_by_category` at ~705). It lists 62 of 79 tools. Missing: every filament and colour tool
   (`get_filaments`, `set_object_filament`, `set_mixed_filament`, `delete_mixed_filament`,
   `set_filament_color`, `get/set/auto_calc_flush_volumes`, `get_toolchanger_config`,
   `suggest_color_mix`, `get_color_palette`), the Flashforge tools (`get_printer_status`,
   `printer_control`, `list_printer_files`, `print_printer_file`), `set_gcode_view_type` and
   `set_object_printable`.
   - It also says version `"1.0.0"`, hard-coded in three places (`OrcaMCPServer.cpp:139`, `232`, `418`).
   - Its response is ~24.5 KB (~6k tokens) and all-or-nothing, which discourages agents from calling it.
3. **The bridge's offline list.** `scripts/tools_schema.py` is served while the app is not running.
   Checked against the live app on 2026-09-26, it:
   - is missing `quit_app` and `set_filament_color`;
   - has 6 descriptions that differ (`delete_plate`, `get_object_components`,
     `get_print_estimate`, `render_plate_view`, `send_to_printer`, `set_object_filament`);
   - has 4 schemas that differ.

   On top of that, the bridge defines `start_orca` **twice with different wording**: the offline
   copy at `orcamcp-bridge.py:388` and the one injected into the live list at `:646`.

Why it matters: an agent framework the user runs fingerprints every tool's name and description
when a human approves the connection. When the app starts, the bridge switches from the offline list
to the live one. The fingerprint changes, the framework treats that as the toolset changing after
review, and it disconnects OrcaMCP. A restart while the app runs refuses to reconnect for the same
reason.

**Why the existing guard catches none of this.** `scripts/tests/test_tools_schema.py` has a drift
test, but:

- it skips when the app isn't running, which is always the case in CI;
- it ignores descriptions;
- it is written as bare pytest functions, while CI runs `python3 -m unittest discover -s scripts/tests -t scripts`
  (`.github/workflows/check_profiles.yml:43`), which never collects them.

**The user's rule:** `get_server_info` must **always** match the registered tools, enforced by
tests, not by anyone remembering.

## Your task

1. **Every tool declares a category, and the compiler enforces it.**
   - Add a required category to `ToolDefinition` (`OrcaMCPServer.hpp:26-31`). A registration
     without one must not compile.
   - Use the categories of the tool table in CLAUDE.md: Scene, Models, Transforms, Plates, Config,
     Per-Object, Layer Ranges, Filaments & colour, Painting, Slicing, Visualization, Printers,
     Adaptive, History, Info. An enum with a to-string is fine.
2. **`get_server_info`'s catalogue is generated from the registry at call time.**
   - Give each tool's name, category and a one-line summary: the description's first sentence, or
     an optional explicit `summary` field. Decide which in your design.
   - The version comes from `SoftFever_VERSION` (`libslic3r_version.h`, generated from `version.inc`),
     in all three places.
3. **A compact default.**
   - With no arguments, `get_server_info` returns `quick_start`, the generated catalogue, and an
     index of the other sections with their sizes.
   - A `section` parameter returns one section (concepts, suggested_flows, tool_examples,
     warnings_and_best_practices, the settings sections) or `all`.
   - Target: the default response under ~6 KB. The catalogue stays in the default: with lazily
     loaded tool schemas, it is how an agent discovers what exists.
4. **Bridge-only tools live in the same source.**
   - `start_orca` is served by the bridge, because it launches the app. Today its text lives only in
     Python, twice.
   - Recommended: declare bridge-only tools in C++ too, with name, description, schema, category and
     a handler that returns "handled by the bridge". Keep them out of `tools/list`, but include them
     in `get_server_info` and in the golden file (item 5). The bridge then reads their definitions
     from the golden file, instead of defining text in Python.
   - Prompt 06 adds a second bridge-only tool (`wait_for_slice`), so the mechanism must take more
     than one. Decide in your design; whatever you pick, the text of every tool exists in exactly one place.
5. **A golden tools file, checked by a C++ test that needs no running app.**
   - Replace `scripts/tools_schema.py` with a checked-in JSON file generated from the registry, and
     make the bridge load it. It holds registry tools and bridge-only tools, clearly separated.
   - A Catch2 test in `tests/slic3rutils/` fails when the file and the registry disagree on any
     name, description or schema. The failure tells you how to regenerate, e.g. a documented env var
     that makes the test rewrite the file.
   - The test binary already links `libslic3r_gui`. The registry is static
     (`OrcaMCPServer::init()`, `s_tools`, `handle_tools_list()`).
   - `init()` also calls `OrcaMCPPlateUtils::CleanupPreviews()`. Split registration from that side
     effect if the test needs it.
   - CI runs ctest excluding only the `NotWorking|RequiresApp` labels
     (`scripts/run_unit_tests.sh:24`), so this test must not need the app.
   - **Check first:** is any description or schema built from runtime data (preset names, enums read
     from the preset bundle, `wxGetApp()`)? If so, the golden file cannot match it. Make it static,
     or report it.
   - Retire `scripts/regen_tools_schema.py`, or make it call the same generator.
6. **Bridge tests (unittest, not pytest).**
   - `start_orca` is defined once.
   - The list the bridge serves offline, and the list it serves online (simulate the live response
     with the golden file), are identical in names and descriptions.
   - Convert `scripts/tests/test_tools_schema.py` to `unittest.TestCase`. Its live-server comparison
     may keep skipping offline, but must also compare descriptions.
   - `scripts/tests/test_bridge_schema_refresh.py` covers the bridge's `listChanged` notification;
     keep it passing.
7. **Name check.** A test that every tool name referenced in `get_server_info`'s content resolves to
   a registered or bridge-only tool.
   - Content means structured fields such as `suggested_flows` steps and `tool_examples` keys, and
     the free text.
   - Free text also mentions config keys (`support_type`, `enable_support`), so a naive
     identifier match gives false positives. Design a rule that catches real tool references, e.g.
     check structured fields strictly, and in prose only tokens that match a registered-name shape
     and are not config keys.
   - Prompt 08 extends this to the server instructions and the tool descriptions, so make it
     reusable.
8. **Docs.** Add a short "Tool list" section to CLAUDE.md: where each tool's text lives, how to
   regenerate the golden file, and what the tests enforce. Update the tool-count command in CLAUDE.md
   if a better source now exists.

**Out of scope, owned by other prompts:**

- rewording descriptions or adding hints (08);
- server `instructions` (08);
- new tools (05, 06).

Regenerating the golden file will fix today's drift. That is expected.

## How to work

1. **Verify.** Read the code at every file:line above; lines may have moved since 2026-09-26.
   Confirm each fact before building on it. If one is wrong, say so.
2. **Design, then stop.** Present a short design to the user. For each item: the approach, the
   files you'll touch, and the tests. Answer every "decide in your design". Then wait for their
   explicit yes before writing any code. If you are a subagent, end your turn with the design as
   your report; you will be resumed with the answer.
3. **Branch.** Create `rel2506/01-tool-list-foundation` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** a failing test, the change, the passing test, then a commit.
5. **Check live** (see the ground rules). Then run the full `slic3rutils`
   suite and the Python tests.
6. **Report back** in the format at the end.

## Ground rules (shared by every prompt in this release)

- **Read CLAUDE.md first.** Its instructions override defaults.
- **Where to work.** In the main checkout, on your branch. **Not in a git worktree:** `build/arm64`
  is 31 GB, a worktree forces a full app rebuild, and the disk has about 80 GB free. Only one agent
  builds C++ at a time; the orchestrator sequences you.
- **Build.**
  - `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests` for tests;
    `--target OrcaSlicer` for the app.
  - The tree is stale (it still says 2.5.0.4-dev), so the first build reconfigures and takes longer.
- **Tests.**
  - C++: `build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[your-tag]"`,
    then the whole suite.
  - Python: `python3 -m unittest discover -s scripts/tests -t scripts`.
  - Python tests must be `unittest.TestCase`. C++ tests must not need a running app.
- **The app is yours to drive.** The user's work is saved, so you may quit OrcaSlicer, relaunch
  it, and change its scene freely for testing; no need to ask.
  - Launch your build with `open -a build/arm64/src/RelWithDebInfo/OrcaSlicer.app --env ORCAMCP_SKIP_CLOUD_LOGIN=1`.
    Retry on LaunchServices error -600.
  - Close it with the `quit_app` MCP tool, never AppleScript.
  - Load test files from outside `~/Documents`, `~/Downloads` and `~/Desktop` (macOS privacy
    prompts). A scratch directory under `/tmp/claude-501/` works.
  - Before trusting a result, check the running binary is the one you built: a stale instance can
    answer on port 13618.
- **Test MCP behaviour through the `mcp__orca-slicer__*` tools**, not curl (CLAUDE.md).
- **Never call `send_to_printer`**: on Flashforge it uploads *and starts* the print. Never call
  `printer_control` or `print_printer_file`.
- **Fixtures** are small and synthetic, generated in the test or committed under `tests/data/`.
  Never use or commit the user's model files.
- **Code.**
  - Single responsibility, DRY, small well-named functions. Match the surrounding code's idiom and
    comment density. snake_case functions, PascalCase classes.
  - Stay close to upstream: prefer a small fork-local check over rewriting upstream code.
  - If you change an upstream (non-OrcaMCP) file, add a probe line to CLAUDE.md's "Carried upstream
    fixes" block.
- **Bugs.** Fix every bug you find in your area, including related occurrences; don't park them.
  Bugs outside your area: report them, don't fix them.
- **Commits.**
  - Small, one concern each, subject style `mcp: <what changed, from the user's view>`.
  - Docs-only commits end their subject with `[skip ci]`.
  - End every message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - **Do not push, tag or bump the version**: the orchestrator does that.
- **Docs.** Update CLAUDE.md (tool table, count, and any claim your change makes true or false) and
  `docs/tools/reference.md` in the same commits.

## Done when

- A registration without a category does not compile.
- `get_server_info`'s default response names all registered and bridge-only tools, reports the real
  version, and is under ~6 KB.
- The golden file matches the registry, and the C++ test fails when you change any description
  without regenerating. Show that the test fails first.
- The bridge serves identical names and descriptions offline and online, and `start_orca`'s text
  exists once.
- CI's Python command collects and runs the bridge tests.
- Checked live: offline list, then start the app, then live list, with identical fingerprints.

## Report back

- **Change list**, mapped to the task numbers above. Mark each one done, deferred (with reason) or
  won't fix (with reason).
- **Commits:** hash and subject.
- **Test results:** the new tests, plus full-suite pass/fail counts.
- **What you checked live**, and how.
- **Bugs found** outside your area.
- **Notes for the later prompts:** how to regenerate the golden file, and how to add a bridge-only
  tool. Prompt 06 needs the latter.
