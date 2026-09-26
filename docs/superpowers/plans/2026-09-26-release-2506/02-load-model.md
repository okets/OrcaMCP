# 02 — `load_model`: always geometry only, and say what happened

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. The server's C++ lives in
`src/slic3r/GUI/OrcaMCP/`. Claude Code talks to it through a stdio bridge,
`scripts/orcamcp-bridge.py`, which forwards to the app's HTTP server on port 13618. Under MCP,
modal dialogs are suppressed and auto-answered. See "Dialog Suppression for Automation" in
CLAUDE.md, and read it.

This is prompt 02 of the v2.5.0.6-dev release. Prompt 01 (the single source of truth for tool text:
categories, a generated `get_server_info`, a golden tools file) must be merged first.

## What happened

On 2026-09-26 an agent imported 3MF files exported from Blender (the ThreeMF_io add-on; the files
carry `Application=BambuStudio-2.3.0` and an embedded Bambu A1 printer, process and filament
preset). The user's printer is a Flashforge Creator 5 Pro, preset "C5P".

Whenever the agent called `load_model` on an **empty** scene:

- the scene was reset;
- the user's unsaved preset edits were discarded;
- the 3MF's embedded presets became active ("Bambu Lab A1 0.4 nozzle(Kuromi head.3mf)",
  "Generic PETG @BBL A1(…)");
- the project was renamed to the 3MF's path.

None of this was reported, apart from one "unsaved preset changes were discarded" line. A
`ValidateError` ("Add G92 E0 to layer_gcode") followed, because the A1 preset was active. Later a
plain `save_project {}` overwrote the user's Blender export, `Kuromi one piece.3mf`, with the Orca
project, because of that silent rename.

On a **non-empty** scene the same call imported geometry only, as CLAUDE.md promises for every
case.

Evidence:

- Transcript: `~/.claude/projects/-Users-hanan-Documents-3D-Prints/69f5c2a1-ff9b-43a1-aaac-1c1649d85dbc.jsonl`,
  UTC. The `load_model` calls are at 04:28:26, 04:29:04, 04:29:46, 04:29:47, 04:36:10, 04:36:32,
  04:45:55 and 05:25:13.
- App log: `~/Library/Application Support/OrcaMCP/log/debug_Fri_Sep_25_03_16_02_39172.log.0`, UTC+7.
  It shows six `validate_presets … not inherit from system` lines, one per empty-scene load.

**Not caused by `load_model`**, so don't chase it: the gap report said the first load "removed two
objects already on the plate". It didn't. Those objects still resolved five seconds after the load,
and vanished later with no MCP call in between, probably deleted in the GUI.

## Causes (verified 2026-09-26; re-check the lines)

**1. The project-vs-geometry decision.**

- `load_model` calls `Plater::load_files(wxArrayString)` (`OrcaMCPServer.cpp:~2931`,
  `Plater.cpp:17380`). A single 3MF goes to `open_3mf_file` (`Plater.cpp:17488`).
- `open_3mf_file` computes `not_empty_plate` across all plates (`17588`). It passes an "always ask"
  override only when the model is non-empty and the setting is `ask_when_relevant` (`17590`). The
  user's setting is `"project_load_behaviour": "ask_when_relevant"`
  (`~/Library/Application Support/OrcaMCP/OrcaMCP.conf`).
- `determine_load_type` (`17542-17579`) checks MCP suppression **only inside the `ALWAYS_ASK`
  branch** (`17558-17562`). With `ask_when_relevant` and an empty model, or with `load_all` in any
  case, control falls through to `return LoadType::OpenProject` (`17577`).

**2. What `OpenProject` does.** It runs `load_project(path, "<loadall>")` (`17597`), which:

- calls `close_with_confirm` (`15290`; its MCP branch answers No, `17815-17821`);
- discards preset edits through the suppressed `UnsavedChangesDialog` (`UnsavedChangesDialog.cpp:836`);
- calls `reset()` (`15335`);
- loads with LoadModel|LoadConfig (`15318`), applying the embedded presets (`8817`, `8991`, `9103`)
  and setting `is_project_file = true` (`9108`);
- calls `set_project_filename(filename)` (`15354`), which is the silent rename.

**3. The geometry path.** `LoadGeometry` takes a snapshot and loads with LoadModel only
(`17600-17603`), appending objects.

**4. Prompts answered silently.**

- The multipart question, "several objects positioned at multiple heights… load as a single object
  with multiple parts?", is at `Plater.cpp:9386`. It is gated by `!is_project_file &&
  model.looks_like_multipart_object()` (`Model.cpp:830-846`), and auto-answered **Yes**
  (`MsgDialog.cpp:94-95`).
  - Effect: a geometry load merges the objects into one, named after the file stem (`Model.cpp:864`);
    a project load keeps them separate. The same file therefore came in merged once and separate
    once.
- The too-large prompt (`9772-9779`, ratio from X/Y only, `9759`) was auto-answered Yes and scaled
  the model by 0.00328. The first Blender export was 1000× too big (`global_scale=1.0`); the answer
  was captured, but never shown as an answer.
- `MsgDialog.cpp:87-103` captures the message text, but not the answer given.

## Your task

1. **`load_model` always imports geometry.**
   - Move the existing MCP-suppression block in `determine_load_type` above the setting checks, so
     every `load_model` of a 3MF takes `LoadGeometry`, whatever the setting or scene state.
   - MCP `load_project` passes `"<silence>"` (`OrcaMCPServer.cpp:~3229`) and never reaches this
     function; that block's own comment says so. Keep `load_project` exactly as it is: it is the
     tool for opening a 3MF as a project.
   - This moves a fork block rather than adding upstream logic. Add a probe line to CLAUDE.md's
     "Carried upstream fixes" block if the edited lines are upstream code.
2. **`load_model` reports what it loaded.** Add `loaded_objects` to the response: for each new
   object, its id, name, scale, size in mm and volume count. Compute it by comparing the object list
   before and after the load; after item 1 every path appends.
   - If the project name ever changes, report `project_renamed_to`. After item 1 it should never
     change: assert that in a test.
3. **Every suppressed prompt reports its answer.** Append the automatic answer to each captured
   message, e.g. `"… (auto-answered Yes)"`.
   - This is in `MsgDialog`'s suppression path, so it applies to every tool, not just `load_model`.
   - Check other suppression sites that capture text (UnsavedChangesDialog, close_with_confirm,
     StepMeshDialog, FileArchiveDialog) and make them consistent.
4. **Decide in your design:** should `load_model` take explicit `multipart` (`merge` | `separate`)
   and `auto_scale` (bool) parameters?
   - With the answer echoed (item 3) and `loaded_objects` (item 2), an agent can already see a
     merge or a scale and undo it.
   - Recommend one way, with the reason. If you add them, the defaults must keep today's GUI-matching
     behaviour.
5. **Don't block startup on a privacy prompt** (found by prompt 01's agent, 2026-09-26).
   - `FileHistory::LoadThumbnails` opens each recent project's 3MF on the main thread during init. A
     freshly built binary whose recent projects live in `~/Documents` blocks on the macOS privacy
     prompt before the MCP server exists, so `start_orca` times out on any unattended launch.
   - Make the thumbnail load lazy or asynchronous, or skip it when the app is launched for an agent
     (`ORCAMCP_SKIP_CLOUD_LOGIN` is already set in that case). Prefer the smallest fork-local change
     and add a probe line if you touch upstream code.
   - Check live: a fresh build with a recent project in `~/Documents` starts and answers MCP
     without anyone clicking.
6. **Docs.**
   - Fix CLAUDE.md's Dialog Suppression table. The `ProjectDropDialog` row is only true after your
     change.
   - Describe the new response fields in `docs/tools/reference.md`.
   - Note in `docs/setup/troubleshooting.md`: a tool call the client shows as "rejected" may already
     have executed if it was sent in parallel before the user interrupted. The 04:29:04 load did.
7. **Tests.**
   - Extract the load-type decision into a small pure function, and test it for each setting
     (`load_all`, `ask_when_relevant`, `always_ask`, `load_geometry`) × empty/non-empty × MCP on/off.
     Put it in `tests/slic3rutils/`, following `test_render_math.cpp`'s pattern.
   - Use a small synthetic 3MF with BambuStudio metadata and an embedded non-system printer preset
     to check live that presets and the project name are unchanged. Generate it or commit a tiny one
     under `tests/data/`.

**Out of scope:** description rewording and result hints (08). Mesh-health hints in `load_model`'s
response come in 08 as well; just make `loaded_objects` easy to extend.

## How to work

1. **Verify.** Read the code at every file:line above; lines may have moved since 2026-09-26.
   Confirm each cause before fixing it. If a cause is wrong, say so.
2. **Design, then stop.** Present a short design to the user. For each item: the approach, the
   files you'll touch and the tests. Answer every "decide in your design". Then wait for their
   explicit yes before writing any code. If you are a subagent, end your turn with the design as
   your report; you will be resumed with the answer.
3. **Branch.** Create `rel2506/02-load-model` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** failing test, change, passing test, commit.
5. **Check live** (see the ground rules). Then run the full
   `slic3rutils` suite and the Python tests.
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

- Loading the synthetic Bambu 3MF onto an empty scene, with each `project_load_behaviour` setting,
  leaves the active printer, filament and process presets, their unsaved edits and the project name
  unchanged. The objects are added.
- The same load onto a non-empty scene behaves identically.
- `load_project` still opens a 3MF as a full project.
- Every suppressed Yes/No prompt's captured message states the answer.
- `load_model` returns `loaded_objects`. A 1000×-too-large file visibly reports its auto-scale.
- The unit tests for the load-type decision pass. Show that they fail on the old code.

## Report back

- **Change list** mapped to the task numbers: done, deferred (with reason) or won't fix (with reason).
- **Commits:** hash and subject.
- **Test results:** new tests and full-suite counts.
- **What you checked live.**
- **Bugs found** outside your area.
- **Your decision on item 4**, and why.
