# Release v2.5.0.6-dev: orchestration

Written 2026-09-26. This folder holds one self-contained prompt per unit of work. Each prompt
(`01-…` to `08-…`) can be handed to a fresh agent as-is: it carries its own context, evidence,
rules and definition of done. This README is for the orchestrator and the user. Agents working on
a prompt do not need to read it.

Pointing and the window screenshot are **not** in this release. They ship together in the one
after it; the agreed design is in [`docs/roadmap.md`](../../../roadmap.md).

---

## Why this release

A real session on 2026-09-26 took a painted Kuromi figurine from Blender to a sliced Creator 5 Pro
project. The job got done, but OrcaMCP made it hard:

- `load_model` silently replaced the user's presets and renamed the project.
- A later save overwrote the user's Blender export.
- Renders went blank after slicing.
- The estimate reported twice the real layer count.
- The agent never found the mesh-inspection and paint tools until the user pushed.

Its gap report was checked against the code, the transcript and the live app. Several causes it
gave were wrong; the prompts carry the corrected facts.

- **Transcript:** `~/.claude/projects/-Users-hanan-Documents-3D-Prints/69f5c2a1-ff9b-43a1-aaac-1c1649d85dbc.jsonl`
  (04:18–05:44 UTC).
- **App log:** `~/Library/Application Support/OrcaMCP/log/debug_Fri_Sep_25_03_16_02_39172.log.0` (UTC+7).

## Release acceptance

1. A fresh agent given only "check this model for problems and set up supports" finds the mesh-health tool
   and `paint_object` without being prompted (checked in 08).
2. A scripted replay passes:
   - load a 3MF with embedded Bambu presets onto an empty plate: presets and project name are kept;
   - render from the Preview tab after a slice: the image is not blank;
   - scale an object: it stays on the bed;
   - the estimate's printed layers ≈ height ÷ layer height.
3. With the app offline and then online, the bridge serves the same tool list (names and
   descriptions), and `get_server_info` names every registered tool.
4. All three platforms are green in CI, including the Linux regression job; the tag is on the
   exact tested commit.

---

## The prompts, in order

| # | Prompt | Needs merged first | Touches mostly | Status |
|---|--------|--------------------|----------------|--------|
| 01 | [Tool-list foundation](01-tool-list-foundation.md) | — | every `register_tool`, `get_server_info`, bridge, golden file | merged and pushed 2026-09-26; Build all green (run 36229581923) |
| 02 | [load_model: geometry only, honest reporting](02-load-model.md) | 01 | `Plater.cpp`, `MsgDialog.cpp`, load handler | merged 2026-09-26; pushed as batch 1 |
| 03 | [Render, transforms, estimate](03-render-transforms-estimate.md) | 01 | `OrcaMCPPlateUtils.cpp`, `OrcaMCPCommon.cpp`, estimate handler | merged 2026-09-26; pushed as batch 1 |
| 04 | [Printer match and slot colours](04-printer-and-colours.md) | 01 | printer tools, Flashforge, preset utils | merged 2026-09-26; pushed as batch 1 with 02-03 |
| 04b | [Quit deadlock, busy-port crash](04b-shutdown-and-port.md) | 01–04 | `HttpServer`, `GUI_App` shutdown, `run_on_main_thread` | implementing (busy-port moved to 09; adds localhost-only bind) |
| 05 | [Mesh health](05-mesh-health.md) | 01, 03 | new tool, `active_warnings`, `get_scene_info` | design approved 2026-09-26; queued behind 04 |
| 06 | [Workflow tools](06-workflow-tools.md) | 01, 03 | bridge, slicing status, preset reads, paint remap, estimate breakdown | design approved 2026-09-26; queued |
| 07 | [Sliced layer plan](07-layer-plan.md) | 01, 03 | `OrcaMCPFirstLayerPlan.cpp`, `render_plate_view` | design approved 2026-09-26; queued |
| 08 | [Server instructions and hints](08-instructions-and-hints.md) | 01–07 | `initialize`, descriptions, result hints | not started |
| 09 | [Several instances, switch between them](09-second-instance-crash.md) | 01–08 | port fallback, instance registry (with open file), bridge `list_instances` / `select_instance` | added 2026-09-26 by the user; design goes to the user; starts after all other coding |

If time runs short, the priority is 01, 02, 03, 04b, 05, 08, 04, 06, 07. Anything unfinished moves to the
roadmap; nothing ships half-done.

## How to run them

- **One C++ build at a time, in the main checkout.** Do not use git worktrees. `build/arm64` is
  31 GB, a worktree forces a full app rebuild, and the disk has about 80 GB free. So implementation
  is sequential: each prompt works on its own branch `rel2506/NN-<name>` off the latest `mcp`.
- **Design phases can overlap.** They are read-only. While one prompt implements, the next can
  verify its evidence and draft its design. Its branch is only created when its turn comes.
- **Python-only work can overlap with a C++ build**, since it needs no build. This is the bridge
  part of 06, once 01 is merged.
- **Launching a prompt:** give an agent the file's full text, or start a fresh Claude Code session
  in the repo with "Read `docs/superpowers/plans/2026-09-26-release-2506/NN-….md` and follow it."
- **Approval gate:** every prompt stops after its design and waits for the user's yes. A subagent
  ends its turn with the design as its report; the orchestrator relays it to the user and resumes
  the agent with the answer.
- **Merging:**
  - The orchestrator reviews the finished branch (code review skill), runs the full test suites,
    then rebases it onto `mcp` and fast-forwards.
  - Pushes are batched: every push starts an hour-long Build all. Check
    `gh run list -R okets/OrcaMCP` first, and cancel redundant runs.
  - Docs-only commits carry `[skip ci]`.
- **Weekend mode (user, 2026-09-26).** The orchestrator runs the release unattended and never waits
  for CI. It pushes at three batch points (after 04b, after 07, and the release push with 08 and the
  version bump) and fixes CI failures in the next sprint. It approves designs that stay inside their
  brief and the user's earlier decisions. When a question or approval genuinely belongs to the user,
  it pauses all work and leaves the question in the session.
- **Acceptance by the orchestrator, per sprint (user, 2026-09-26: "you are an agent and we are
  building an MCP").** Before merging, the orchestrator uses the sprint's build *as an agent*, through
  the `mcp__orca-slicer__*` tools only, on a copy of the data dir: it replays the sprint's user-facing
  scenario and records every friction point it hits (a tool it couldn't find, a result that didn't say
  what happened, a missing next step). Friction goes into the owning prompt, or into 08 if it is
  discoverability. Unit tests and code review come on top of this, not instead of it.
- **Status:** update the table above as prompts move through design → approved → implementing → merged.

## Shared traps (also inside every prompt)

- **The app is free to use.** The user's work is saved; agents may quit, relaunch and change the
  live OrcaSlicer without asking (user, 2026-09-26).
- **The user's preset library now holds embedded Bambu presets**, e.g.
  `Bambu Lab A1 0.4 nozzle(Kuromi head.3mf)`. Don't delete them without asking.
- **Screenshots are allowed for this weekend session only** (user, 2026-09-26). The user keeps
  sensitive windows minimized. Capture or crop to the OrcaSlicer window, delete images after the check.
  (Earlier the same day a full-screen capture caught a WhatsApp window; it was deleted at once.)
- **macOS Accessibility is NOT granted** to this session's tools (a scripted Tab press was refused), so
  no agent can switch OrcaSlicer's tabs; a Preview-tab check needs the user's click.
- **The dev build is unoptimized** (`-O0`, upstream CMake): slicing is 20-70x slower than the release.
  A six-minute tree-support slice on 2026-09-26 looked like a stall; the release did it in 5 s.
- **Never call `send_to_printer`**: on Flashforge it uploads and starts the print.
- **The local build tree is stale.** `libslic3r_version.h` says 2.5.0.4-dev while `version.inc`
  says 2.5.0.5-dev, so the first build reconfigures and takes longer.

## Release checklist (orchestrator)

1. All prompts merged, or moved to the roadmap with a reason.
2. CLAUDE.md tool table and count, `docs/tools/reference.md`, and the golden tools file are current.
   The 01 tests enforce the last one.
3. The acceptance items above pass. The fresh-agent check runs in 08.
4. Bump `version.inc` to `2.5.0.6-dev`, push, and wait for green CI, including the Linux regression job.
5. Tag `v2.5.0.6-dev` on that exact commit and push the tag; `release.yml` publishes.
6. Record the release in memory and in this README.
