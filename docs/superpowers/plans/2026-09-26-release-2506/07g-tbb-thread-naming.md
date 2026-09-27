# 07g — The first slice of a session can hang the app for good

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. Read CLAUDE.md first,
especially "Threading Model" and "Carried upstream fixes". Also read tests/AGENTS.md before writing
tests. Prompts 01–07f of this release are merged.

## The bug (seen by an agent on 2026-09-28; the stack sample is the evidence)

- **What happened:** on a dev build, the first `slice_all` of the session never answered, and no call
  after it did either. The app had to be killed.
- **The stack** (`/private/tmp/claude-501/rel2506-07g/sample_hang_first_slice.txt`, which the orchestrator put in your
  scratch folder):
  - the main thread waits in `BackgroundSlicingProcess::start()` (`BackgroundSlicingProcess.cpp:~550`)
    for the slicing thread to report that it started;
  - the slicing thread, and the TBB workers, wait in `name_tbb_thread_pool_threads_set_locale()`
    (`src/libslic3r/Thread.cpp:~217-280`), on `cv.wait` at `~251`.
- **Why:** that function runs a `tbb::parallel_for` over `max_concurrency()` one-element ranges and
  makes every task wait on a condition variable until ALL of them are running at once. It is a barrier
  across the whole TBB pool. If one TBB thread never takes a task (busy in another arena, asleep, or the
  arena has fewer threads than `max_concurrency()` reports), every waiter waits forever. It runs once,
  on the first slice of a session (`static bool initialized`), from `BackgroundSlicingProcess.cpp:~334`,
  `Print.cpp:~2690` and `SLAPrint.cpp:~694`.
- **Intermittent:** the same sequence sliced normally on the next launch.
- **Upstream:** identical on `upstream/main` (OrcaSlicer, and PrusaSlicer, where it comes from).
- **What the function is for,** and must keep doing: it names each TBB worker thread
  (`slic3r_tbb_<n>`, for debuggers and crash reports) and sets its locale to "C" (`uselocale(newlocale(...
  "C"))`, `_configthreadlocale` + `setlocale` on Windows). **The locale part matters:** G-code and config
  numbers are printed from those threads, and a worker left on the user's locale can write "0,2" for 0.2.

## Your task

1. **Verify** by reading the code: the barrier, its callers, and every place the worker locale is relied
   on. Check whether anything else in the app already sets a TBB worker's locale (a
   `task_scheduler_observer`, a per-thread init), and whether threads that join the pool AFTER the first
   slice (TBB can create workers later) ever get the "C" locale today.
2. **Design the fix, and keep it small.** The usual approach is a `tbb::task_scheduler_observer` whose
   `on_scheduler_entry(bool is_worker)` names the thread and sets its "C" locale, once per thread, as each
   worker joins the arena, with no waiting at all; observe the default arena from app start (or from the
   first slice), and keep `name_tbb_thread_pool_threads_set_locale()` as the entry point that installs it,
   so the three callers don't change. Check the TBB version in `deps/` supports it (oneTBB `task_scheduler_observer`
   with `observe(true)`), that the observer object lives for the whole process, and that it can't run
   before `main` sets up what naming needs. Name threads by a counter, since there's no range index any
   more. Present the design and stop for the orchestrator's yes.
3. **Tests.**
   - A unit test that the entry point returns while every TBB worker is kept busy by other work (the case
     that hung): e.g. occupy the pool from another thread with long-running tasks, then call it on a
     separate thread and wait for it with a generous deadline (a condition deadline like 30 s, never a
     speed assertion). It must hang before the fix and pass after.
   - A unit test that a TBB worker's locale is "C" after the call, even when the process's global locale
     is not (e.g. set a locale with a comma decimal point for the test, and restore it), including a worker
     created after the call if TBB lets you force one (if not, say so).
   - Put them in the libslic3r suite (`tests/libslic3r/`), tagged per tests/AGENTS.md.
4. **Probe letter.** `Thread.cpp` is upstream code: add the next free letter (AG is taken by 07f; check
   CLAUDE.md) with a paragraph saying what to do when upstream changes it.
5. **Upstream.** Say whether this should go on the list for the after-tag upstream review (it hangs the
   GUI too).

## How to work

Verify, design, then stop and present the design. Wait for a yes. After that: create branch
`rel2506/07g-tbb-thread-naming` off the latest `mcp`, implement test-first, run the suites (libslic3r
full, fff_print full, slic3rutils full, Python), check it live (several launches, a first slice each time;
it's intermittent, so also say you can't prove absence live, and rely on the test), and report back.

## Ground rules (shared by every prompt in this release)

- **Read CLAUDE.md first.** Its instructions override defaults.
- **Where to work.** In the main checkout, on your branch. **Not in a git worktree:** `build/arm64`
  is 31 GB, a worktree forces a full app rebuild, and the disk has about 80 GB free. Only one agent
  builds C++ at a time; the orchestrator sequences you.
- **Build.** `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests` for tests,
  `--target OrcaSlicer` for the app. If the tree is stale, the first build reconfigures.
- **Tests.**
  - C++: `build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[your-tag]"`,
    then the whole suite.
  - Python: `python3 -m unittest discover -s scripts/tests -t scripts`.
  - Python tests must be `unittest.TestCase`. C++ tests must not need a running app.
- **Tool text.** If you change any tool's name, description or schema, regenerate the golden tools
  file as CLAUDE.md's "Tool list" section describes (added by prompt 01). A test enforces it, and
  `get_server_info` follows automatically.
- **The app is yours to drive.** The user's work is saved, so you may quit OrcaSlicer, relaunch
  it, and change its scene freely for testing; no need to ask.
  - Launch your build with `open -a build/arm64/src/RelWithDebInfo/OrcaSlicer.app --env ORCAMCP_SKIP_CLOUD_LOGIN=1`.
    Retry on LaunchServices error -600.
  - Close it with the `quit_app` MCP tool, never AppleScript.
  - Load test files from outside `~/Documents`, `~/Downloads` and `~/Desktop` (macOS privacy
    prompts). A scratch directory under `/tmp/claude-501/` works.
  - Before trusting a result, check the running binary is the one you built: a stale instance can
    answer on port 13618.
- **Screenshots: allowed for this weekend session only** (user, 2026-09-26; they keep sensitive windows
  minimized). Use them only to check OrcaSlicer's own state. Capture just the OrcaSlicer window, or
  crop to it, when you can. Delete the images once the check is done, and never quote or describe
  other apps' content. Outside this session the default is no screenshots. Prefer MCP renders when
  they answer the question.
- **The dev build is unoptimized.** `RelWithDebInfo` compiles at `-O0` here (upstream
  `CMakeLists.txt:742-750`), so slicing, especially tree supports, runs 20-70x slower than the release.
  For live checks, slice small models without tree support. Never read a slow slice as a stall or a
  regression without first timing the same case on `/Applications/OrcaMCP.app` on a data-dir copy.
- **Scratch space.** Use a scratch folder named after your prompt, e.g. `/private/tmp/claude-501/rel2506-<id>/`.
  Never read from or copy out of another agent's folder. On 2026-09-27 an agent copied 29 stale files out of a
  shared `final/` folder over the working tree; it was caught and undone.
- **Before any live check: check who holds the port.** Run `lsof -nP -iTCP:13618 -sTCP:LISTEN`. If
  ANYTHING listens, even `*:13618` from `/Applications/OrcaMCP.app`, stop and tell the orchestrator. After
  launching, confirm your own pid is the ONLY listener, and that your calls appear in YOUR build's log,
  before any scene-changing call. The user's release binds all interfaces and a dev build binds 127.0.0.1,
  and macOS lets both listen at once. On 2026-09-27 an agent's new_project reached the user's open app
  this way and discarded their scene.
- **Never launch the default app, and prove you're on a data copy.** Never `open <file>` or anything else
  that makes macOS start `/Applications/OrcaMCP.app` (that's the user's app, on their real data). Always
  `open -a <absolute path of YOUR build> --env ORCAMCP_SKIP_CLOUD_LOGIN=1 --args --datadir <fresh copy>`,
  and load files through MCP. After launch, your build's startup log must appear under `<copy>/log/`, not
  `~/Library/Application Support/OrcaMCP/log/`; if it doesn't, quit it and stop. (2026-09-27: test builds
  ran on the user's real data dir three times, and a test file opened with the default app launched the
  user's OrcaSlicer.)
- **Test MCP behaviour through the `mcp__orca-slicer__*` tools**, not curl.
- **Never call `send_to_printer`**: on Flashforge it uploads *and starts* the print. Never call
  `printer_control` or `print_printer_file`.
- **Fixtures** are small and synthetic. Never use or commit the user's model files.
- **Don't delete the user's presets.** The user's preset library holds embedded Bambu presets from
  the 2026-09-26 session, e.g. `Bambu Lab A1 0.4 nozzle(Kuromi head.3mf)`. Don't delete them
  without asking.
- **Code.**
  - Single responsibility, DRY, small well-named functions. Match the surrounding code's idiom and
    comment density. snake_case functions, PascalCase classes.
  - Stay close to upstream: prefer a small fork-local check over rewriting upstream code. If you
    change an upstream file, add a probe line to CLAUDE.md's "Carried upstream fixes" block.
- **Bugs.** Fix every bug you find in your area, including related occurrences. Report bugs outside
  your area; don't fix them.
- **Commits.**
  - Small, one concern each, subject style `mcp: <what changed, from the user's view>`.
  - Docs-only commits end their subject with `[skip ci]`.
  - End every message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - **Do not push, tag or bump the version.**
- **Docs.** Update CLAUDE.md and `docs/tools/reference.md` in the same commits.

## Done when

- The first slice of a session can't wait on a barrier across the TBB pool.
- Every TBB worker still runs with the "C" locale and a slic3r_tbb name, including workers created later.
- A test that hung before the fix passes after it.

## Report back

- **Verification:** the callers, the locale reliance, and what happens to workers created later today.
- **The fix,** and why it can't wait.
- **Tests** and the **live result.**
- **Probe letter** and the **upstream recommendation.**
