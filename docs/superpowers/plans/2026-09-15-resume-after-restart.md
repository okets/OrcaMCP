# Resume note — 2026-09-15, after the machine restart

Written just before the user restarted the machine. Branch `sync-upstream-2.5`, working tree clean.

## The one thing to do first

Start the slicer, and **answer the macOS keychain prompt if it appears**:

> OrcaMCP wants to use your confidential information stored in "OrcaSlicer/Auth" in your keychain.

Until it is answered the MCP HTTP server never starts and no tool can reach the app. It did *not*
appear on the three restarts tonight, so it may not appear again — but if the MCP server does not
come up within about ten seconds, that dialog is why.

```bash
open /Users/hanan/Projects/OrcaMCP/build/arm64/src/Release/OrcaSlicer.app
```

`osascript ... to quit` **hangs** while that modal is up. Use `pkill -x OrcaSlicer`.

## Then, to get back to where we stopped (about 3 minutes)

1. `load_project /Users/hanan/Downloads/frame-abs-4plate-ready.3mf`
2. `slice_all {all_plates: true}`, wait ~2.5 min, poll `get_slicing_status` until `state: "done"`
3. Expect 4/4 plates valid, `active_warnings: 0`

Slice results are not stored in the 3MF, which is the only reason step 2 is needed. Everything
else — plate layout, per-object filament assignment, the single-material support config, the
user's own plate-2 arrangement — is in the file.

## The agreed plan

**Print plate 1.** The user chose "Hold — don't send yet" earlier only because the machine needed
restarting; the message before the restart was *"we will resume this session and printing the first
plate after the restart."* Confirm before sending — it is still a real print on real hardware — but
the direction is settled.

- Plates 1-3 are dark gray ABS on filament slot 2. Station slot 2 is the only loaded slot, so the
  material mapping is unambiguous.
- **Plate 4 is the red one and must wait.** The user has no filament dryer yet (Amazon) and will
  load the fire engine red only when plate 4 is about to start, rather than leave an unwrapped
  spool idle for a day.
- The send dialog may flag a colour mismatch: the machine reports its ABS as `#8C8C89` (mid gray)
  while the project carries `#3A3A3A` (dark gray). Metadata only; it changes nothing in the print.

Estimates, all with **zero tool changes and zero filament changes**:

| Plate | Time | Filament | Weight |
|---|---|---|---|
| 1 | 7 h 26 m | 2 (gray) | 123 g |
| 2 | 6 h 29 m | 2 (gray) | 128 g |
| 3 | 7 h 05 m | 2 (gray) | 145 g |
| 4 | 4 h 51 m | 3 (red) | 73 g |

Total ~26 h, 469 g.

## Why we are printing now: the release gate

The user's words: *"The next thing we are testing is that the device tab reporting properly. If it
works as expected, we will release. I have few of my friends waiting for the new release."*

Device tab reporting is verified everywhere it can be **without** a print running:

- **Data contract** — all 17 `raw` fields the console page reads are present in the live printer
  payload and all 17 survive `console_raw_detail`'s allowlist; every structured field it reads
  (state, temperatures, material station, door, light, camera, job) is present too.
- **Console logic** — 25 cases / 238 assertions.
- **Real hardware** — `[flashforge-live]`, 26 assertions against the Creator 5 Pro: status fetch
  with 4 nozzles, chamber light toggling and reading back both ways, G-code file listing, and the
  temperature semantics the page depends on (setting the bed must not reset a nozzle target; an
  explicit 0 means off, while an absent value means leave alone). Printer left idle, all targets 0.

**The remaining gap, and the reason to print:** the job card — progress, current layer, time
remaining, file name — only populates while something is printing. It is the one half of the Device
tab nobody has seen with real data. Starting plate 1 exercises it within a minute or two.

So: send plate 1, then watch the Device tab's job card and confirm progress, layer count and time
remaining move sensibly. That closes the release gate.

## What changed tonight (6 commits, all pushed)

| Commit | What |
|---|---|
| `8d997f9e35` | First write-up of the crash — **superseded, the mechanism in it is wrong** |
| `0305157112` | The real fix: slicing an already-sliced plate dereferenced a null `Print` (T14) |
| `8c83f7695c` | `get_print_estimate` ignored `plate_index`; `on_bed` used the selected plate (T15, T16) |
| `11be54df16` | Strict integer checks refused calls the caller made correctly (T17) |
| `6073588c3b` | This fork's data dir is `OrcaMCP`, not `OrcaSlicer`; how to run the live printer test |

Findings T14-T17 are in `docs/superpowers/plans/2026-09-10-testing-session-bugs.md`.

Suites: fff_print 151 cases / 3011 assertions, slic3rutils 334 cases / 2394 assertions, both green.

## Open, not blocking the release

- **CI has never run on these commits.** The branch is pushed, but `build_all.yml` watches only
  `main`, `mcp`, `release/*` and `belt-printer`. Trigger it explicitly:
  `gh workflow run build_all.yml -R okets/OrcaMCP --ref sync-upstream-2.5`
- **The untested request-parsing layer** in `OrcaMCPPaintTools.cpp` (~200 lines). Both Critical bugs
  on this branch lived there, three reviewers flagged it, and T17 was another parsing bug. The
  reason it was deferred — it would have meant refactoring the file about to be tested live — has
  expired. This was the recommended next increment before the Device tab work took priority.
- **`slice_all` has no guard against being called while a slice is in flight.** Deliberate: with
  the null dereference fixed, a second slice is no longer a crash.
- **`on_bed` still only means "inside the plate in XY and not sunk"** — no collision check, and a
  part floating above the bed passes it.
- **Brim for the ABS parts** was raised and never decided. `brim_type` is `no_brim` and parts are up
  to 120 mm tall. Worth a decision before plate 1 goes, if the user wants one.
