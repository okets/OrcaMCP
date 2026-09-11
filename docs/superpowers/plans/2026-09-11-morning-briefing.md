# Morning briefing — 2026-09-11

Written overnight. Branch `sync-upstream-2.5`. Nothing is pushed to `mcp`, nothing is tagged,
no release exists.

---

## Do this first (30 seconds, only you can do it)

Start the slicer and **answer the macOS keychain prompt** that appears:

> OrcaMCP wants to use your confidential information stored in "OrcaSlicer/Auth" in your keychain.

Click **Always Allow** and enter your login password. Until that is answered the MCP HTTP
server never starts, so no tool can reach the app.

This is not a bug we introduced. Rebuilding changes the binary's code signature, so macOS
treats it as a new application and re-asks. It will happen again after every rebuild, and it
will happen to users once after every upgrade.

I could not answer it for you — I do not have your password, and clicking *Deny* on your
behalf would change your security posture and could break printer authentication later.
I quit the app rather than leave a password dialog open on an unattended machine.

**One gotcha if you script around this:** `osascript -e 'tell application "OrcaSlicer" to quit'`
**hangs** while that modal is up, because the app cannot process the AppleEvent. Use `pkill`.

---

## What is ready for you to test

The painting feature is complete: **`paint_object`, `get_object_paint`, `clear_object_paint`,
`set_brim_ears`**. Tool count is **74 registered, 75 reachable**. Suite: **257 cases, 1827
assertions, 0 failures**.

All four tools now answer with the same shapes. A volume reads the same way whichever tool
returned it — `{volume_id, name, original_facets, bounding_box, modes}` — and all three tools
that report a bounding box report the *same* box, computed the same way. That consistency was
the last thing fixed overnight, so if you learn one tool's response you can rely on the others.

The acceptance run — the scraper painted in 14 bands end to end — is the one thing not done,
because it needs the app running. The script is written and ready:
`.superpowers/sdd/2026-09-11-batch2-plan2-painting/task-13-brief.md`. It loads the scraper,
makes 18 filament slots, paints 14 bands along Y, reads the paint back, renders it, slices,
and round-trips a 3MF. Say the word and I will run it.

---

## What CI says

Run `34512872653` — **green on all five platforms**: macOS arm64 and Universal, Windows x64
and arm64, Linux x86_64 and aarch64, including every unit-test job and the slice check.

That run predates the last nine commits. I will re-trigger on the current head.

**Note for future sessions:** pushing this branch does **not** trigger CI. `build_all.yml`
watches `main`, `mcp`, `release/*` and `belt-printer` only. Use:

```
gh workflow run build_all.yml -R okets/OrcaMCP --ref sync-upstream-2.5
```

---

## Bugs found and fixed overnight

The whole-branch review returned **ship** — no Critical findings — then found 3 Important and
12 Minor, all now fixed. The four that would have reached you:

1. **A phantom filament on multi-material prints.** Clearing paint from one part of a
   multi-part object emptied the annotation but left an internal state flag set, so the slicer
   still counted that filament — loading and purging it for nothing. An earlier ruling had
   filed this as an upstream problem shared with the GUI. That was wrong: the GUI clears every
   volume at once, which hides it. Our per-volume clear is the only path in the application
   that triggers it.

2. **A bounding box that would have made today's test lie.** `get_object_info` reports a
   looser, differently-computed box than the paint tools use — it is the AABB of the *corners*
   of the untransformed AABB, unioned over every instance. Three places told an agent to band
   from it. On a rotated model that silently gives 26/47/26 mm when you asked for equal thirds.
   `paint_object` now reports its own box in every response, and the prose is corrected.

3. **Undefined behaviour from a plain tool call.** `set_brim_ears` bounded an ear's radius but
   never its position. Slicing scales a stored ear into a 32-bit integer, so any coordinate past
   ~2147 mm overflowed it — and persisted into the 3MF, so the file stayed poisoned. Passing
   microns where millimetres were meant is already a hundred times over the limit. Now bounded.

4. **A release blocker hiding inside a green CI run.** The Windows step that uploads the
   installer globbed for `OrcaSlicer*.exe`. This fork's CPack names it
   `OrcaMCP_Windows_Installer_V<version>_<arch>.exe`, so it matched nothing, uploaded an empty
   artifact, and reported success. A release would have built for an hour and then found no
   Windows installer. This is the second half of the rename bug fixed in `7c821fe9c6`. The glob
   now derives from `version.inc`, and both Windows uploads fail hard on an empty match.

---

## A correction to my own work

I wrote in the docs that clearing an annotation differs from painting every facet `none` —
one leaving no data, the other a bitstream of zeroes. **That is false.** A test written for
an unrelated fix disproved it: the serializer stores a triangle only if it is split or
non-NONE, and the write path undivides first, so painting all-none serialises to nothing,
exactly like a clear. An implementer caught it and corrected the prose; a reviewer then
verified it independently, including for the `replace: false` case that could have broken it.

---

## Decisions waiting on you

**1. The untested parsing layer (review finding M8).** The ~200-line request-parsing layer in
`OrcaMCPPaintTools.cpp` has no automated coverage, and **both Critical bugs found on this
branch lived there**. Most of it needs no wx and could be extracted into the tested files.

I did not do it overnight on purpose: it is a refactor of the exact file you are about to
test, for no currently-observed defect, and it would have changed the thing under test. It is
the largest remaining gap. Your call whether it goes before the release or after.

**2. What comes next.** Plans 3 and 4 remain — mesh editing (12 tasks) and creation/measurement
(12 tasks) — which you chose as full toolbar parity before release. I froze the branch at
Plan 2 rather than landing 24 more tasks overnight, so what you test is what CI checked.

**3. Two things deliberately left alone.** The `undo`/`redo` tools still do not undo most MCP
operations, because the MCP layer takes almost no snapshots (logged as T7). And one path to
the phantom-filament symptom survives through the GUI's own "Remove color painting" — that
cause is genuinely upstream, in `FacetsAnnotation::reset()`.

---

## Where the detail lives

| What | Where |
|---|---|
| Every ruling, finding and commit, in order | `.superpowers/sdd/2026-09-11-batch2-plan2-painting/progress.md` |
| The whole-branch review | `.../branch-review.md` |
| The re-review of the fixes | `.../rereview-fixes.md` |
| The acceptance script | `.../task-13-brief.md` |
| Live-testing findings from 2026-09-10 | `docs/superpowers/plans/2026-09-10-testing-session-bugs.md` |
