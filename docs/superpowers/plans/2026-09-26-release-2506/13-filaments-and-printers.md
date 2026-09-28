# 13 — Filament slots, a slot's settings, installing printers and filaments, Flashforge controls

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. Read CLAUDE.md first, especially
"Tool list", "Threading Model", "Dialog Suppression" and "Server instructions and next steps". Also read
tests/AGENTS.md before writing tests. Everything before you in this release is merged; read the release
README's status table for what each prompt added.

## Why (the user, 2026-09-28)

"An agent shouldn't request the user to press anything. It should be able to do it itself, unless it is a
security prompt or something similar. We own this fork and I aim for full agentic support. Everything a user
can do, the agent should be able to do as well." In prompt 08's acceptance runs, test agents told the user
to split parts, delete a fragment and merge parts in the GUI, because MCP had no tools for them. A parity
audit (2026-09-28) checked 201 GUI actions: 68 fully covered by MCP, 23 partly, 82 not at all. The user put
the everyday-workflow gaps into this release, in four prompts (11 to 14). This is one of them.

## Your items

1. **Add or delete a physical filament slot** (`Plater.cpp:~5396,~5409`). There is no tool, and
   `match_project_to_printer` says it "does not add filament slots" (`OrcaMCP/OrcaMCPProjectMatch.cpp:~249`),
   so a one-slot project can't become a four-colour print. Deleting a slot re-maps objects and paint;
   "Merge with" (`Plater.cpp:~5467`) raises a native `wxMessageBox` that suppression doesn't catch: handle it
   at its call site.
2. **Edit a chosen slot's filament settings** (the sidebar's Edit, `Plater.cpp:~5502`). `apply_config` with
   type filament writes to whatever preset the Filament tab has open
   (`OrcaMCP/OrcaMCPPresetConfigUtils.cpp:~419`), and nothing points that tab at slot N: add a `slot`
   argument (or the equivalent), and make sure the other slots' presets are untouched.
3. **Add a printer or filament the user hasn't installed** (the Setup Wizard, the sidebar's "Add printer",
   Create printer: `MainFrame.cpp:~2752`, `Plater.cpp:~3316,~12287`). `get_presets` lists only presets
   already installed (`OrcaMCP/OrcaMCPPresetConfigUtils.cpp:~300-327`). This writes to the data folder:
   test only on a data copy, and never install into the user's real data folder.
4. **Flashforge fans (filtration, chamber, cooling), print speed and Z offset.** The Device page already
   builds these commands (`FlashforgeConsoleHandler.cpp:~811,~822`); `printer_control` only has pause,
   resume, cancel, light and temperatures. Reuse the page's command builders (DRY).
   **These act on a real printer.** Unit-test the commands they build; NEVER send them to the printer
   yourself. A live check of item 4 happens only later, with the user present and on their word, through
   the orchestrator.

## Rules for every tool you add (from earlier prompts; keep them)

- **One action, one tool, named like the others** (`split_object`, `delete_volume`, ...), in the category
  whose row it belongs to in CLAUDE.md's tool table. Prefer extending an existing tool with an argument
  (e.g. `volume_id` on `set_object_config`) over a near-duplicate tool.
- **Do what the GUI does, through the same code path** (the Plater / ObjectList / PartPlate calls the GUI's
  menu or button calls), not a re-implementation. If that path opens a dialog, handle it under MCP dialog
  suppression with the answer the call's arguments give (CLAUDE.md, Dialog Suppression); a native dialog
  suppression doesn't catch must be checked at its call site.
- **Undo:** one snapshot per call, taken only before the first real change; a call that changes nothing
  takes none (prompt 06b's rule).
- **Refusals** are the tool's own `{"status":"error","message":...}` before anything changes, and say what
  to send instead. A busy pipeline or a running UI job refuses like the others (07e's `ui_job` messages).
  Jobs (arrange, fill bed, orient) use 07e's wait: the tool answers once its job has been applied.
- **Arguments:** every argument declared in the schema (07c's central check refuses anything else);
  `object_id` is the index (`object_index`), never `internal_id`; `volume_id` as `get_object_info`
  reports it.
- **Answers state facts:** what changed, the resulting objects / volumes / placement fields (07f's
  per-instance placement), and `active_warnings`. No verdicts.
- **next_steps** (08) where a natural next call exists, e.g. after a split, `get_object_components`.
- **Text:** no tool text, hint or next step may send the agent to a GUI button or tell it to ask the user
  to click (except a security prompt).
- **get_server_info's default response has a 6 KB cap** and was near it after 08. Prompt 11 decides how the
  catalogue scales with many more tools (for example: the default lists tool names per category, summaries
  by `section`); prompts 12-14 follow what 11 did. Don't raise the cap to make room.
- **Server instructions** (the text every agent reads first) are the user's: don't change them unless the
  orchestrator relays the user's approval.
- **Upstream files:** a fork fix in an upstream file gets a probe letter in CLAUDE.md's block (next free
  letter; check when you start).

## How to work

Verify every item against the code (entry points, what the GUI does, dialogs on the way), then design,
then stop and present the design to the orchestrator: per item the tool (name, arguments, answer), the code
path it reuses, dialogs and how they're answered, undo, refusals, and the test plan. Wait for a yes. After
that: create branch `rel2506/13-filaments` off the latest `mcp`, implement test-first (a test per tool reachable
without the app, e.g. pure helpers over a Model; the handler's GUI part covered live), check every tool live
as an agent through the bridge (the ground rules below), run the suites (slic3rutils full, [orcamcp] in
several random orders, Python, and libslic3r / fff_print if you touch them), regenerate the golden file,
update CLAUDE.md (tool table and count) and `docs/tools/reference.md`, and report back.

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
- **Before any live check: check who holds the port.** Run `lsof -nP -iTCP:13618 -sTCP:LISTEN`. If
  ANYTHING listens, even `*:13618` from `/Applications/OrcaMCP.app`, stop and tell the orchestrator. After
  launching, confirm your own pid is the ONLY listener, and that your calls appear in YOUR build's log,
  before any scene-changing call. The user's release binds all interfaces and a dev build binds 127.0.0.1,
  and macOS lets both listen at once. On 2026-09-27 an agent's new_project reached the user's open app
  this way and discarded their scene.
- **Never launch the default app, and prove you're on a data copy.** Never `open <file>` or anything else
  that makes macOS start `/Applications/OrcaMCP.app` (that's the user's app, on their real data). Always
  `open -a <absolute path of YOUR build> --env ORCAMCP_SKIP_CLOUD_LOGIN=1 --args --datadir <fresh copy>`,
  and load files through MCP. After launch, your build's startup log must appear under `<copy>/log/`, not
  `~/Library/Application Support/OrcaMCP/log/`; if it doesn't, quit it and stop. (2026-09-27: test builds
  ran on the user's real data dir three times, and a test file opened with the default app launched the
  user's OrcaSlicer.)
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

- An agent can do each item in this prompt through MCP, with the same result as the GUI's own action.
- Each tool follows the rules above, and the suites and the golden file are green.

## Report back

- **Verification:** per item, the GUI path and anything surprising.
- **Tools added or extended,** with their arguments and answers.
- **Tests** and the **live result** per tool.
- **Probe letters,** and any bug found outside your area (not fixed).
