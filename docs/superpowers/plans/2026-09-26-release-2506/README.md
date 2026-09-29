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
| 02 | [load_model: geometry only, honest reporting](02-load-model.md) | 01 | `Plater.cpp`, `MsgDialog.cpp`, load handler | merged 2026-09-26; pushed as batch 1; Build all green after the Windows include fix (run 36247216022) |
| 03 | [Render, transforms, estimate](03-render-transforms-estimate.md) | 01 | `OrcaMCPPlateUtils.cpp`, `OrcaMCPCommon.cpp`, estimate handler | merged 2026-09-26; pushed as batch 1; Build all green after the Windows include fix (run 36247216022) |
| 04 | [Printer match and slot colours](04-printer-and-colours.md) | 01 | printer tools, Flashforge, preset utils | merged 2026-09-26; pushed as batch 1 with 02-03; Build all green after the Windows include fix (run 36247216022) |
| 04b | [Quit deadlock, busy-port crash](04b-shutdown-and-port.md) | 01–04 | `HttpServer`, `GUI_App` shutdown, `run_on_main_thread` | merged and pushed 2026-09-26 (batch 1b); Build all green (run 36253659676) |
| 04c | [Crashes quitting around a slice](04c-quit-crashes.md) | 04b | `Plater` reset/teardown order, quit with open dialogs | merged and pushed 2026-09-27 (batch 1c); green after the CI fixes (run 36282514718) |
| 04d | [Leftover "Loading..." windows](04d-loading-windows.md) | 04c | `Plater::priv::load_files` progress dialog | closed 2026-09-27: with the screen unlocked, 5 loads left 0 ghosts (checked with the user present). A locked-screen artefact; fix not shipped, noted in the roadmap |
| ci | CI fixes (no prompt file; 04b's agent) | 04c | Windows test force-include of win_platform.hpp; the Linux login-route test race | fixed and pushed 2026-09-27; Build all green on every platform (run 36282514718) |
| 05 | [Mesh health](05-mesh-health.md) | 01, 03 | new tool, `active_warnings`, `get_scene_info` | merged and pushed 2026-09-27; Build all green (run 36282514718) |
| 06 | [Workflow tools](06-workflow-tools.md) | 01, 03 | bridge, slicing status, preset reads, paint remap, estimate breakdown | merged and pushed 2026-09-27 (live checks passed); Build all green (run 36292935184) |
| 07 | [Sliced layer plan](07-layer-plan.md) | 01, 03 | `OrcaMCPFirstLayerPlan.cpp`, `render_plate_view` | merged and pushed 2026-09-27 (overhang facts, not verdicts); Build all green (run 36302057792) |
| 06b | [Slicing and loading follow-ups](06b-slicing-followups.md) | 07 | `slice_all`, slicing status, OrcaMCPCommon, load errors, undo, layer ranges | merged and pushed 2026-09-27 (five rounds, 31 commits; merge ca1a608e30; Build all 36319238134); probe letters Z, AA–AD |
| 06c | (no prompt file; 06b's agent) | 06b | `slice_all`'s refusal reason across plates; `transform_objects` all-or-nothing | merged and pushed 2026-09-27 (merge 871ff5a60a, three commits; Build all 36320742400 green on every platform, also covers 06b) |
| 07b | [Over-height check on non-Bambu printers](07b-height-check.md) | 06c | GCodeProcessor `;Z:` parsing, the 0-means-no-limit rule, MCP send/export gating | merged and pushed 2026-09-27 (four rounds, 10 commits; merge a726b9ca52); probe Y (six counts) |
| 07c | [A misspelled or missing argument is refused](07c-unknown-arguments.md) | 07b | `handle_tools_call` (`OrcaMCPToolArguments`), the bridge's own tools | merged and pushed 2026-09-28 (two rounds, 6 commits; merge 0021515d7e) |
| 07d | (no prompt file; 07c's agent) | 07c | nine per-tool argument bugs from 07c's audit: rotate relative, one-bound range delete, empty reset keys, flatten's plate-wide orient, clone count, cut keep, config-key category, uniform scale, empty set_temperature | merged and pushed 2026-09-28 (three rounds, 14 commits; merge 85ae40e7fa; Build all green) |
| 07e | (no prompt file; 07c's agent) | 07d | arrange_objects / auto_orient / flatten_object wait for their job and return the final placement; get_slicing_status reports a running job | merged and pushed 2026-09-28 (three rounds, 9 commits; merge 6b3491f94c); probes AE and AF |
| 07f | (no prompt file; 07c's agent) | 07e | an object whose instances sit on two plates: a 3MF round trip loses the second plate; on_bed measured over both instances | merged and pushed 2026-09-28 (two rounds, 4 commits; merge e9431bd0c4; Build all green); probe AG |
| 07g | [The first slice can hang the app](07g-tbb-thread-naming.md) | 07f | `name_tbb_thread_pool_threads_set_locale` (Thread.cpp): a barrier across the TBB pool | merged and pushed 2026-09-28 (two rounds, 3 commits; merge 5c0bbc0278; Build all green on every platform); probe AH |
| 08 | [Server instructions and hints](08-instructions-and-hints.md) | 01–07g | `initialize`, descriptions, result hints | merged and pushed 2026-09-28 (two rounds, 16 commits; merge 1a3afdc522); the user's shorter instructions text (1,228 characters); acceptance 3/3 valid runs |
| 08b | [An agent can repair a mesh](08b-repair-mesh.md) | 08 | a `repair_mesh` tool on the object list's CGAL repair (`FixModelByCgal.cpp`), one core for the GUI and MCP | merged and pushed 2026-09-28 (three rounds, 4 commits; merge 003f581c0d); probe AI |
| 09 | [Several instances, switch between them](09-second-instance-crash.md) | 01–08 | port fallback, instance registry (with open file), bridge `list_instances` / `select_instance` | merged and pushed 2026-09-28 (four rounds, 11 commits; merge 3aa965e785; Build all green); probe AJ. A test launch once opened the user's installed app (13:23); tests are now hermetic and can't launch or reach 13618 |
| 11 | [Parts and mesh edits](11-parts-and-mesh-edits.md) | 09 | split to objects/parts, add part/modifier/negative/support volumes, per-part settings and transforms, change type, delete, rename, merge; the get_server_info catalogue's growth | merged and pushed 2026-09-28 (11a + two rounds; merge 1769d93cd0; Build all green); probes AK, AL |
| 12 | [Arrange, instances, plates](12-arrange-instances-plates.md) | 11 | arrange all plates and options, remove instance / instance count / fill bed, plate settings, the global bed type path | merged and pushed 2026-09-28 (two rounds, 8 commits; merge b876149c10; one racy bed-fill test failed on Linux and Windows, fixed in 13); probes AM, AN |
| 13 | [Filaments and printers](13-filaments-and-printers.md) | 12 | add/delete filament slots, a slot's settings, installing printers/filaments, Flashforge fans / speed / Z offset (no live printer commands without the user) | merged and pushed 2026-09-28 (three rounds; merge 7c42ab06d4; Build all green, 12's race fixed); probes AO-AX |
| 14 | [Slice, export, view](14-slice-export-view.md) | 13 | cancel a slice, export .gcode.3mf and STL, G-code at a layer, the user's tab and camera, reload from disk | merged and pushed 2026-09-29 (four rounds; merge 186abeec05; Windows ARM64 then failed on a test variable named `far`, an empty macro in windef.h, renamed in 67aa9d956c; Build all green); probes AY-BK |
| 15 | Follow-ups (no prompt file; sent as messages) | 14 | 14's review leftovers (filament-change reports before a slice, the plate's build-volume test), binary STL read as ASCII (admesh), export_gcode waits for its .gcode | merged and pushed 2026-09-29 (four rounds; merge ca4a7251d2; Build all green); probes BL, BM |
| 10 | [Flashforge print options](../2026-09-27-flashforge-print-options.md) (flow calibration, leveling, time-lapse) | 01–14 | `FlashforgeApi::PrintOptions`, the send dialog, `send_to_printer`, `print_printer_file`, `get_printer_status` | merged and pushed 2026-09-29 (four rounds; the live check with the user on a Creator 5 Pro, firmware 1.9.9: every option takes effect, flow calibration ~3.5 min per tool, leveling ~12.5 min, time-lapse recorded; the dialog remembers each box as last left, the user's call; merge ad26d8c182); probes BN-BQ |

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

Order of work: 08, 08b, 11a, 09, 11, 12, 13, 14, 15, then 10 (the Flashforge print options, last), then the version bump.

Server instructions: decided 2026-09-29 by an A/B test (the user's call): 36 fresh `claude -p` agents, three texts (current 1,301 chars; +7 tool names; +10), six tasks needing tools the current text does not name. 12/12, 12/12, 11/12 (the miss warned first on purpose); same calls, searches and cost. Agents find tools by name in the deferred list, so the current text stays.
The instructions text is the user's shorter version (2026-09-28); each prompt adds only its own tools to it.

## Waiting for the user

- ~~**04d: a one-minute unlocked-screen check.**~~ Done 2026-09-27: no ghosts when unlocked. With the screen unlocked, the orchestrator runs five
  `load_model` calls while the user watches OrcaSlicer, and the user says whether small "Loading..."
  windows pile up next to the main window. If they don't, the ghosts are a locked-screen artefact, and
  the fix (turning off the progress-dialog fade on macOS) is optional. If they do, ship the fix.
- ~~**09: the multi-instance design.**~~ Approved by the user 2026-09-28.
- **Optional: upstream issues.** The user decides whether to file any. (1) Closing many upstream Yes/No
  dialogs with the close box takes the "proceed" branch (roadmap). (2) The G-code over-height check never
  fires on non-Bambu printers (07b). Warn upstream that parsing `;Z:` alone breaks every non-Bambu
  multi-extruder preset, because their unset per-nozzle height reads as 0 mm, so the guard is needed. (3) STL repair counts are thrown away (roadmap).
  (4) Undo of a printable toggle leaves the plate unsliceable in the GUI too (probe AD, found in 06b).
  (5) A Type 1 prime tower on a non-Bambu printer drops its top tool-change layer's block, because an unset
  extruder height (0) reads as a limit (probe Y's sixth count, found in 07b). 07b's agent recommends offering (2)
  with (5) and the PrintObject companion as one issue: treat 0 as no limit everywhere a height is compared.
  (6) OrientJob's finalize can write to an instance a delete freed after process() returned (07d review; the GUI
  too). Carried as probe AE (07e). (7) A cancelled or failed arrange leaves plates locked and the arrange button dead
  (probe AF, 07e). (8) The plate's own arrange button can leave m_arrange_running stuck when the UI worker is busy
  at the click (select_plate_by_hover_id, action 3; not fixed). (9) Opening a 3MF places only an object's first
  instance on a plate (probe AG, 07f). (10) The first slice's thread-naming barrier can hang the app for good (probe
  AH, 07g; from PrusaSlicer, whose #5661 was an earlier hang in the same barrier).
- ~~**Sequencing the Flashforge print-options plan.**~~ Decided 2026-09-28: it runs last (row 10), right before the
  version bump.

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

## After the tag: an upstream review with the user

The user asked on 2026-09-27: once the release is tagged, go through every issue this release found that
belongs upstream, and decide together which to offer. Prepare one list from:

- CLAUDE.md's "Carried upstream fixes" probe block: every letter J–AC and later, with what each fixes and
  whether upstream still has it (run the probes against `upstream/main`);
- docs/roadmap.md's "left as upstream has them" items;
- the README's "Optional: upstream issues" notes, e.g. the non-Bambu over-height check and its trap.

For each item, note its severity, how easy it is to reproduce on upstream, and whether it's a clean small
patch. The user decides; nothing is filed without their word.

**Released 2026-09-29:** v2.5.0.6-dev, tag on 90c5e8b374 (the version bump; Build all green, 23 jobs), release
workflow green, published with the three installers: https://github.com/okets/OrcaMCP/releases/tag/v2.5.0.6-dev.
Next: the upstream issues, from `../2026-09-29-upstream-issues.md`, in a fresh session.

## Release checklist (orchestrator)

1. All prompts merged, or moved to the roadmap with a reason.
2. CLAUDE.md tool table and count, `docs/tools/reference.md`, and the golden tools file are current.
   The 01 tests enforce the last one.
3. The acceptance items above pass. The fresh-agent check runs in 08.
4. Bump `version.inc` to `2.5.0.6-dev`, push, and wait for green CI, including the Linux regression job.
5. Tag `v2.5.0.6-dev` on that exact commit and push the tag; `release.yml` publishes.
6. Record the release in memory and in this README.
