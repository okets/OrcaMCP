# 08 — Discoverability: server instructions, description hints, result hints

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. The server's C++ lives in
`src/slic3r/GUI/OrcaMCP/`. Claude Code talks to it through a stdio bridge,
`scripts/orcamcp-bridge.py`, which forwards to the app's HTTP server on port 13618. While the app is
not running, the bridge answers `initialize` and `tools/list` itself, from the golden tools file.

This is prompt 08, the last of the v2.5.0.6-dev release. Prompts 01 to 07 must be merged first,
because your text names their tools. Read their merged results:

- CLAUDE.md's tool table;
- CLAUDE.md's "Tool list" section, which says where tool text lives and how to regenerate the golden
  file;
- `get_server_info`.

## Why agents don't find the tools

Claude Code loads MCP tool schemas lazily. An agent sees only the tool **names**, until it searches
for a tool and loads it. It sees a tool's description only after it has already chosen that tool.
The one text it sees every session before loading anything is the server's `instructions`, from
the `initialize` result. **OrcaMCP sends none.**

In the session of 2026-09-26 (transcript
`~/.claude/projects/-Users-hanan-Documents-3D-Prints/69f5c2a1-ff9b-43a1-aaac-1c1649d85dbc.jsonl`):

- All 12 ToolSearch calls were exact-name `select:` queries. None were keyword searches.
- `get_server_info` was loaded at 04:28:05, but first called at 05:27:54, after the user pushed back.
- The agent diagnosed stray mesh fragments in Blender, saying "Orca's API doesn't expose the mesh
  errors". It first loaded `get_object_components` at 05:28:02, after the user's pushback.
- `paint_object` was never loaded. The agent worked around it in Blender.
- The agent told the user "The icon itself is on screen, which I can't see from here", and "nothing
  renders the toolpaths", after loading `get_preview_base64` without calling it.
- The user's verdict: "the fact that there are tools you had to actively search for only if I push
  back is a concern I would like to address."

**Two constraints on the fix:**

- Claude Code truncates long server instructions. On 2026-09-26 another server's instructions were
  shown cut off with "… [truncated]". Keep them to about 300 words, most important lines first.
- Hints only help if they name real tools. Prompt 01 built a name-check test; extend it to
  everything you add.

## Your task

1. **Server instructions.**
   - Return an `instructions` string in the `initialize` result, from the C++ server
     (`handle_initialize`, `OrcaMCPServer.cpp:~221`).
   - Return the identical string from the bridge's own offline `initialize`, read from the golden
     tools file. Extend prompt 01's generator and test so the two cannot differ.
   - Content, about 300 words:
     - One line per capability area, naming its key tools. The areas are: scene and projects, models
       and transforms, mesh inspection (`get_mesh_health`, `get_object_components`), painting
       (colour, support, seam, fuzzy skin), presets and config (including filtered reads),
       filaments and colour, slicing (`slice_all`, `wait_for_slice`, `get_print_estimate`),
       seeing results (`render_plate_view`, the layer plan), and printers.
     - The canonical workflow: load → inspect (mesh health, components) → configure → slice → wait
       → estimate → render → save. Plus re-import without losing settings: `load_model` keeps
       presets; `load_project` replaces them.
     - The footguns:
       - `send_to_printer` starts the print on Flashforge;
       - `save_project` without a path overwrites the file the project is named after;
       - `load_project` replaces the scene and presets.
     - "Call get_server_info for the full catalogue and guides."
   - Take the tool names from the merged code, not from this prompt.
2. **Description keywords and redirects.**
   - Every tool's first sentence says what it reveals or does, in the words an agent would search
     with. For example, `get_object_components` should mention loose parts, stray shells and mesh
     fragments.
   - Add one-sentence redirects for **confusable pairs only**, after the tool's own purpose, e.g.
     "This does not report mesh errors; use get_mesh_health." Candidates to review:
     - `get_object_info` vs `get_object_components` vs `get_mesh_health`;
     - `set_object_filament` vs `paint_object`;
     - `apply_config` vs `set_filament_color` vs `select_preset`;
     - `get_edited_presets` vs the filtered read from 06;
     - `render_plate_view` vs `get_preview_base64` vs the layer plan;
     - `slice_all` vs `get_slicing_status` vs `wait_for_slice`;
     - `load_model` vs `load_project`;
     - `export_3mf` vs `save_project`.
   - Update the one-line `summary` fields (added by prompt 01) wherever your changes make them stale.
     Known: `get_slicing_status` says "poll this"; after prompt 06 it should point to `wait_for_slice`.
   - Don't link everything to everything. With 80+ tools that becomes noise, and it costs tokens on
     clients that load every schema up front.
3. **Result hints.**
   - Add structured next-step hints to responses where the result implies a follow-up, e.g.
     `hints: [{"tool": "get_object_components", "why": "3 shells"}]`. Structured, so the name check
     can verify every `tool` field.
   - Candidates:
     - `load_model`: more than one shell in a loaded object → `get_object_components`; mesh repaired
       or open edges → `get_mesh_health`.
     - `slice_all` → `wait_for_slice`.
     - `get_scene_info`: objects with `mesh_warning` → `get_mesh_health`.
     - `render_plate_view`, when it returns a uniform image: say why, and what to do.
   - Decide the exact field name and shape in your design. One shape everywhere, built by one
     helper.
   - From the orchestrator's acceptance pass (2026-09-26), as an agent reading `get_server_info`:
     - `quick_start.first_steps` never mentions inspecting the model (mesh health, components).
     - `get_object_components`' summary, "List a part's connected mesh shells", would not match an
       agent looking for stray fragments or loose parts. Summaries are searchable text too.
   - Naming trap found in the acceptance pass (2026-09-26): `get_scene_info` and `load_model`'s
     `loaded_objects` carry both `"id": "71"` (an internal ObjectID) and `"object_index": 0`, while
     every tool's parameter is called `object_id` and means the index. An agent can easily pass 71.
     Decide in your design how to remove the trap (rename or drop `id`, or say it in the descriptions),
     keeping existing readers of `object_index` working.
4. **Extend the name check** (from prompt 01) to cover the instructions, every description and every
   hint.
5. **The acceptance test, with a fresh agent.**
   - Launch the built app and load a small synthetic model that has a hole, a
     stray shell and overhangs that need support.
   - Start a fresh agent: a new Claude Code session in a scratch directory with this repo's
     `.mcp.json`, or a subagent with no project context. Give it only "check this model for
     problems and set up supports".
   - Pass if it uses the mesh-health or component tool, and considers support painting or support
     settings, **without being told those tools exist**.
   - Run it three times; agents vary. Report each run's tool calls.
6. **Docs.** In `docs/setup/troubleshooting.md`, add: an agent session that started before OrcaMCP
   was upgraded keeps the tool schemas it loaded at start (seen 2026-09-26: `load_model` showed no
   `multipart`), so reconnect the MCP server (`/mcp` in Claude Code) after an upgrade.
   Also: Add a short section to CLAUDE.md on where the instructions live and how the hints
   work. Update `docs/tools/reference.md` for changed descriptions. Regenerate the golden tools file.

**Out of scope:**

- Pointing and the window screenshot: next release, see `docs/roadmap.md`. Don't mention pointing
  in the instructions yet.
- New tools.

## How to work

1. **Verify.** Read the merged code for every tool you'll name; the names in this prompt are
   provisional. Confirm the facts above still hold.
2. **Design, then stop.** Present to the user:
   - the full draft instructions text, verbatim, with its word count;
   - the list of description changes and redirects;
   - the hint shape and where hints go;
   - the acceptance-test setup.

   Then wait for their explicit yes before changing code. If you are a subagent, end your turn with
   the design as your report; you will be resumed with the answer.
3. **Branch.** Create `rel2506/08-instructions-and-hints` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** failing test, change, passing test, commit.
5. **Check live and run the acceptance test** (see the ground rules).
   Then run the full `slic3rutils` suite and the Python tests.
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

- `initialize` returns the same `instructions`, with the app running and without it.
- The name check covers the instructions, the descriptions and the hints, and fails on a misspelt
  tool name.
- The confusable pairs carry redirects.
- `load_model`, `slice_all` and `get_scene_info` return structured hints where they apply.
- The fresh-agent acceptance test passes in at least two of three runs. Report all three.

## Report back

- **Change list** mapped to the task numbers: done, deferred (with reason) or won't fix (with reason).
- **The final instructions text**, verbatim, with its word count.
- **Commits:** hash and subject.
- **Test results:** new tests and full-suite counts.
- **The three acceptance runs:** the tool calls in order, and pass or fail for each.
- **Bugs found** outside your area.
