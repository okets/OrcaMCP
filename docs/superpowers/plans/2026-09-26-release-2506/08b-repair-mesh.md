# 08b — An agent can repair a mesh, as the object list's Repair does

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. Read CLAUDE.md first,
especially "Tool list", "Threading Model", "Dialog Suppression" and "Active Warnings". Also read
tests/AGENTS.md before writing tests. Prompts 01–08 of this release are merged: `get_mesh_health` came in
05, the per-call undo rules in 06b, the argument check in 07c, the job waits in 07e, and the server
instructions and `next_steps` pointers in 08.

## Why (the user, 2026-09-28)

The user clicked Repair on a mesh warning in OrcaSlicer on macOS and it worked. An agent had told them
repair "only works on Windows". That was PrusaSlicer's Netfabb / Win10 SDK repair; this fork's is CGAL
and cross-platform. MCP can find mesh errors (`get_mesh_health`, `get_object_components`, the
`MeshErrors` warning) but cannot fix them. The user asked for the repair in this release.

## What exists (verify, don't trust)

- **The GUI path:** the warning icon's Repair and the context menu's item
  (`MenuFactory::append_menu_item_fix_through_cgal`, `GUI_Factories.cpp:~961`) call
  `ObjectList::fix_through_cgal()` (`GUI_ObjectList.cpp:~6150`). It takes an undo snapshot ("Repairing
  model object"), opens an app-modal `ProgressDialog`, and for each object (or volume) calls
  `fix_model_with_cgal_gui(model_object, volume_idx, progress_dialog, msg, res, keep_painting)`
  (`src/slic3r/Utils/FixModelByCgal.cpp:~71`).
- **What the repair does:** in a worker thread (`cgal_fix_model`) that writes to the `ModelObject` while
  the dialog pumps events: it splits a splittable volume into parts, drops tiny parts, repairs each part
  with `MeshBoolean::cgal::repair` (`src/libslic3r/MeshBoolean.cpp:~478`: polygon-soup cleanup, orient,
  ...), and merges them back. It honours the `keep_painting` app setting (painting is cleared unless it is
  set). Afterwards the list calls `ensure_on_bed`, `changed_mesh`, re-lists added volumes, notifies the
  plate list and updates the error icon.
- **Why MCP can't call it as it is:** the `ProgressDialog` is app-modal, and a modal opened inside
  `run_on_main_thread` blocks the GUI thread for good (CLAUDE.md, Dialog Suppression). And its worker
  mutates the model off the main thread, which is safe in the GUI only because the modal dialog keeps the
  user out.
- **Upstream:** `fix_through_cgal` and `FixModelByCgal.cpp` exist in upstream OrcaSlicer.

## Your task (design first)

1. **Verify** the path above, and exactly what the repair changes: volume count (split / merge), dropped
   parts and the threshold, painting, the object's transform and bed position, the mesh stats it records
   (`repaired_errors`), the error icon. Note how long it takes on a large mesh at -O0.
2. **Design `repair_mesh`**, an app tool in the Models category:
   - Arguments: `object_id`, optional `volume_id` (like the GUI's per-volume repair); anything else only
     if it maps to something the GUI offers (e.g. whether to keep painting, defaulting to the app setting).
   - **One repair core shared by the GUI and MCP** (DRY): separate the repair from the dialog in
     `FixModelByCgal.cpp` so `fix_model_with_cgal_gui` becomes the dialog wrapper around it, and MCP calls
     the core with no dialog. Keep the GUI's behaviour exactly.
   - **Threading:** decide where the repair runs under MCP and why it is safe: the model must not be
     mutated off the main thread while the main thread runs other work, the GUI must not open any modal,
     the HTTP thread's quit rules (CLAUDE.md "Shutdown") must hold (a long repair and a quit: what
     happens), and nothing may hang. A copy-repair-apply scheme (copy the meshes on the main thread,
     repair off it, apply on the main thread) is one option; blocking the main thread for the repair is
     another; compare them and pick one.
   - **Undo:** one snapshot, taken only if the repair changes something (06b's rule); refused calls take
     none.
   - **Answer:** facts only, before and after: `get_mesh_health`'s numbers for the object (open edges,
     shells, manifold, repaired errors), volumes before / after, parts dropped, whether painting was kept,
     the object's placement fields (07f), and a failure's message. No verdict.
   - **Refusals** (the tool's own error, nothing changed): a busy pipeline or job (like 07e's), a gizmo
     open (the GUI's own check), an unknown object or volume, an object with nothing to repair (say so;
     not an error if the GUI treats it as a no-op).
   - **Text:** the tool's description, 08's `MeshErrors` warning text and `get_mesh_health`'s description
     now name `repair_mesh` (replacing "MCP has no repair tool"); add a `next_steps` pointer from
     `get_mesh_health` / the `MeshErrors` warning to `repair_mesh` for an object that has errors; one line
     in the server instructions only if it fits under the 2,048-character cap, and only with the user's
     approval through the orchestrator (the text is the user's).
   Present the verification and the design, then stop and wait for the orchestrator's yes.
3. **Tests.** A synthetic mesh with a hole and one with a stray shell: the core repairs them (numbers
   before and after), the GUI wrapper and the MCP path give the same mesh for the same input, one undo step,
   no snapshot for a no-op, the refusals. Put mesh tests where tests/AGENTS.md says they belong.
4. **Probe letter** for any change to `FixModelByCgal.cpp` or `GUI_ObjectList.cpp` (upstream Orca code):
   the next free letter in CLAUDE.md's block, with a paragraph.

## How to work

Verify, design, then stop and present. Wait for a yes. After that: create branch
`rel2506/08b-repair-mesh` off the latest `mcp`, implement test-first, check it live (a model with a hole
and a stray shell: `get_mesh_health` before, `repair_mesh`, `get_mesh_health` after, `undo`, the GUI's own
Repair on the same file gives the same result), run the suites, regenerate the golden file, update
CLAUDE.md (tool table and count) and `docs/tools/reference.md`, and report back.

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

- An agent can repair an object's mesh through MCP, getting the same result as the object list's Repair.
- The GUI's Repair behaves exactly as before, through the same core.
- The mesh warning text and `get_mesh_health` point agents to `repair_mesh`.

## Report back

- **Verification:** what the repair changes, and how long it takes.
- **The design:** the core, the threading choice and why it can't hang, undo, the answer, the refusals.
- **Tests** and the **live result.**
- **Probe letter.**
