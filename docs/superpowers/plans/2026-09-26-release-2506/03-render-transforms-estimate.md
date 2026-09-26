# 03 — Renders that work from any tab, transforms that stay on the bed, honest layer counts

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. The server's C++ lives in
`src/slic3r/GUI/OrcaMCP/`. Claude Code talks to it through a stdio bridge,
`scripts/orcamcp-bridge.py`, which forwards to the app's HTTP server on port 13618. Tool handlers
run GUI work on the main thread through `run_on_main_thread` (CLAUDE.md, "Threading Model").

This is prompt 03 of the v2.5.0.6-dev release. Prompt 01 must be merged first. It adds a single
source of truth for tool text: categories, a generated `get_server_info`, and a golden tools file.

## What happened

These surfaced on 2026-09-26, in a session that took a painted figurine from Blender to a sliced
Creator 5 Pro project. The transcript is
`~/.claude/projects/-Users-hanan-Documents-3D-Prints/69f5c2a1-ff9b-43a1-aaac-1c1649d85dbc.jsonl`;
times below are UTC.

- **Blank renders.** `render_plate_view` returned a blank image after slicing (04:48:42 and
  05:28:06): `uniform_image: true`, `objects_in_frame: []`, hint "plate 0 has no printable volumes;
  nothing to draw". The object was on the plate. **Reproduced live on 2026-09-26:** state "done",
  object fully inside plate 0 and on the bed, and still a blank render with that hint.
- **Clipped fit-to-object renders.** These reported the object as `clipped: true`, with a
  full-frame screen box of [0,0,512,512] (04:36:42, 04:46:40).
- **`scale_object` left the object below the bed.** A uniform scale of 1.4932 about the
  bounding-box centre left the object's feet 24 mm below Z=0 (`on_bed: false`) at 05:25:31. The
  agent had to `move_object` it back up. The GUI's own scale keeps it on the bed.
- **Doubled layer counts.** `get_print_estimate` reported `layer_count` 1567 for a 98.95 mm model at
  0.12 mm, where about 825 were expected, and 2340 at 147.76 mm, where about 1231 were expected. It
  gave the same count with `independent_support_layer_height` on and off. The agent guessed a
  cause, then had to retract it.
- **An empty features field.** `get_scene_info` with `with_model_object_features: true` returned
  `features: {}` (05:21:09), while the schema promises "overhang, volume, etc.".

## Causes (verified 2026-09-26; re-check the lines)

**1. The renderer reads whichever canvas is showing.**

- `OrcaMCPPlateUtils.cpp:495-496` takes its volumes from `plater()->canvas3D()`. That calls
  `get_current_canvas3D()` (`Plater.cpp:20750-20754`), which returns the **preview** canvas while
  the Preview tab shows (`Plater.cpp:13739-13740`).
- In FFF mode the preview canvas holds no model volumes: its shells go to `m_gcode_viewer`
  (`GLCanvas3D.cpp:3073-3080`). So `report.drawn` stays empty, and the hint at
  `OrcaMCPPlateUtils.cpp:~187` fires.
- The GUI's Slice buttons switch to Preview (`Plater.cpp:12890`, `12914`).
- `pick_facet` already uses `get_view3D_canvas3D()` (`OrcaMCPPaintTools.cpp:~292`), and so do
  upstream's thumbnails (`Plater.cpp:13429`).
- While the 3D view is hidden, `reload_scene` is postponed (`GLCanvas3D.cpp:2516`), so its volumes
  can be stale after transforms.
- Turntable previews (`include_preview`, `OrcaMCPPlateUtils.cpp:~1191`) share the bug.
- The filter `is_visible` (lines ~509-520) is also too strict. It needs full containment in the
  plate box, including height, so an object partly off the plate vanishes entirely, and the hint
  then wrongly says "no printable volumes". `plate_idx` is hard-coded to 0 through
  `thumbnail_params` (line ~485).

**2. Transforms never touch Z.**

- `scale_object` scales about the bounding-box centre (`OrcaMCPServer.cpp:~3977`,
  `OrcaMCPCommon.cpp:193-218`).
- `rehome_and_report_placement` (`OrcaMCPCommon.cpp:174-191`) never changes Z.
- The GUI snaps back to the bed in `GLCanvas3D::do_scale` (`GLCanvas3D.cpp:5282-5300`),
  `do_rotate` (`5190`) and `do_mirror` (`5392`), unless the instance was already sinking.

**3. The layer count adds two different counts.**

- `get_print_estimate` reports the maximum over objects of `total_layer_count()`
  (`OrcaMCPServer.cpp:~3134-3137`). That is `layer_count() + support_layer_count()`
  (`Print.hpp:397-400`, commented as "not supposed to be compared").
- Upstream counts distinct print heights, EPSILON-merged, across object and support layers
  (`GCode.cpp:2983-3002`, and 2963-2981 for by-object printing). That is what the G-code's
  `total_layer_count` uses (`GCode.cpp:3398`).

**4. The features field is a stub.**

- `GetModelObjectFeaturesJson` returns `{}` (`OrcaMCPPlateUtils.cpp:~970-974`).
- The schema text is at `OrcaMCPServer.cpp:~907-910`, with a tip at ~854.

## Your task

1. **Render from the 3D view, whatever tab is showing.**
   - In `render_plate_view`, and wherever else `OrcaMCPPlateUtils` takes volumes (including the
     turntable preview), use `get_view3D_canvas3D()`.
   - If the 3D view's reload is delayed (`is_reload_delayed()`), refresh it first with
     `reload_scene(true)`.
   - Leave the user's visible tab untouched.
   - Verify on macOS that a hidden canvas can be made current. Note in code if Linux or Windows
     might differ.
2. **Honest hints and filtering.**
   - Split the hint into "no model volumes on the canvas" and "N volumes on plate P, none in this
     view" (the second one exists already).
   - Draw volumes that are only partly inside the plate, instead of dropping them.
   - Remove the hard-coded plate id, if it matters.
3. **Fit-to-object framing.** Find out why fit-to-object renders report the object `clipped` with a
   full-frame box, and fix the framing. The camera fit is in `OrcaMCPRenderMath.cpp`, and is
   unit-tested in `tests/slic3rutils/test_render_math.cpp`.
4. **Transforms drop to the bed, like the GUI.**
   - Add one helper next to `transform_instances_in_plate_frame` (`OrcaMCPCommon.cpp`). Before a
     transform it records each instance's lowest point and whether it was sinking; afterwards it
     drops non-sinking instances back to Z=0.
   - Use it in `scale_object`, `rotate_object`, `mirror_object` and `transform_objects`.
   - `move_object` stays as it is: an explicit Z move is the user's intent.
   - Report the final `on_bed` state as today.
5. **Real layer counts in `get_print_estimate`.** Replace `layer_count` with:
   - `printed_layers`: distinct print heights, merged by EPSILON the way `GCode.cpp` does;
   - `object_layers`;
   - `support_layers`.
   - Decide in your design whether to keep `layer_count` as a deprecated alias equal to
     `printed_layers`.
   - Update the tool's description.
6. **Stop promising features we don't return.** Make `with_model_object_features`' schema text
   honest. Prompt 05 (mesh health) will fill this field and update the text again.
7. **Render file housekeeping** (found by prompt 01's agent, 2026-09-26).
   - `OrcaMCPPlateUtils.cpp:~151` and `~1320` hard-code `/tmp/` for render and preview files, which
     breaks on Windows. Use the platform temp directory (e.g. `boost::filesystem::temp_directory_path()`)
     in one shared helper.
   - `CleanupPreviews` (`~1080`) never deletes `orcamcp_render_*` PNGs, so they pile up. Clean them
     the same way as the preview files.
8. **Tests.**
   - Render: extend `test_render_math.cpp` for the framing fix.
   - Add a unit test for the volume filter, if you extract it.
   - Live: slice, switch the GUI to Preview, then render. The image must not be uniform.
   - Transforms: `tests/slic3rutils/test_transform_frames.cpp` already builds ModelObjects against
     `OrcaMCPCommon`.
   - Layer count: `tests/slic3rutils/test_slice_estimate.cpp`. Use a tiny sliced object with
     support; expect printed layers ≈ height ÷ layer height.
   - Regenerate the golden tools file for any description or schema you changed.

**Out of scope:**

- mesh health and filling `features` (05);
- the per-feature time breakdown in the estimate (06);
- the any-layer plan (07);
- hints (08).

## How to work

1. **Verify.** Read the code at every file:line above; lines may have moved since 2026-09-26.
   Confirm each cause before fixing it. If a cause is wrong, say so.
2. **Design, then stop.** Present a short design to the user: for each item, the approach, the
   files you'll touch and the tests, plus answers to every "decide in your design". Then wait for
   their explicit yes before writing code. If you are a subagent, end your turn with the design as
   your report; you will be resumed with the answer.
3. **Branch.** Create `rel2506/03-render-transforms-estimate` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** failing test, change, passing test, commit.
5. **Check live** (see the ground rules). Then run the full `slic3rutils`
   suite and the Python tests.
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

- After a slice, with the GUI on the Preview tab, `render_plate_view` (fit plate, and fit object)
  shows the model, and the user's tab is unchanged.
- Fit-to-object frames the whole object, without `clipped`.
- Scale, rotate, mirror and transform keep a resting object on the bed.
- `get_print_estimate` on a supported model reports printed layers ≈ height ÷ layer height, with
  object and support layers split.
- No tool promises data it doesn't return.

## Report back

- **Change list** mapped to the task numbers: done, deferred (with reason) or won't fix (with reason).
- **Commits:** hash and subject.
- **Test results:** new tests and full-suite counts.
- **What you checked live.**
- **Bugs found** outside your area.
- **The cause** of the fit-to-object clipping.
