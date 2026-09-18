# Primer prompt for the next session

Paste the block below as the first message of a fresh session. It is written to be read by the
agent, not by a person, so it is blunt and front-loads the things that are expensive to rediscover.

---

You're picking up OrcaMCP: a fork of Orca Slicer that embeds an MCP server so AI agents can drive
the slicer. I'm Hanan. I own a Flashforge Creator 5 Pro and I test everything on real hardware.

We just shipped v2.5.0.1-dev — nine fixes found by driving the printer, plus the macOS bundle
renamed and given its own identifier, signing and notarization actually running, and the About
page rebranded. It took seven release runs. `docs/release/2026-09-17-release-postmortem.md` says
why, and what now catches each cause in seconds: read it before touching anything under
`.github/workflows/`. This session is the follow-up: catch up to upstream, and start contributing
our fixes back so the diff stops growing.

**Read this before doing anything:** `docs/superpowers/plans/2026-09-17-next-release-plan.md`. It is
the handoff from the last session and it is the authority. Everything below is a summary of it.

**First, confirm the release actually published:**

```bash
gh release view v2.5.0.1-dev -R okets/OrcaMCP --json assets -q '.assets[]|.name'
```

Three assets expected: macOS universal, Linux, Windows. If it did not publish, that is problem one.

**Where things stand.** The tag `v2.5.0.1-dev` is at `753322a957`, what shipped. `mcp` is the
default branch and sits a few docs/CI commits past it (`89eb8b12e0` at time of writing — verify with
`git fetch && git rev-parse --short origin/mcp`). `main` sits exactly on `upstream/main` and carries
no fork work. `mcp` is 208 commits behind upstream, and 64 files were changed by both sides — that
overlap is the entire cost of the sync.

**Two scripts are not optional.** `scripts/check-fork-customizations.sh` asserts 19 things upstream
merges keep reverting; run it after the merge and before any tag. `scripts/release-preflight.sh
v<tag>` must exit 0 before any tag is pushed. And before publishing, download the DMG and check it
the way the post-mortem shows — two green CI runs shipped an image that overwrote my Orca Slicer.

**The four stages, in this order:**

1. **Identification — already done.** The plan lists six of our fixes that are pure upstream bugs,
   each verified still present in `upstream/main`. Re-verify before opening anything; upstream moves.
2. **Sync with upstream.** Merge, do not rebase. Prove no regression on the fork afterwards: unit
   suites, then the external regression suite, then the profile slice sweep.
3. **Fix the extruder count gap** on top of merged code, and send it upstream. It is upstream's bug,
   not ours — their own Moonraker agent has it too. The plan explains why the obvious one-line fix
   silently breaks nozzle temperatures.
4. **Send the other six fixes upstream**, one PR each, after the merge so they apply to current code.

**Rules I do not want re-litigated:**

- A release needs a fully green CI run on the exact commit being tagged. Not "green except one known
  case". If the branch moves after the run, that run no longer counts.
- The tag must equal `SoftFever_VERSION` in `version.inc`, with a `v` prefix, or the release job
  fails after a full build.
- Never push a tag, open a PR, or push to a shared branch without me saying so in that turn.
- Stay as close to upstream as possible, but never by dropping a fork feature or a stability fix.
  Never diverge for cosmetics — I turned down repainting the accent colour for exactly this reason.
- If you find a bug, it is yours to fix, along with its siblings. Every fix last session had one:
  both polling agents, not just Flashforge; every third-party printer, not just mine.

**How I want you to work:**

- Test against the running app, not against your reading of the code. Two wrong diagnoses last
  session were caught only by instrumenting the real thing. When you tell me a crash is fixed,
  reproduce it first, then show it no longer reproduces.
- Short increments. Tell me what you found before you act on it, and give me a recommendation rather
  than a menu.
- Watch for a second OrcaSlicer instance. Two copies fight over port 13618, and an old one crashing
  looks exactly like your fix failing. Check `pgrep -f OrcaSlicer.app | wc -l`, and compare a crash
  report's `procLaunch` against the binary's mtime before believing the crash is yours.

**Environment:** the fork's data directory is `~/Library/Application Support/OrcaMCP/`, *not*
`.../OrcaSlicer/` — physical printers live in `user/default/machine/C5P.json` there. `FF_CHECK_CODE`
is a printer access credential: never echo it, never commit it, never paste it into the conversation.
