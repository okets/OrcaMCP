# Morning note — 2026-09-16: releasing v2.5.0.1-dev

**Updated late morning.** The one red case is fixed, not documented around: the wipe-tower placement
clamp was missing upstream's comfort-margin commit. Found by building `upstream/main` and running the
identical case on both binaries — same estimate to the digit, clamp 14 mm apart. Details in T20.

Also done this morning on request: the Device page shows only Obico's primary camera (the "side view"
is gone).

The user's rule for this release, recorded: **CI must be fully green on the exact commit, then tag.**
No known-open failures.

## What to do now

1. Wait for the CI run on the current tip (run id given in chat when triggered) to finish **green**:
   19 jobs, and the Linux *"Run external slicer regression tests"* step at **0 failed**.
2. Tag **that exact commit** — the one the run's `headSha` names, not `HEAD`:

       cd ~/Projects/OrcaMCP
       git tag v2.5.0.1-dev <sha CI tested>
       git push origin v2.5.0.1-dev

   `version.inc` already reads `2.5.0.1-dev`. `release.yml` rebuilds every platform (~1 h) and
   publishes a **draft**; nothing is public until you press Publish.

## Check these first

**1. CI on the real tip.** Two runs matter:

| Run | Commit | What it says |
|---|---|---|
| `35005568253` | `a6ed29bb17` | **Done, 1 failure of 82** — the three original ports fixed 7 of the 8 red cases. This run is the proof of that; it predates the tower work. |
| `35016354227` | `2b0a9fa2d3` | **The current tip**, queued at 03:35. Includes the tower chain below. Expected: same single failure, or green if a Linux build behaves differently from mine. |

    gh run view 35016354227 -R okets/OrcaMCP

**2. The print.** Plate 1 finishes around **09:20** — chamber held 60 all night. `get_printer_status` or the
Device tab.

## The one open case (RESOLVED — see T20; kept for the record)

`mixed-filament-defined-on-cli-slices`: a bare STL, sliced from the CLI with a mixed filament whose two
components are the *same* PLA preset, on the X1C profile. Ours exits 154 (`CLI_GCODE_PATH_IN_UNPRINTABLE_AREA`,
plate-area bit). Upstream's test repo marks it must-pass, so upstream's build slices it.

What I established, each step verified in the produced G-code rather than inferred:

- The tower is **not** the problem any more. After the tower chain (below) the clamp places a tower whose body
  and brim end at Y 254.6 on a 256 mm plate — inside the +2 mm tolerance. Before the chain the tower reached
  Y 276.9; that part is genuinely fixed.
- The failing positions are **1188 extruding moves per tool at exactly X 20, Y −3**, inside the prime-tower
  block, once per filament change. Y −3 is 1 mm past the tolerance.
- They come from the **X1C `change_filament_gcode` macro itself** — `G1 X20 Y50 / G1 Y-3 / … / G1 E18` — the
  vendor's own purge at the front edge, on every swap. That macro is byte-identical to upstream's (md5 checked),
  and so are the filament and process profiles the test builds its datadir from.
- The code that would exclude those purges as "custom" is also near-identical to upstream: `GCode.cpp` differs
  by 33 lines, `GCodeProcessor.cpp` by 15, none of them about role tagging or this check. The processor writes
  the Custom role only around start/end G-code, in both trees. Neither tree treats `M620…M621` as a custom
  region.

So: **on our build every X1C filament change purges 1 mm outside the plate check's tolerance and is counted; upstream
evidently is not counted, and I could not locate why** in any diff small enough to read tonight. The honest
next step is to build upstream/main and slice the same case to compare — an investigation, not a port, and not
one to start at 3 a.m. before a release.

**Scope of the impact if you release with it open:** CLI-only. It is the CLI's post-slice check
(`-102` exit); the GUI and the MCP path do not run it. It affects mixed-filament plates sliced from the
command line on Bambu profiles whose change macro purges off-plate. Nothing about the Creator 5 path is
involved. Your call whether that is a release blocker for a `-dev` tag your friends are testing.

## If you decide to release

`version.inc` already reads `2.5.0.1-dev`; the tag must match exactly, `v`-prefixed, and should name the
commit CI tested, not `HEAD`:

    cd ~/Projects/OrcaMCP
    git tag v2.5.0.1-dev 2b0a9fa2d3
    git push origin v2.5.0.1-dev

`release.yml` rebuilds every platform (~1 h) and publishes a **draft**. Nothing is public until you press
Publish. The previous `-dev` tag shipped a Linux AppImage, a macOS universal DMG and a Windows installer through
this exact path. If run `35016354227` is red on the one case above, the release build will report the same
single failure in the same Linux step — it does not stop the artifacts being built, but do read the job before
publishing.

## What landed tonight, in order

All upstream authorship and `(cherry picked from …)` provenance preserved. None touch fork features
(MCP, Flashforge, our mixed-filament UI); the one fork-side edit is the adaptation noted.

| Commit | What |
|---|---|
| `ad8372b5a8` | #15636 — mixed-filament CLI gate (two trivial conflicts resolved) |
| `e13bc0772a` | CI: external suite **pinned** to `orca-test-repo@d7b53ac`, SHA echoed in the log |
| `3182ba08ab` | #15438 — `--load-filaments` resolves the inherits chain (clean) |
| `a6ed29bb17` | #15639 — short per-filament vector keeps its first value (clean) → **CI run 35005568253: 1 failure** |
| `f88fa6bfc7` | Extract and Unify Wipe Tower Estimation (one header hunk; **dependency I had first skipped** — see below) |
| `bebd54362b` | `[CLI]: Place Wipe Tower before Slicing` (clean, re-applied in upstream order) |
| `4b6df13cf7` | **fork edit**: `get_scene_info`'s prime-tower reader adapted to the new 6-arg estimator |
| `ca0becfbc0` `f844a64850` `4def1a09a8` | estimator: review fixes; size from the planners; no-purge tower at idle depth (all clean) |
| `2b0a9fa2d3` | #15666 — toolchange label mask cleared when an extruder has no instances (clean, 9 lines) |

Suites at the tip: libslic3r **370 cases / 58 440**, fff_print **156 / 3 033**, slic3rutils **2 475 assertions** — all green.

## Two mistakes of mine worth knowing about

- I ported `Place Wipe Tower before Slicing` **before** the estimator refactor it depends on and got a segfault
  upstream never had (`PartPlate::get_extruders(bool)` → `wxGetApp()` through a null app). The crash report named
  it; I reverted, took the refactor first, and re-applied in upstream's order. Lesson recorded in the commits.
- I ran the external suite on **macOS** and chased ten failures that were artefacts: `record_exit_reson` is
  compiled `#if defined(__linux__)`, so `result.json` never exists here and every check on it fails by
  construction. The workflow runs the suite on Linux for exactly that reason. **Only Linux CI is authoritative
  for this suite** — the first CI run proved it: one failure where I saw eleven.

## Worth saying in the release notes

**A profile fix does not reach projects you already saved.** A 3MF carries its own config and `load_project`
restores it over the system profile. Anyone with a saved Creator 5 ABS project must re-apply the start G-code (or
re-create the project) to get chamber heating (T18a).

## After the release — not blocking it

- The open case above, done properly: build upstream/main, slice the case, diff what the processor sees.
- **Full upstream sync**: ~190 commits behind. Tonight's ports were the CLI-relevant slice; your stated direction.
- `get_print_estimate`'s `layer_count` is wrong (1240 vs the printer's correct 766).
- `obico.configured` flickers to `false` right after `send_to_printer`; values intact on disk; restart clears.
- Plate 4 needs the red spool loaded before it can print.
