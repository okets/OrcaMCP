# 06b — Four small slicing and loading fixes found during testing

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. Read CLAUDE.md first,
especially "Tool list", "Threading Model" and "Dialog Suppression". Also read tests/AGENTS.md
before writing tests. Prompts 01–07 are merged: slicing tools, busy states and the bridge's
`wait_for_slice` came in 06, the per-plate helpers in 03, and the dialog-suppression rules in 02 and 04c.

## Your task (each item: verify the cause in code first, then fix test-first)

1. **A stale `invalid` right after a fix.**
   - After a failed validation, upstream's `Plater::priv::reslice` returns early ("process_completed_with_error,
     return directly") until its 0.5 s background timer applies the new settings and clears that marker.
   - So `slice_all` called right after an agent fixes a bad setting reports the OLD `invalid` message.
   - Found live on 2026-09-27: invalid config → `invalid`; fix it → an immediate `slice_all` still says
     `invalid`; 1 s later → `slicing_started`.
   - Approved fix: a fork hook `Plater::apply_pending_background_update()`. If the background timer is
     running, it stops the timer and runs what the timer would have run
     (`update_restart_background_process(false, false)`). `slice_all` calls it before deciding and
     starting.
   - `Plater.cpp` is upstream code, so add a probe line. Letters are in CLAUDE.md's probe block: T is
     reserved for prompt 09, Y for prompt 07b; take the next free one after Y.
   - Live: invalid → fix → immediate `slice_all` gives `slicing_started`, then `wait_for_slice` gives `done`.
2. **Empty plates in a Slice All run.**
   - `get_slicing_status` and `wait_for_slice` report an empty plate as "its slice failed, was
     cancelled, or an edit invalidated it", with outcome `incomplete`.
   - If the selected plate is empty, `state` stays `idle` even though other plates were sliced, so a
     wait for `done` never ends.
   - Empty plates must count as "nothing to slice" (skipped), not failed. A run is `done` when every
     non-empty plate is sliced, and the state must reflect the run, not only the selected plate.
   - Test the decision as a pure function. Live: two plates, one empty, both selections.
3. **Slow `instances_on_plate`.**
   - `OrcaMCPCommon`'s `instances_on_plate` calls `ModelObject::instance_bounding_box`, which is uncached
     and transforms every vertex: about 1.7 s per call at -O0 on a 1M-facet mesh.
   - It is used by `get_scene_info`, 3D-view `fit` and the first-layer footprints.
   - Use a cached exact box per instance. For example, cache it keyed by the instance transform plus the
     object's mesh/volume invalidation, or use upstream's cached convex-hull box if it is exact enough
     for footprints. Justify the choice.
   - Measure before and after on a 1M-facet mesh; generate one, e.g. a subdivided sphere, in your scratch
     folder.
4. **A modal on a failed load.**
   - A `load_model` that FAILS (e.g. an STL that admesh can't parse) leaves an "OrcaMCP error" modal
     open under MCP suppression. 04c's `active_warnings` reported it and `quit_app` closed it, but no
     modal should open.
   - Find the dialog (show_error / MessageDialog in the load path). Capture its text into the
     response's `error_messages`, and return `status: error` with that text.
   - Check the other error dialogs on the load path, and cover them the same way.
   - Test with a deliberately broken STL. An ASCII STL with garbage lines works; admesh reads a binary
     STL with no byte above 127 in its first 128 bytes as ASCII.

## How to work

1. **Verify.** Confirm each cause in the code at the current `mcp`, with file:line.
2. **Design, then stop.** Present a short design per item: cause, fix, files, tests. Then wait for an
   explicit yes. If you are a subagent, end your turn with the design; you'll be resumed.
3. **Branch.** Create `rel2506/06b-slicing-followups` off the latest `mcp`, in the main checkout.
4. **Implement test-first**, in small commits.
5. **Check live** as listed. Then run `[orcamcp]` in random order, the full `slic3rutils` suite
   (excluding `[flashforge-live]`) and Python. Regenerate the
   golden file if tool text changes.
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
- **Scratch space.** Use a scratch folder named after your prompt, e.g. `/private/tmp/claude-501/rel2506-<id>/`.
  Never read from or copy out of another agent's folder. On 2026-09-27 an agent copied 29 stale files out of a
  shared `final/` folder over the working tree; it was caught and undone.
- **Before any live check: check who holds the port.** Run `lsof -nP -iTCP:13618 -sTCP:LISTEN`. If
  ANYTHING listens, even `*:13618` from `/Applications/OrcaMCP.app`, stop and tell the orchestrator. After
  launching, confirm your own pid is the ONLY listener, and that your calls appear in YOUR build's log,
  before any scene-changing call. The user's release binds all interfaces and a dev build binds 127.0.0.1,
  and macOS lets both listen at once. On 2026-09-27 an agent's new_project reached the user's open app
  this way and discarded their scene.
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

- Fixing a setting then calling `slice_all` slices at once.
- A run with an empty plate ends `done`.
- `get_scene_info` on a 1M-facet mesh is fast; report the measurement.
- A failed load returns an error with no modal left open.

## Report back

- Per item: cause (file:line), commit, test, live result.
- Counts.
- The probe letter used.
- Bugs found.
