# 06 — Workflow tools: wait for a slice, read only the settings you need, remap paint, explain the estimate

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. The server's C++ lives in
`src/slic3r/GUI/OrcaMCP/`. Claude Code talks to it through a stdio bridge,
`scripts/orcamcp-bridge.py`, which forwards to the app's HTTP server on port 13618. The bridge also
serves its own tools; `start_orca` is one of them. Read CLAUDE.md's "Adding New Tools" and
"Threading Model" sections.

This is prompt 06 of the v2.5.0.6-dev release. Prompts 01 and 03 must be merged first:

- 01 makes one source of truth for tool text, including bridge-only tools. Read its CLAUDE.md
  "Tool list" section for how to add one.
- 03 replaces the estimate's `layer_count` with `printed_layers`, `object_layers` and
  `support_layers`.

Item 1 is Python only, and needs no C++ build. The orchestrator may run it while another prompt
builds.

## What happened

These all come from a session on 2026-09-26. The transcript is
`~/.claude/projects/-Users-hanan-Documents-3D-Prints/69f5c2a1-ff9b-43a1-aaac-1c1649d85dbc.jsonl`.

- **Polling for slices.** The agent polled `get_slicing_status` 13 times across four slices. Its
  harness blocks plain `sleep`, and the status has no progress.
- **Oversized settings reads.** To check 12 support keys, the agent read `get_edited_presets`, which
  returned 25 KB. After a preset swap the same call returned 48 KB, including a 12,470-character
  Bambu start G-code.
- **A paint workaround in Blender.** The Blender 3MF exporter numbers painted colours by sorting
  their hex codes. So a 3-colour model always lands on slots 1–3. To keep slot 1 free for PETG
  support filament, the agent painted a hidden 0.005 mm² triangle in Blender with a colour that
  sorts first. There is no way to say "everything painted with filament 1 becomes filament 3".
- **An unexplained estimate.** After re-importing the model and restoring settings, the estimate
  moved from 17h20m to 17h32m. The agent could not explain why, because the estimate has no
  breakdown.

## Facts (verified 2026-09-26; re-check the lines)

**Slicing status and progress.**

- `get_slicing_status` reports running, done or idle, plus per-plate validity
  (`OrcaMCPServer.cpp:~2973-3024`).
- `PartPlate::get_slicing_percent()` (`PartPlate.hpp:~509`) holds the percentage:
  - updated in `on_slicing_update` (`Plater.cpp:~12431-12432`);
  - 100 or -1 on valid or invalid (`PartPlate.cpp:~3619-3622`);
  - updates are dropped while another UI job runs (`Plater.cpp:~12421`).
- The stage text (`SlicingStatus::text`, `PrintBase.hpp:~450-451`) only goes to
  `notification_manager->set_slicing_progress_percentage` (`Plater.cpp:~12426`), which has no getter
  (`NotificationManager.hpp:~332-342`).

**Timeouts and blocking.**

- The HTTP server calls handlers synchronously on a single I/O thread (`HttpServer.cpp:~114`,
  `~214`). A server-side wait would block every other request.
- The bridge's request timeout is `ORCAMCP_TIMEOUT`, 120 s by default (`orcamcp-bridge.py:~51`).

**Where each piece lives.**

- The print estimate handler is `get_print_estimate` in `OrcaMCPServer.cpp`. It reads the G-code
  processor result. `PrintEstimatedStatistics` has per-mode `roles_times` (time per extrusion role).
  Verify the exact members.
- Colour paint is stored per volume as `TriangleSelector` states, where the state is the filament
  number. `paint_object`, `get_object_paint` and `clear_object_paint` are in `OrcaMCPPaintTools.cpp`.

## Your task

1. **`wait_for_slice(timeout_s)`**: a bridge-only tool (category: Slicing), declared through prompt
   01's mechanism so its text lives in one place.
   - Poll `get_slicing_status` every 1–2 s.
   - Return when every plate that `slice_all` started is done, or on error, or at the timeout.
   - Cap the timeout below `ORCAMCP_TIMEOUT`, with headroom, and say what the cap is.
   - Return the final status, and a `timed_out` flag.
   - Test it with a fake server in `scripts/tests/`, as a unittest.
2. **Progress in `get_slicing_status`.**
   - Per plate `percent`, from `get_slicing_percent`.
   - The current `stage` text, from a small fork-local hook that remembers the last
     `SlicingStatus::text` in `on_slicing_update`, without adding upstream behaviour.
   - Update the description.
3. **Filtered settings reads.**
   - Add `keys: [..]` and `dirty_only: bool` to `get_edited_presets`, or a new `get_config_values(type, keys)`.
     Decide in your design, with the reason.
   - Either way, an agent must be able to read 12 keys for well under 1 KB, and see which values
     differ from the saved preset.
   - Also give a cheap answer to "which presets are selected": printer, process, and each filament
     slot's preset, in well under 1 KB. Found in the orchestrator's acceptance pass on 2026-09-26: the
     only way today is paging `get_presets` lists and reading `is_selected`.
   - Unknown keys return an error naming them. The key validation already used by `apply_config`
     is centralised; reuse it.
4. **Paint remapping.**
   - `remap_paint(object_id, volume_id?, mode: "color", mapping: {"1": 2, "2": 3, "3": 4})` rewrites
     painted states atomically: 1→2 and 2→3 together, not chained.
   - It is one undo step. It returns per-state facet counts before and after.
   - Also add a `paint_object` selection by existing state: `selection: "state", state: N` paints
     every facet currently in state N.
   - Both belong to the Painting category. Validate filament numbers against the printer's slot count.
5. **Estimate breakdown.**
   - `get_print_estimate` gains `time_by_feature`: outer wall, inner wall, infill, support, support
     interface, prime tower, travel, and tool changes (time and count), from the processor's
     per-role times.
   - Keep the existing totals.
   - Say in the description how to use it to compare two slices.
6. **Tests.**
   - Unit tests for the remap logic (a pure function over states) and for key filtering.
   - The bridge test for item 1.
   - Live checks for progress and the breakdown.
   - Regenerate the golden tools file; add the new tools to CLAUDE.md's table and count, and to
     `docs/tools/reference.md`.

**Out of scope:** result hints, e.g. `slice_all` pointing to `wait_for_slice`. Prompt 08 adds those.

## How to work

1. **Verify.** Read the code at every file:line above; lines may have moved since 2026-09-26.
   Confirm each fact before building on it. If one is wrong, say so.
2. **Design, then stop.** Present a short design to the user: for each item, the approach, the
   files you'll touch, the tests and the new schemas. Answer every "decide in your design". Then
   wait for their explicit yes before writing code. If you are a subagent, end your turn with the
   design as your report; you will be resumed with the answer.
3. **Branch.** Create `rel2506/06-workflow-tools` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** failing test, change, passing test, commit. Do item 1 first; it needs
   no C++ build.
5. **Check live** (see the ground rules). Use a small model: slices of
   big plates take 10–18 minutes. Then run the full `slic3rutils` suite and the Python tests.
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

- `slice_all`, then one `wait_for_slice` call, returns done with no polling by the agent.
- `get_slicing_status` shows a moving percentage and stage while slicing.
- Reading 12 support keys costs under 1 KB.
- `remap_paint {"1":2,"2":3,"3":4}` on a 3-colour model frees slot 1, and `undo` restores it.
- The estimate shows time per feature, and the parts add up to the total within rounding.

## Report back

- **Change list** mapped to the task numbers: done, deferred (with reason) or won't fix (with reason).
- **Commits:** hash and subject.
- **Test results:** new tests and full-suite counts.
- **What you checked live.**
- **Bugs found** outside your area.
- **For prompt 08:** the final names and one-line summaries of the new tools and parameters.
