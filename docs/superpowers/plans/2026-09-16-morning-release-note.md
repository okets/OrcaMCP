# Morning note — 2026-09-16: releasing v2.5.0.1-dev

Written last night while you went to bed. The print is running, CI is running, the tag is **not**
pushed — that is yours to do, and it is the only step left.

## Check these two things first

**1. Full CI on the release commit.** Run `35005568253`, `build_all.yml`, commit `a6ed29bb17`:

    https://github.com/okets/OrcaMCP/actions/runs/35005568253

    gh run view 35005568253 -R okets/OrcaMCP

Nineteen jobs. The one that was red before is Linux → *"Run external slicer regression tests"*.
It now runs the suite **pinned at `d7b53ac`** and the log prints the SHA it checked out.

**2. The print.** `get_printer_status` — or the Device tab — should show plate 1 finishing around
**08:30–09:00**. Chamber held at 60 all night if the fix did its job; the printed part should
show no lifted corners or split layers.

## If CI is green: push the tag

`version.inc` already reads `2.5.0.1-dev`. The tag **must** match it exactly, `v`-prefixed:

    cd ~/Projects/OrcaMCP
    git tag v2.5.0.1-dev a6ed29bb17
    git push origin v2.5.0.1-dev

That triggers `release.yml`, which rebuilds all platforms (~1 h) and publishes a **draft**
release — nothing is public until you press Publish. The previous `-dev` tag, `v2.4.0.1-dev`,
shipped a Linux AppImage, a macOS universal DMG and a Windows installer through this exact path.

Tag a specific commit, not `HEAD`, so a stray morning commit cannot move the release.

## What went into this release since the last green CI (6c672ad2e9, 2026-09-14)

Findings T14–T19 in `docs/superpowers/plans/2026-09-10-testing-session-bugs.md`. Headlines:

- **Slicing an already-sliced plate segfaulted the app** (null `m_print`) — fixed, with a
  single-threaded regression test.
- **ABS printed with the chamber heater off** — the shipped Creator 5 profile hard-coded
  `M191 S0`, which also suppressed the slicer's own chamber command. Fixed in all six profiles;
  verified as `M191 S60` in exported G-code and as chamber 60/60 on the machine.
- **Two query tools answered about the selected plate, not the one asked about**
  (`get_print_estimate`, `on_bed`) — fixed.
- **Strict integer checks refused valid calls** in five tools — fixed via `parse_integer_param`.
- **Obico as the console's camera and watch source**, with fallback to the printer's single
  stream — the Obico agent's work, six commits.
- **Three upstream fixes ported** so the external suite passes: #15636 mixed-filament CLI gate,
  #15438 `--load-filaments` inherits resolution, #15639 short-variant-column broadcast. None touch
  fork features. Upstream authorship and `(cherry picked from …)` provenance preserved.
- **CI pinned** to `orca-test-repo@d7b53ac` so a red board is attributable to *this* repo.

Suites at the release commit: libslic3r 349 cases / 58 275 assertions; fff_print 151 / 3 011;
slic3rutils 340 / 2 443 — all green.

## Worth saying in the release notes

**A profile fix does not reach projects you already saved.** A 3MF carries its own config and
`load_project` restores it over the system profile. Anyone with a saved Creator 5 ABS project must
re-apply the start G-code (or re-create the project) to get chamber heating. This bit us last
night with our own file. (T18a.)

## After the release — not blocking it

- **Full upstream sync.** We are 199 commits behind `upstream/main` (fork point 2026-09-07). The
  three ports cover the CLI failures; the rest is your stated direction and is its own increment.
- **Our `layer_count` is wrong.** `get_print_estimate` says 1240 for plate 1; the printer says 766,
  and 92 mm at 0.12 mm *is* 766. Ours is the bug.
- **`obico.configured` flickers to `false`** in `get_printer_status` right after `send_to_printer`,
  while the values stay intact on disk; a restart clears it. In-memory read, not data loss.
- **Two agents on one repo.** The Obico agent committed to `docs/printers/` last night while I was
  working here. It went fine, but it is worth knowing both sessions push to the same branch.
- **Plate 4 needs the red spool** loaded into a station slot before it can print.
