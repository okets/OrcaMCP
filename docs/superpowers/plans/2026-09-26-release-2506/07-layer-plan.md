# 07 — Sliced layer plan: let the agent see any layer's supports, interfaces and tool heads

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. The server's C++ lives in
`src/slic3r/GUI/OrcaMCP/`. Rendering and plans:

- `OrcaMCPPlateUtils.cpp` holds `render_plate_view`.
- `OrcaMCPFirstLayerPlan.cpp` draws the top-down first-layer plan.
- `OrcaMCPRenderOverlay.cpp` draws the 2D overlays.
- `OrcaMCPRenderMath.cpp` holds the pure math, unit-tested in `tests/slic3rutils/test_render_math.cpp`.

This is prompt 07 of the v2.5.0.6-dev release. Prompts 01 and 03 must be merged first:

- **01** adds a single source of truth for tool text: categories, a generated `get_server_info`,
  and a golden tools file.
- **03** fixes `render_plate_view` to read the 3D view from any tab.

## What happened

On 2026-09-26 an agent set up tree supports in PETG, on a 4-head toolchanger, under a three-colour
PLA model. Then the user asked, "is this the best support settings we can have for a smooth print?"

The agent could not see the result:

- `set_gcode_view_type` changes the GUI preview, but nothing captures it.
- `render_plate_view` draws the model, not the toolpaths.
- The only sliced view is `layer_view: "first_layer"`.

The agent answered "about as good as they get", right after a blank render and an unchanged
estimate: advice with no evidence behind it. The transcript is
`~/.claude/projects/-Users-hanan-Documents-3D-Prints/69f5c2a1-ff9b-43a1-aaac-1c1649d85dbc.jsonl`,
at 04:48:16 to 04:49:08 UTC.

## Facts (verified 2026-09-26; re-check the lines)

- `OrcaMCPFirstLayerPlan.cpp` already reads the sliced `Print` directly. For example, it reads
  `support_layers().front()->support_fills` (line ~134) for brim, support and wipe tower. It falls
  back to model footprints when the plate is unsliced.
- `render_plate_view`'s `layer_view` parameter accepts only `"first_layer"`.
- A full 3D toolpath render is a separate, larger job, and it is on the roadmap (`docs/roadmap.md`).
  Don't start it here.

## Your task

1. **Any layer, not just the first.** Generalise the first-layer plan to a layer chosen by index or
   by height.
   - Suggested shape: `layer_view: {layer: N}` or `{z: mm}`, keeping `"first_layer"` working as
     today.
   - Resolve a height to the nearest printed layer, and report which object layer and which
     support layer were drawn. After prompt 03 these are counted separately and can sit at
     different heights.
2. **Filters.**
   - `features`: any of `perimeters`, `infill`, `support`, `support_interface`, `brim`,
     `wipe_tower`. Default: all.
   - `extruders`: a list of tool numbers. Default: all.
   - Colour by feature, or by tool: tools use the slot colours, the way the GUI does.
   - The legend must say which colour means what.
3. **Numbers alongside the picture.** Per drawn layer, return:
   - the height;
   - which tools print on it;
   - the extrusion area by feature, and support area vs object area.

   These let an agent check "does support interface fully cover the overhang at layer 400" without
   guessing from pixels.
4. **Performance.** Build only the requested layer, not the whole print. Say how long it takes for a
   large model; the session's model had 1M facets and about 1230 layers.
5. **Tests.**
   - Unit-test layer selection by index and by height. Include support layers at independent
     heights, and heights between layers.
   - Unit-test the feature and extruder filters, in `tests/slic3rutils/test_first_layer_plan.cpp`
     or next to it.
   - Regenerate the golden tools file, update `docs/tools/reference.md`, and update the
     visualization row in CLAUDE.md.

**Out of scope:**

- 3D toolpath rendering (roadmap);
- window screenshots (roadmap);
- hints (08).

## How to work

1. **Verify.** Read the code at every file:line above; lines may have moved since 2026-09-26.
   Confirm each fact before building on it. If one is wrong, say so.
2. **Design, then stop.** Present a short design to the user: the approach, the files you'll touch,
   the tests, and the exact new parameter shape and response fields. Then wait for their explicit
   yes before writing code. If you are a subagent, end your turn with the design as your report;
   you will be resumed with the answer.
3. **Branch.** Create `rel2506/07-layer-plan` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** failing test, change, passing test, commit.
5. **Check live** (see the ground rules): slice a small model with tree
   supports on two tools, and view a mid-height layer by tool. Then run the full `slic3rutils`
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

- An agent can view layer N of a sliced plate, filtered to support and support interface, coloured
  by tool.
- The response states the height, the tools, and the area by feature.
- `"first_layer"` behaves exactly as before.
- On an unsliced plate, the result says so clearly instead of drawing nothing.

## Report back

- **Change list** mapped to the task numbers: done, deferred (with reason) or won't fix (with reason).
- **Commits:** hash and subject.
- **Test results:** new tests and full-suite counts.
- **What you checked live**, with one example image path.
- **Timing** on a large model.
- **Bugs found** outside your area.
