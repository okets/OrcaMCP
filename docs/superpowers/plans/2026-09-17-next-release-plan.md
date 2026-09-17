# Next release plan — v2.5.0.2-dev

Written 2026-09-17, immediately after v2.5.0.1-dev was tagged. For the agent picking this up:
read this whole file before touching anything. The traps section exists because two of the items
below look like one-line fixes and are not.

---

## Where things stand

| Branch | Commit | Meaning |
|--------|--------|---------|
| `mcp` (default) | `b83b42efb7` | What shipped. Fast-forwarded, no merge commit. |
| `main` | `ca668a3bc9` | Exactly `upstream/main`. Tracks Orca Slicer, carries no fork work. |
| `sync-upstream-2.5` | `b83b42efb7` | The release branch. Same commit as `mcp`. |

`v2.5.0.1-dev` points at `b83b42efb7`, which is the exact commit CI ran green on, including the
external slicer regression suite on Linux. `version.inc` reads `2.5.0.1-dev`.

**First thing to check:** did the release workflow finish and publish?

```bash
gh release view v2.5.0.1-dev -R okets/OrcaMCP --json assets -q '.assets[]|.name'
```

Three assets are expected: macOS universal, Linux ubuntu 24.04, Windows. If the run failed after
building, the usual cause is `version.inc` not matching the tag — it did match here, so a failure
means something else and the run log is the place to look.

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

---

## Goal for v2.5.0.2-dev

**Catch up to upstream.** That is the centrepiece and everything else is secondary. Bump
`version.inc` to `2.5.0.2-dev` as part of the release commit, not before.

---

## Task 1 — Sync with upstream Orca Slicer

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

## Task 2 — Flashforge extruder modelling

**Do this after the sync, not before.** Seventeen of the incoming upstream commits touch this exact
device layer, so doing it first means doing it twice.

### The gap, plainly

The Creator 5 Pro has four tool heads, and the printer profile says so. The slicer also keeps a
separate live model of the machine, built from what the printer reports over the network, and that
model says **one** extruder. It says one because nothing ever told it otherwise: the count is only
ever set by a newer message format this printer does not speak, so it keeps the default of one.

So two pictures of the same printer disagree — the profile says four, the live model says one.

When the Send dialog works out which nozzle each filament comes from, it compares the two, sees the
mismatch, gives up, and logs an error. While the dialog is open it does that several times a second.

Two consequences:

- The log fills with `get_mapped_nozzles: total_ext_count not match` (`SelectMachine.cpp:1499`).
- The check it abandoned is the one that warns when a filament is too abrasive for the nozzle.
  **That warning never appears on this printer.** This is a missing safety check, not just noise.

Printing is unaffected.

### Why the obvious fix is wrong

Setting the live model to four extruders breaks something that currently works.
`ExtderSystemParser::ParseV1_0` (`DevExtruderSystem.cpp:191`) returns immediately unless the count
is exactly one:

```cpp
if (system->GetTotalExtderCount() != 1) { return; }
```

That parser is what feeds the Flashforge's nozzle temperatures into the Device tab. Raise the count
and the temperatures stop updating, silently.

Note also `GetTotalExtderCount()` asserts `m_extders.size() == m_total_extder_count`, so the count
and the extruder vector have to move together.

### What the real fix looks like

The count is only set in `ExtderSystemParser::ParseV2_0` (`DevExtruderSystem.cpp:283`), reached
from `DeviceManager.cpp:5446`, inside the newer protocol path gated by `check_enable_np`. Doing
this properly means emitting a version-two `device.extruder` block with per-tool bit-packed fields
(`info`, `filam_bak`, `temp`, `spre`, `snow`, `star`, `stat`, `hnow`) — and having the printer
claim that protocol, which changes far more than extruders.

Budget it as a real piece of work with its own verification, not a patch. Do not silence the log:
the error is telling the truth.

---

## Task 3 — Smaller known items

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

## Environment notes

- Fork data directory is `~/Library/Application Support/OrcaMCP/`, **not** `.../OrcaSlicer/`.
  Physical printers live in `user/default/machine/C5P.json` there.
- `FF_CHECK_CODE` is a printer credential. Never echo it, never commit it, never paste it into a
  conversation. Read it from the preset and hand it straight to the test.
- The live hardware test is `tests/slic3rutils/test_flashforge_live.cpp`, tagged
  `[flashforge-live]` and excluded from CI. It is safe on an idle machine with an operator present.
