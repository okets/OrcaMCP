# Release post-mortem: v2.5.0.1-dev, 17 September 2026

Six release runs, roughly ninety minutes each, to ship one version. This is every failure in order,
what caused it, what fixed it, and what would have caught it in seconds. Read the **procedure** at
the end before the next release; it is the distillation.

Written for whoever runs the next release, including the author on a worse day.

---

## The short version

Three root causes account for everything except the transient failures:

1. **`release.yml` never mirrored the CI matrix.** It called the Windows build with the OS alone.
   CI passed `arch` and `compiler`; the release did not. One of those fed a script that validates
   its input, so the release died after a full build. The other meant the Windows binary about to
   ship was built with a different compiler than CI had tested.
2. **The September upstream merge took `--theirs` on `build_orca.yml`** and the sync guide recorded
   that the fork's rebranding would be "re-applied in a later task -- release CI is knowingly broken
   until then". It never was. That single resolution removed three fork customisations that then
   hid each other: the DMG bundle rename, the signing gate, and the quoting of the signing secrets.
3. **A green CI run does not test the release path.** `build_all.yml` skips the Store packaging
   step, never installs a DMG, and never signs or notarizes. So "CI is green" was true before every
   one of these failures and meant nothing about whether the release would work.

Everything in categories 1 and 2 is now asserted by `scripts/check-fork-customizations.sh`, which
runs in CI on every push. Category 3 is addressed by `scripts/release-preflight.sh` and by the
manual artifact check below, because some of it genuinely needs a build.

---

## Timeline of failures

| # | Run | Where it died | Cause | Fix |
|---|-----|---------------|-------|-----|
| 1 | 35180931198 | Windows, "Build MSIX Store package", after a full build | `release.yml` passed no `arch`; the packaging script declares `[ValidateSet("x64","arm64")]`, so `""` failed validation | `release.yml` now passes `arch: x64` and `compiler: clang`, mirroring `build_all.yml` |
| 2 | 35204008163 | Nothing — the run was green | The release was a **draft** and could not be published by the workflow: `A && B \|\| C` falls through to `C` when `B` is false, so the draft toggle was always true | Expression rewritten so tag pushes draft by design and a manual dispatch can publish |
| 2b | same artifact | The user's machine | The DMG contained **`OrcaSlicer.app`** and installed over the real Orca Slicer. The bundle was ad-hoc signed and Gatekeeper refused it | See 4 and 5 |
| 3 | 35219913522 | Cancelled | Shellcheck went red on the newly added guard script (SC2164) | Fixed; verified with CI's exact command and version |
| 4 | 35225609115 | macOS Universal, "Sign app and notary" | `security unlock-keychain` printed its usage and exited 2. The fork had quoted `"$KEYCHAIN_PASSWORD"`; upstream's version does not; the merge took upstream's. A secret with a space word-splits | Quoting restored; two guard checks |
| 5 | 35235124489 | macOS Universal, notarization | `HTTP 403: A required agreement is missing or has expired` — Apple's Program License Agreement needed accepting by the Account Holder | Accepted at developer.apple.com. Pre-flight now probes this in two seconds if a keychain profile exists |
| 5b | same, rerun | "Create Release", after all three assets uploaded | `Headers Timeout Error` from the release action — transient GitHub API | Rerun just that job; nothing to fix |
| 6 | 35249727814 | "Create Release" | `Error saving asset`, then an HTML error page on rerun — the action's upload to GitHub's asset API failing on the 358 MB DMG | Superseded: the remaining-time fix was folded in and a new tag cut |
| 7 | 35260570099 | "Create Release", **four attempts** | Same upload failure. On the last attempt the Linux and Windows assets landed and only the macOS image, the largest, failed | **Uploaded the DMG directly with `gh release upload` from the run's artifact.** All builds, signing and notarization had succeeded every time |

Two more defects surfaced during the investigation rather than as run failures:

| Defect | Cause | Fix |
|--------|-------|-----|
| Signing had been skipped for eight months even before this week | The step was gated on `github.repository == 'OrcaSlicer/OrcaSlicer'` **and** on branch refs; our releases are tag-driven, so `refs/tags/*` matched nothing either way | Gated on `okets/OrcaMCP`, branch list dropped, unsigned fallback made the strict inverse |
| The bundle identifier was upstream's, `com.orcaslicer.OrcaSlicer` | Hardcoded in `cmake/modules/MacOSXBundleInfo.plist.in`. macOS keys Launch Services, preferences and file associations off it, so even a renamed bundle was "the same app" | Made a variable like every other field in that template; set to `com.orcamcp.OrcaMCP` beside the bundle name |

---

## What each failure cost, and what would have caught it

| Failure | Time lost | Catchable in seconds? | How, now |
|---------|-----------|-----------------------|----------|
| MSIX arch | ~95 min | Yes | guard: release passes the Windows arch |
| Wrong compiler | (silent) | Yes | guard: release passes the Windows compiler |
| Draft toggle | ~95 min + confusion | Yes, by reading | fixed; nothing to re-check |
| OrcaSlicer.app in the DMG | a user's Orca Slicer install | Yes | guard: DMG renames the bundle (both paths) |
| Unsigned bundle | a user unable to open it | Yes | guard: signing gated on this repo, not on branches |
| Shellcheck | ~10 min | Yes | pre-flight runs CI's exact command |
| Unquoted secrets | ~95 min | Yes | guard: signing quotes its password secrets |
| Apple agreement | ~95 min | Yes, with credentials | pre-flight: `notarytool history` hits the same 403 |
| Asset upload (`Headers Timeout`, `Error saving asset`, HTML error page) | ~5 min × 6 attempts | No | The `softprops/action-gh-release` upload is flaky on the ~360 MB DMG: it failed on 6 of 7 attempts this week while everything before it succeeded. Rerun the job once; if it fails again, `gh run download` the artifact and `gh release upload` it yourself. |
| ARM Linux runner lost (earlier in the week) | ~60 min | No | rerun the job; look for a step still "in progress" on a failed job |

Roughly seven hours of build time went to failures that a ten-second script now catches.

---

## Traps that cost time without failing a build

- **The dead plist.** `src/dev-utils/platform/osx/Info.plist.in` looks authoritative, is configured
  into the build tree, and is never installed. The bundle's plist comes from
  `cmake/modules/MacOSXBundleInfo.plist.in`. An hour was spent editing the wrong one and rebuilding.
  The reference is now annotated. **If you edit a plist and the bundle does not change, you edited
  the wrong plist.**
- **Two app instances.** A stale OrcaSlicer process from an earlier session crashed during a test
  of a crash fix, making the fix look broken. Two copies also fight over port 13618, so the project
  briefly looked empty. Check `pgrep -f OrcaSlicer.app | wc -l` and compare a crash report's
  `procLaunch` against the binary's mtime before believing a crash is yours.
- **Reading the log with the wrong filter.** A grep for notarization status matched profile
  filenames. Grep for `status: Accepted`, `403`, and `##[error]` specifically.
- **Deferrals in documents.** "Re-applied in a later task" in the sync guide was the origin of
  three of the six failures. A deferral is not a task; either do it in the merge or open an issue
  that blocks the release.

---

## What is in place now

| Thing | What it does |
|-------|--------------|
| `scripts/check-fork-customizations.sh` | Fourteen invariants an upstream merge tends to revert: app names, DMG rename in both code paths, signing gate on this repo and not on branches, unsigned fallback as strict inverse, quoted secrets, release inputs, bundle identifier and its template variable, Connect AI strings, bridge script packaging. Each check names why it matters. Verified by reintroducing the real regressions. |
| CI job `Fork customisations survive upstream merges` | Runs the above on every push in about a second. A merge that drops one fails the build. |
| `scripts/release-preflight.sh <tag>` | The guard, plus Shellcheck with CI's command, YAML parse of all workflows, `version.inc` matches the tag, tag not already on the remote, and an Apple notary credential probe. Says what it cannot check. |
| Sync guide | Leads with the guard command, records what the deferral cost, forbids relaxing a check to make a merge pass. |
| `release.yml` | Windows call mirrors the CI matrix; draft toggle works. |
| `build_orca.yml` | Fork's rename, signing gate and quoting restored, each with a comment saying what it protects. |

---

## What still needs a build to verify

The pre-flight is honest about this. Two things only a release run exercises:

1. That the DMG contains `OrcaMCP.app`, is Developer ID signed, and is notarized.
2. That the MSIX packaging step runs (CI skips it).

So **verify the artifact before publishing.** Download the DMG from the draft and run:

```bash
hdiutil attach -nobrowse -readonly OrcaMCP-*.dmg
ls /Volumes/OrcaMCP                                      # must list OrcaMCP.app, not OrcaSlicer.app
A=/Volumes/OrcaMCP/OrcaMCP.app
/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$A/Contents/Info.plist"   # com.orcamcp.OrcaMCP
codesign -dv "$A" 2>&1 | grep TeamIdentifier            # a real team id, not "not set"
spctl -a -vv -t exec "$A"                                # "accepted" + "source=Notarized Developer ID"
hdiutil detach /Volumes/OrcaMCP
```

The `spctl` line is the one that matters: it is what Gatekeeper will say on a user's machine.

Known, minor: the app *inside* the DMG has no stapled ticket; the DMG itself does, and Gatekeeper
accepts the app on that basis. Stapling the app before creating the DMG would let it validate
offline. Upstream's flow has the same gap. Not a blocker.

---

## The release procedure, distilled

1. `scripts/release-preflight.sh v<version>` — must exit 0. Fix anything it reports first; every
   item costs a ninety-minute build otherwise.
2. Confirm the release notes and that `mcp` is at the commit you mean to ship.
3. `git tag -a v<version> <sha> -m "OrcaMCP v<version>"` and push the tag. The tag **must** equal
   `SoftFever_VERSION` in `version.inc` with a `v` prefix; the pre-flight checks this.
4. Wait for the Release run. **Expect "Create Release" to fail on the asset upload** — it did on
   six of seven attempts this week, always after every build, the signing and notarization had
   succeeded. Rerun that one job once (it reuses the artifacts; minutes, not ninety). If it fails
   again, do not keep rerunning:

   ```bash
   gh run download <run-id> -R okets/OrcaMCP -D /tmp/rel
   cp "/tmp/rel/.../OrcaMCP_Mac_universal_V<version>.dmg" "/tmp/rel/OrcaMCP-v<version>-macos-universal.dmg"
   gh release upload v<version> "/tmp/rel/OrcaMCP-v<version>-macos-universal.dmg" -R okets/OrcaMCP --clobber
   ```

   The action creates the draft before uploading, so a direct upload lands on the right release.
   If a job fails with a step still marked in-progress and no log, the runner was lost; rerun it.
5. **Download the DMG from the draft and run the artifact check above.** Do not skip this. Two
   green runs this week produced artifacts that overwrote a user's Orca Slicer and would not open.
6. Only then publish the draft.

If the notary service returns 403, stop: an Apple agreement needs accepting by the Account Holder
at developer.apple.com. No change to the repository will get past it.

---

## Outcome

**Published 2026-09-17 20:46 UTC**, tag `v2.5.0.1-dev` at `753322a957`, from run 35260570099.
`Build all` was green on that same commit (run 35260498508), including the Linux regression suite.

The artifact check above was run on the downloaded DMG before publishing:

| Check | Result |
|-------|--------|
| Contents | `OrcaMCP.app` + `Applications` link — not `OrcaSlicer.app` |
| `CFBundleIdentifier` | `com.orcamcp.OrcaMCP` |
| `CFBundleShortVersionString` | `2.5.0.1-dev` |
| Signature | `TeamIdentifier=9PCJMHHHK6`, Developer ID Application |
| Gatekeeper (`spctl`) | `accepted`, `source=Notarized Developer ID` |

Seven release runs in total, roughly ten and a half hours of build time, for a release that a
ten-second pre-flight and a five-minute artifact check would now keep to one run plus one upload
retry. The remaining-time fix (`753322a957`) was folded in before the final tag because nothing
had shipped yet.
