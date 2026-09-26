# 05 — Mesh health: tell the agent what the GUI's warning icon means

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. The server's C++ lives in
`src/slic3r/GUI/OrcaMCP/`. Read CLAUDE.md's "Adding New Tools", "Threading Model" and "Active
Warnings" sections before you start.

This is prompt 05 of the v2.5.0.6-dev release. Prompts 01 and 03 must be merged first:

- **01** adds a single source of truth for tool text: categories, a generated `get_server_info`,
  and a golden tools file.
- **03** makes `with_model_object_features`' schema honest. This prompt fills that field.

## What happened

On 2026-09-26 the user sent a screenshot of OrcaSlicer's object list. It showed an orange warning
triangle next to their object "Kuromi" (1M facets, one part), and they asked why.

The agent had no way to find out. `get_scene_info {with_model_object_features: true}` returned
`features: {}`, and `active_warnings.count` was 0. The agent said "Orca's API doesn't expose the mesh
errors", and went and inspected the mesh in Blender. Afterwards it stated a cause as fact, with no
data from Orca behind it: "Orca patched those same microscopic gaps itself".

A later `get_object_components` showed three shells: 557,446, 499,894 and 104 facets, the last a
stray fragment. The agent had not known that tool could help.

The transcript is `~/.claude/projects/-Users-hanan-Documents-3D-Prints/69f5c2a1-ff9b-43a1-aaac-1c1649d85dbc.jsonl`,
at 05:20:48 to 05:28:06 UTC.

## Facts (verified 2026-09-26; re-check the lines)

**The data exists in the core.**

- `TriangleMeshStats` (`src/libslic3r/TriangleMesh.hpp:47-84`) holds `number_of_facets`, `min`,
  `max`, `size`, `volume`, `number_of_parts` (the shell count) and `open_edges`, plus `manifold()`
  (open_edges == 0) and `repaired()`.
- `RepairedMeshErrors` (`TriangleMesh.hpp:19-45`) holds `edges_fixed`, `degenerate_facets`,
  `facets_removed`, `facets_reversed` and `backwards_edges`.
- Object level: `ModelObject::get_object_stl_stats()`. Per volume: `volume->mesh().stats()`.

**Where the icon and its tooltip come from.**

- `ObjectList::get_mesh_errors_info(obj_idx, vol_idx, wxString* sidebar_info, int* non_manifold_edges)`
  (`src/slic3r/GUI/GUI_ObjectList.hpp:268`).
- `get_warning_icon_name` and `get_mesh_errors_info` (`GUI_ObjectList.cpp:584-630`).
- The icon shows when the mesh is not manifold, or when it was repaired.

**The icon never reaches the MCP.**

- `get_active_warnings` iterates pop-up notifications only (`NotificationManager.cpp:3343-3383`).
- The only MCP use of `TriangleMeshStats` is a hash helper (`OrcaMCPPlateUtils.cpp:~977-993`).
- `GetModelObjectFeaturesJson` returns `{}` (`OrcaMCPPlateUtils.cpp:~970-974`).

**Shells are listed already.** `get_object_components` (`OrcaMCPPaintTools.cpp`) lists each part's
connected shells, with facet count, area and bounding box.

## Your task

1. **New tool `get_mesh_health(object_id)`** (category: Models).
   - Per volume: name and type; facets; shells; open edges; manifold; every `RepairedMeshErrors`
     field; and whether it was repaired.
   - Per object: the same summary, plus the GUI's state. Report whether the warning icon shows, and
     the **exact tooltip text** the GUI would show, built from the same source
     (`get_mesh_errors_info`), so they cannot disagree.
   - When there is more than one shell, include a short shell list (id, facet count, bounding box).
     Point to `get_object_components` for the full list, and share that code; don't duplicate it.
   - If `get_mesh_errors_info` needs the GUI object list, call it on the main thread. Otherwise, a
     pure function over the stats is better, and you can test it without the GUI. Decide in your
     design; a pure core with a thin GUI wrapper is preferred.
   - Say in the description, in its first sentence, what an agent would search for: "mesh errors,
     warning icon, open edges, holes, repaired facets, non-manifold, loose parts, stray shells".
2. **`active_warnings` carries mesh warnings.** Every object that shows the GUI's warning icon adds
   an entry: `type: MeshErrors`, level `warning`, with the object name and the tooltip text. Add it
   where `get_active_warnings` is assembled for MCP responses. Don't change the notification manager.
3. **`get_scene_info`**
   - A per-object `mesh_warning` flag, plus a one-line reason.
   - Fill `with_model_object_features` with the mesh-health summary: volume, shells, manifold,
     repaired. Also add overhang data, if it is cheap and correct; otherwise leave it out and say so
     in the schema.
   - Make the schema text match exactly what is returned.
4. **Tests.** In `tests/slic3rutils/`, build meshes with known defects:
   - a cube with one facet removed (open edges);
   - two disjoint cubes in one volume (two shells);
   - a mesh with a reversed facet, repaired on load.
   Assert the reported numbers and the icon state, and assert that the tooltip text matches
   `get_mesh_errors_info`'s wording for the same stats.
5. **Docs.** Add the new tool to the CLAUDE.md tool table and count, and to
   `docs/tools/reference.md`. Regenerate the golden tools file.

**Out of scope:** the hint in `load_model`'s result, and the cross-reference redirects in other
tools' descriptions. Prompt 08 adds both and needs this tool's final name, so report it.

## How to work

1. **Verify.** Read the code at every file:line above; lines may have moved since 2026-09-26.
   Confirm each fact before building on it. If one is wrong, say so.
2. **Design, then stop.** Present a short design to the user: for each item, the approach, the
   files you'll touch, the tests and the exact response shape. Answer every "decide in your
   design". Then wait for their explicit yes before writing code. If you are a subagent, end your
   turn with the design as your report; you will be resumed with the answer.
3. **Branch.** Create `rel2506/05-mesh-health` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** failing test, change, passing test, commit.
5. **Check live** (see the ground rules), on a test model with a hole and
   a stray shell. Then run the full `slic3rutils` suite and the Python tests.
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

- For a model that shows the GUI's warning icon, `get_mesh_health` reports the icon state, the
  exact tooltip text, and the numbers behind it.
- `active_warnings` lists it.
- `get_scene_info` flags it.
- For a clean model, all three say it is clean.
- The unit tests pass, and fail on a deliberately wrong count.

## Report back

- **Change list** mapped to the task numbers: done, deferred (with reason) or won't fix (with reason).
- **Commits:** hash and subject.
- **Test results:** new tests and full-suite counts.
- **What you checked live.**
- **Bugs found** outside your area.
- **For prompt 08:** the final tool name, and the one-line summary an agent should see.
