# Next release plan — v2.5.0.2-dev

Written 2026-09-17, immediately after v2.5.0.1-dev was tagged. For the agent picking this up:
read this whole file before touching anything. The traps section exists because two of the items
below look like one-line fixes and are not.

---

## Where things stand

| Branch | Commit | Meaning |
|--------|--------|---------|
| `mcp` (default) | `89eb8b12e0` | Tip. The released commit plus three docs/CI commits after it. |
| `sync-upstream-2.5` | `89eb8b12e0` | Same commit as `mcp`. |
| `main` | `ca668a3bc9` | Exactly `upstream/main`. Tracks Orca Slicer, carries no fork work. |
| tag `v2.5.0.1-dev` | `753322a957` | **Published 2026-09-17 20:46 UTC**, three assets, artifact verified. |

`version.inc` reads `2.5.0.1-dev`. Release: <https://github.com/okets/OrcaMCP/releases/tag/v2.5.0.1-dev>.

Verify before trusting any of the above -- commits move:

```bash
git fetch origin --tags
for b in mcp sync-upstream-2.5 main; do echo "$b $(git rev-parse --short origin/$b)"; done
echo "tag $(git rev-list -n1 v2.5.0.1-dev | cut -c1-10)"
gh release view v2.5.0.1-dev -R okets/OrcaMCP --json isDraft,assets -q '"draft: \(.isDraft)  assets: \(.assets|length)"'
```

### The release took seven runs. Read the post-mortem before touching release plumbing.

**`docs/release/2026-09-17-release-postmortem.md`** is the authority on what went wrong: every
failed run in order, its cause, its fix, and what now catches it in seconds. Do not re-derive it
from this file. The one-paragraph version:

Three root causes covered everything but the transient failures. `release.yml` never mirrored the
CI matrix. The September upstream merge took `--theirs` on `build_orca.yml` with a note saying the
fork's rebranding would be re-applied "later", which never happened, and that one resolution removed
three customisations that then hid each other. And a green `Build all` run does not exercise the
release path at all -- it skips Store packaging, never installs a DMG, never signs or notarizes --
so "CI is green" was true before every one of the failures and meant nothing. On top of that the
release action's asset upload failed on six of seven attempts on the 360 MB macOS image.

### Release tooling that now exists -- use it, in this order

| Tool | What it does | When |
|------|--------------|------|
| `scripts/check-fork-customizations.sh` | Asserts **19 invariants** an upstream merge tends to revert: app names, the DMG bundle rename in both code paths, the fork's bundle identifier and its template variable, signing gated on this repo and not on branches, quoted signing secrets, the release workflow's Windows inputs, the gh-CLI asset upload, the asset verification step, Connect AI strings, bridge packaging. Runs in CI on every push that touches source. | After any merge; before any tag. |
| `scripts/release-preflight.sh v<tag>` | The guard, plus Shellcheck with CI's exact command, YAML parse of every workflow, `version.inc` matches the tag, tag not already on the remote, and an Apple notary credential probe if a `release-preflight` keychain profile exists. Says what it cannot check. | Immediately before pushing a tag. Must exit 0. |
| Artifact check (post-mortem, "What still needs a build") | Download the DMG from the draft; confirm `OrcaMCP.app`, `com.orcamcp.OrcaMCP`, Developer ID team `9PCJMHHHK6`, `spctl` accepted as notarized. | Before publishing. **Never skip** -- two green runs shipped an image that overwrote the user's Orca Slicer and would not open. |

The release workflow was rewritten after the last release so that the gh CLI uploads assets with
retries and a final step fails the run unless all three are attached at the built size. **Its
first end-to-end test is the next release.** If "Create Release" still fails, the manual fallback
is in the post-mortem's procedure step 4.

### What shipped in v2.5.0.1-dev

Eight fixes, all found by driving a real Flashforge Creator 5 Pro rather than by reading code.
Four were in the send path and each was hiding the next, which is worth knowing: fixing one
revealed the following gate, so do not assume a single fix clears a path.

1. Storage state was wiped on every poll. Both polling agents set it outside the payload, and the
   parser treats a missing `sdcard` key as proof of no card.
2. The dialog refused over nozzle material. The local API reports a bore, never a material, so it
   now comes from the machine preset.
3. The material station never reached the dialog, so it mapped jobs to the external spool.
4. Every third-party printer displayed as "Unknown" — the name lookup only knew Bambu's models.
5. A failed print killed the app: a worker thread touched Cocoa view geometry.
6. The error said "ftp" on a printer with no FTP, instead of the printer's own words.
7. The error panel drew its text on top of itself.
8. `add_plate` over MCP left the canvas pointing at freed picking meshes.
9. Flashforge "remaining time" was the firmware's elapsed time. Now projected from elapsed and
   progress, unknown below 2 %; the raw firmware field is kept as `firmware_estimated_s`.

Plus, in the same tag: the About page rebranded and crediting the author; the macOS bundle renamed
to `OrcaMCP.app` in the DMG and given its own identifier `com.orcamcp.OrcaMCP`; macOS signing and
notarization actually running; `release.yml` passing the Windows arch and compiler.

---

## Goal for v2.5.0.2-dev

**Catch up to upstream, and start giving fixes back.** Every fix that lands upstream is one less
file in the conflict set forever, which is the cheapest way to make future syncs smaller. Bump
`version.inc` to `2.5.0.2-dev` as part of the release commit, not before.

### The four stages, in this order

The order matters. Identification comes first because after a 208-commit merge it is much harder to
tell which change was the fork's, and the surrounding context the patches apply to will have moved.

| Stage | What | Why here |
|-------|------|----------|
| **1** | Identify which of our fixes are pure upstream. **Identify only — do not open PRs yet.** | Cheapest before the merge, and the list steers what needs re-verifying after it. |
| **2** | Sync with `upstream/main`. Prove no regression on the fork. | The big one. See the conflict surface and traps below. |
| **3** | Fix the extruder count gap, and send it upstream as a PR. | It is upstream's bug (see Stage 3 section). Doing it after the merge means doing it once. |
| **4** | Send the rest of the Stage 1 list upstream as PRs, one per fix. | Contributing back is what shrinks the diff going forward. |

### Stage 1 output — already done, 2026-09-17

Each of these was **verified still present in `upstream/main`** at the time of writing, by reading
upstream's copy of the file. Re-check before opening each PR; upstream may have moved.

| # | Fix | Upstream file | Our commit | Why it is upstream's problem too |
|---|-----|---------------|-----------|----------------------------------|
| A | Worker thread touches widgets, killing the app on a failed print | `Jobs/BoostThreadWorker.hpp` | `4d76a06287` | Confirmed: upstream still calls `m_progress->show_error_info` straight from the worker thread. Any failed print on macOS aborts the process. Hits Bambu users too. **Highest value.** |
| B | Use-after-free on exit | `GLCanvas3D.cpp` (`~GLCanvas3D`) | `f599bda795` | Confirmed: the destructor still calls `reset_volumes()`, which ends by reading the notification manager through a half-destroyed Plater. |
| C | Unguarded index into an empty intersection | `libslic3r/PrintConfig.cpp` | `f599bda795` | Confirmed: `result = result_polygon[0]` with no emptiness check. Segfaults when extruder areas do not overlap. |
| D | Moonraker printers lose their storage state every poll | `Utils/MoonrakerPrinterAgent.cpp` | `91efc63d30` | Confirmed: upstream's payload has **no** `sdcard` key and still sets the state out of band, so `DevStorage::ParseV1_0` resets it on every status push. Klipper users get "Storage needs to be inserted". |
| E | Every third-party printer displays as "Unknown" | `DeviceManager.cpp` | `041db47482` | Confirmed: `get_printer_type_display_str` still falls straight through to `_L("Unknown")`. Affects upstream's own Moonraker printers. |
| F | Error panel draws its text on top of itself | `SelectMachine.cpp` | `f5ff97bfa1` | Confirmed: still no layout pass after `Wrap()`, so the error code overlaps the link below it. |

Two more worth considering, both weaker candidates:

- **Prefer an agent-reported failure reason over the code-derived string** (`Jobs/PrintJob.cpp`,
  `f5ff97bfa1`). Useful for any non-Bambu agent, but it is a small API addition rather than a plain
  bug fix, so expect more discussion. Send it after A–F have landed.
- **`PartPlate::set_shape` should unregister its raycasters before replacing the meshes**
  (`PartPlate.cpp`). Defensive hardening that would have made the `add_plate` crash impossible for
  any caller. There is precedent in `invalidate_plate_name_texture`. Optional.

**Not PR-able** — these depend on fork-only code and should stay here: the nozzle material read from
the preset, the material-station-to-AMS translation, everything in `FlashforgeApi.cpp` and
`FlashforgePrinterAgent.cpp`, the MCP `add_plate` and `unplaced_objects` work, and the About page.

Practical notes for Stage 4: one PR per fix, each with the reproduction from the commit message,
and open them against upstream's `main` **after** the merge so they apply cleanly to current code.

---

## Stage 2 — Sync with upstream Orca Slicer

`mcp` is **208 commits behind `upstream/main`** and 332 ahead of it. This is the deliberate
increment the user has been deferring; it is now top of the list.

### The real shape of the job

Measure it yourself before planning — these numbers will have moved:

```bash
git fetch upstream
BASE=$(git merge-base origin/mcp upstream/main)
git diff --name-only $BASE origin/mcp   > /tmp/fork.txt   # 260 files at time of writing
git diff --name-only $BASE upstream/main > /tmp/up.txt    # 507 files
comm -12 <(sort /tmp/fork.txt) <(sort /tmp/up.txt)        # 64 files — the conflict set
```

**64 files were changed by both sides.** That is the work. It includes the fork's hottest code:

- `src/libslic3r/GCode.cpp` — the fork's chamber-temperature and mixed-filament changes live here
- `src/libslic3r/GCode/WipeTower*.{cpp,hpp}` — six files, prime tower, heavily modified both sides
- `src/libslic3r/Preset.cpp`, `PresetBundle.cpp`, `AppConfig.cpp`
- `src/OrcaSlicer.cpp`, `src/dev-utils/OrcaSlicer_profile_validator.cpp`

Upstream's 208 commits land mostly in `src/slic3r/GUI` (327 file-touches), `src/slic3r/Utils` (33),
and vendor profiles. Seventeen of them touch `DeviceCore/`, `DeviceManager.cpp` and
`SelectMachine.cpp` — precisely where the eight fixes above live. Expect conflicts there and
resolve them keeping **both** upstream's change and the fork's fix. Never drop a fork fix to make a
merge easy; that rule is in the user's standing preferences.

### What upstream actually landed — the 208 commits by theme

Counts are approximate; they are there to show where the weight sits.

| Theme | ~n | What it is |
|-------|----|------------|
| **Publish 3MF** | ~45 | A whole new subsystem: publish a project with embedded settings, tabbed publish dialog, badges in thumbnails, mixed-filament and per-extruder slot selection, import-side hardening, OTA/updater plumbing gated by an app-config flag. Biggest single block and entirely new surface. |
| **Build warnings / -Werror** | ~25 | A campaign to zero the warning list, then enable `-Werror` with an exception list. Also ccache, PCH, clang-cl, shared object cache. Mechanical, but it touches a very large number of files — expect many trivial conflicts and almost no semantic ones. |
| **Wipe tower estimation + placement** | ~13 | Estimation extracted and unified, footprint sized from the planners, clamping fixes, brim/cone-aware preview, comfort margin for auto placement, placement before slicing and in the profile validator. **See the duplicate-port trap below.** |
| **CLI** | ~12 | `--inspect-paint`, `--inspect-mesh`, `--ground-*`, `--strict` plus a warnings array in `result.json`, `--export-settings -` to stdout, `compatible_printers_condition` in compat checks, relative path resolution, GUI mixed-filament rules applied on the CLI. |
| **Preset / vendor loading** | ~8 | One shared library load in the CLI resolver, vendor trees loaded once, failed vendor loads no longer kept, inherited presets resolved through vendor manifests, hotfix for system bundles being recopied on every startup, detach-from-parent for parentless profiles. |
| **Slicing correctness** | ~8 | Non-deterministic slicing fixed by ordering per-layer intersection lines canonically, deterministic tree support, internal bridges over Hilbert/Octagram infill, bridge flow with zero-gap supports, fuzzy-skin min junction width, stale paths when merging perimeter regions. Plus features: inward wipe for external perimeters, toolchange cyclic order. |
| **Security / robustness** | ~6 | **Two stack buffer overflows in ADMesh `stl_read`** (unbounded solid name, MW metadata parse) — a malicious STL issue, take this one. Bounds-checked toolchange flush-volume and HRC per-filament lookups, guarded short per-filament config arrays, config import confined to the preset directory, float-or-percent range validation. |
| **macOS / UI** | ~6 | Menu icons on macOS and Linux, custom colour accuracy, single-instance activation no longer maximises, publish dialog layout, Windows light-mode text, paint-on-resize. |
| **Profiles** | ~6 | **Flashforge Creator 5 and Creator 5 Pro 0.25 mm nozzle profiles** — a direct gain for this project's printer. Also Qidi X-Plus 5 chamber heating, Snapmaker U1 ABS/ASA bed caps, PETG SuperTack temps, Folgertech i3 printable area. |
| **Plates / instances** | ~2 | "Register Instance Copies and Moves with Their Plate" and "Let the Remaining Per-Plate Object Scans See Every Instance". Adjacent to the `add_plate` fix that shipped — read these before touching plate code. |
| **Plugins** | 1 | Plater notification API for plugins. |

Most relevant to this fork, in order: the Flashforge 0.25 mm profiles, the plate/instance fixes, the
ADMesh overflow fixes, the HRC and flush-volume bounds checks (the fork touches HRC in
`SelectMachine.cpp`), and the determinism fixes, which should make the regression suite steadier.

### Two traps specific to this merge

**1. `docs/superpowers/` is gitignored upstream.** Upstream added it at `.gitignore:56` and deleted
its own plan docs. This fork tracks **17** plan documents there, including this file. Already-tracked
files stay tracked, so nothing disappears — but after the merge, a newly written plan will be
silently ignored by `git add` and will look like it simply did not save. Decide deliberately: either
negate the rule in the fork's `.gitignore`, or move the fork's plans somewhere upstream does not
ignore. Do not let it be discovered by accident.

**2. Some upstream commits are already in the fork as ports with different SHAs.** The wipe tower
cluster and "Fix the Folgertech i3 0.6 nozzle printable area" were cherry-picked or re-implemented
here, so git sees upstream's originals as missing and will try to apply them again. Check each one
against what the fork already has before resolving; applying a port twice is how a working tree ends
up with duplicated logic that still compiles.

### Suggested approach

1. Work on a fresh branch off `mcp`, not on `mcp` itself.
2. Merge, do not rebase. 332 commits of fork history replayed one at a time is a worse job than one
   merge, and the fork's history is already public.
3. Resolve in passes by area rather than file-by-file in whatever order git lists them: libslic3r
   first (it is what everything else compiles against), then GUI, then profiles.
4. After the merge builds, the gates in order: unit suites, then the external regression suite, then
   the profile slice sweep. All three are in CI; run them locally first where you can.
5. Re-verify the eight shipped fixes by hand against the printer if one is available. A merge that
   compiles can still have dropped a fix — that has happened on this project before, where a
   single line was lost in a three-way merge and only the compiler caught it.

### Known port-order trap

Porting an upstream commit before its dependency has caused a segfault upstream never had on this
project. If you cherry-pick anything rather than merging wholesale, take commits in upstream's own
order.

---

## Stage 3 — The extruder count gap: investigated 2026-09-18, not fixing it here

**Decision (2026-09-18, with the user):** this is upstream's bug, it is bigger than one printer, and
the fork will not carry a patch for it. Reported as
<https://github.com/OrcaSlicer/OrcaSlicer/issues/15758>, with a suggested direction and an offer
to PR it if upstream agrees. What follows replaces the earlier text, whose central claim turned out
to be wrong.

### What the evidence showed

Established against the merged tree (upstream `52f4c68c41`), the live Creator 5 Pro, and
Flashforge's published specifications -- not by reading alone.

- **The printer reports the count.** `nozzleCnt: 4`, per-nozzle temperatures, per-nozzle bores
  (`nozzleModel: "0.4mm;0.4mm;0.4mm;0.4mm"`). "Nothing ever told it otherwise" is true of the
  *device model*, not of the source data.
- **The nozzles are hardened steel.** Flashforge ships the C5/C5P with red-copper nozzles carrying a
  hardened-steel tip, rated 320 °C and sold for CF filaments. So the preset's `hardened_steel`
  (HRC 55) is correct, and the hardness gate is *right* never to fire: the most demanding filament in
  the table needs HRC 40. **There is no missing abrasive warning on stock nozzles.**
- **The hardness gate does not go through the count comparison.** It walks
  `GetExtderSystem()->GetExtruders()` directly. The nozzle-label path exits before the comparison on
  any printer without a hotend rack. The only two live callers of `get_mapped_nozzles()` are the
  blacklist checks, driven by the dialog's refresh timer, and upstream's own comment there handles
  the empty map for "a non-rack printer". **Consequence on the C5P: log noise at timer rate.**
  Printing, the hardness gate and the material blacklist are all intact.
- **Agents never see it.** `send_to_printer` on a print host uploads directly and never opens the
  `SelectMachineDialog`; boost log lines never reach `active_warnings`. Zero occurrences were logged
  during the live MCP send. The spam is a human-in-the-GUI symptom only.
- **Raising the device count would introduce bugs.** The Send dialog assumes one or two extruders in
  23 places, the Device tab in 13, and `sync_extruder_list` -- gated today on `is_multi_extruders()`
  -- would index a one-entry `physical_extruder_map` with `extruder_nums == 4`, asserts compiled out.
- **It is upstream's, by construction.** `get_printer_extruder_count()` *is* `nozzle_diameter.size()`.
  Upstream's own Snapmaker U1 (four tool-heads, added in this merge) is modelled exactly like the C5P:
  four nozzle diameters, one logical extruder, no physical map. Upstream's Moonraker agent publishes a
  single `nozzle_temper` and never sets a count. `get_mapped_nozzles()` is byte-identical between the
  fork and upstream.
- **The fix the evidence points at** lives in the dialog, not the device layer: Bambu's dual profile
  declares `physical_extruder_map: ["1","0"]`; tool-changers leave the default `[0]`. Distinct
  physical extruders -- 2 for the H2D, 1 for the U1 and C5P -- matches the device model in every
  case. That is the direction offered in the issue.

### What happened while gathering it

Calling `send_to_printer` with no arguments to "open the dialog and observe" **started a print**:
on a print host the tool uploads directly and `start_print` defaults to true. It was cancelled
before any heater reached target. CLAUDE.md's description of the tool was wrong and has been
corrected. Two design questions came out of it and are open with the user: whether an agent-facing
send tool should default to starting a print, and how to tell agents which log warnings are known
noise without hiding them (see "Agent-visible warnings" in the backlog below).

## Backlog — smaller known items

### Agent-visible warnings — decide how to mark known noise

Raised 2026-09-18. Agents that tail `log/debug_*.log` to debug pay tokens for repeated,
known-benign lines (the `total_ext_count not match` spam above being the example). The user does
not want them suppressed at source. Candidate approaches, not yet chosen: an MCP diagnostics tool
that returns recent log lines de-duplicated with repeat counts and annotated from a known-benign
registry (pattern, reason, upstream issue URL); a fork-side log-sink formatter that collapses
consecutive duplicates into one line with a count; or both, sharing one registry file in
`resources/`. `get_server_info` should point agents at the tool instead of the raw log.


### `get_print_estimate` reports the wrong layer count — confirmed, with numbers

For plate 2 of `frame-abs-4plate-chamber60.3mf` (tallest object 37.0 mm, layer height 0.12 mm,
initial layer 0.25 mm):

| Source | Layers |
|--------|--------|
| Exported G-code, `;LAYER_CHANGE` markers | **307** |
| `get_print_estimate` `layer_count` | **362** |

Max Z in the G-code is 36.97 mm, and `(36.97 − 0.25) / 0.12 + 1 = 307`, so the G-code is right and
the tool is wrong by 55 layers. Reproducible: slice that plate, export, count markers, compare.
Start at whatever `layer_count` is derived from in the estimate handler — it is not counting the
same thing the G-code writer counts.

### `obico.configured` flickers to false after `send_to_printer`

Carried over and **not re-verified since the fixes above landed** — confirm it still happens before
investigating. Values stay correct on disk; a restart clears it. Suspect a read of preset state
while the send path has it in flux.

### Volumic EXO42 mirror mode logs a G-code path conflict

The profile validator reports a conflict between the wipe tower and the test cube in mirror mode.
It exits 0, so it does not gate CI. Carried over, not re-verified.

### Report the two inherited crashes upstream

`f599bda795` fixed a use-after-free on exit and an unchecked empty intersection, both of which are
upstream bugs, not fork bugs. They have not been reported to SoftFever. Doing so is the
stay-close-to-upstream rule working in the other direction.

---

## Standing rules you must not rediscover the hard way

- **A release needs a fully green CI run on the exact commit being tagged.** Not "green except
  one known case". If the branch moves after the run, the run no longer counts.
- **The tag value must equal `SoftFever_VERSION` in `version.inc`**, prefixed with `v`. The release
  workflow downloads artifacts by that exact name and fails after a full build if they disagree.
- **Never push a tag without the user saying so**, in those words, in that turn.
- **Stay close to upstream, and never diverge for cosmetics.** Asked in September whether to repaint
  the accent teal in the OrcaMCP purple — about 200 literals across 97 files — the user said no:
  *"I don't want to diverge from upstream just for a color."* Fork-specific artwork such as the
  About wordmark is fair game; shared UI chrome that upstream maintains is not.
- **If you find a bug, it is yours to fix, along with related occurrences.** Every one of the eight
  fixes above came with its sibling: both polling agents, not just Flashforge; every third-party
  printer, not just this one.
- **Test against the running app, not against your reading of the code.** Two wrong diagnoses on
  this project were caught only by instrumenting the real thing. When you claim a crash is fixed,
  reproduce it first, then show it no longer reproduces.
- **Watch for a second app instance.** Two copies fight over port 13618, and an old one crashing
  will look exactly like your fix failing. Check `pgrep -f OrcaSlicer.app | wc -l` and compare the
  crash report's `procLaunch` against the binary's mtime before believing a crash is yours.
- **A green `Build all` is not a release test.** It skips Store packaging, never installs a DMG,
  never signs or notarizes. Only a release run exercises those, and only the artifact check
  proves the result. Both `Build all` and the Release run must be green on the tagged commit.
- **A deferral in a document is not a task.** "Re-applied in a later task -- release CI is
  knowingly broken until then" sat in the sync guide for months and cost three broken releases.
  Either do it in the merge or open an issue that blocks the release.
- **If you edit a plist and the bundle does not change, you edited the wrong plist.**
  `src/dev-utils/platform/osx/Info.plist.in` is configured and never installed; the bundle's plist
  comes from `cmake/modules/MacOSXBundleInfo.plist.in`. An hour was lost to this once.
- **Expect the release's asset upload to be the flaky step**, always after every real check has
  passed. Rerun that one job once; then upload the artifact directly with `gh release upload`.
- **Run Shellcheck with CI's exact command before pushing any shell.** Two scripts written to
  prevent regressions each turned CI red on their first push. `scripts/release-preflight.sh` does
  this for you.
- **Pushes to `mcp` that touch source start an hour-long build.** Check `gh run list` first; a
  docs-only commit takes `[skip ci]`.

## Environment notes

- Fork data directory is `~/Library/Application Support/OrcaMCP/`, **not** `.../OrcaSlicer/`.
  Physical printers live in `user/default/machine/C5P.json` there.
- `FF_CHECK_CODE` is a printer credential. Never echo it, never commit it, never paste it into a
  conversation. Read it from the preset and hand it straight to the test.
- The live hardware test is `tests/slic3rutils/test_flashforge_live.cpp`, tagged
  `[flashforge-live]` and excluded from CI. It is safe on an idle machine with an operator present.
