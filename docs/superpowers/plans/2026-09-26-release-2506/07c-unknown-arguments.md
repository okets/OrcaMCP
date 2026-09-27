# 07c — A misspelled or missing argument is refused, not silently ignored

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. Read CLAUDE.md first,
especially "Tool list" and "Threading Model". Also read tests/AGENTS.md before writing tests.
Prompts 01–07, 06b and 07b are merged.

## The bug (found by the orchestrator on 2026-09-27; re-check)

- **What an agent sees:** `scale_object {"object_id": 0, "scale": 0.5}` returns `"status": "success"`,
  and the object is not scaled. The tool takes `x`, `y`, `z` and `uniform`; `scale` is not one of its
  arguments. Nothing tells the agent that its call did nothing. Any misspelled or guessed argument on
  any tool does the same.
- **Why:** every tool's schema says `"additionalProperties": false` (`tool_list_entry`,
  `OrcaMCPServer.cpp:~617`, adds it when a registration leaves it out). But `handle_tools_call`
  (`~662`) passes `arguments` straight to the handler, and nothing checks them against the schema.
- **The bridge's own tools** (`start_orca`, `wait_for_slice`, answered in `scripts/orcamcp-bridge.py`)
  have the same gap.

## Your task

1. **Audit first.** For every registered tool, list each argument its handler reads (`params.value`,
   `params.contains`, `params[...]`, `params.at`, helpers that take `params`) and check that each one is
   in its schema. An argument a handler reads but the schema doesn't declare is a hidden parameter:
   refusing unknown arguments would break it. Declare it (with a description), or remove the read if
   it is dead. Check the bridge too: does it add, rename or pass through any argument (Windows path
   normalization only rewrites values)? Do MCP clients put anything else inside `arguments`
   (`_meta` belongs to `params`, not `arguments`; check the spec version the server declares)?
   In the same pass, list every read of a key that may be absent through `operator[]` on a **const**
   json (`params["x"]` where `params` is `const nlohmann::json&`). In the bundled nlohmann that is only a
   `JSON_ASSERT` and then a dereference of `find()`'s end iterator: undefined behaviour in the release
   build when the caller left the key out. 06c found and fixed one (`transform_objects {}` read
   `params["transforms"]`). The required-argument check below covers required keys; an optional key
   read that way must use `value()` / `contains()` instead.
2. **Design the check, and keep it small.** One function, in one place, before the handler runs:
   - Refuse a top-level argument the schema doesn't declare, when its schema says
     `additionalProperties: false`.
   - Refuse a call that leaves out an argument the schema lists as `required`, naming it (and nested
     `required` keys inside items, e.g. a `settings` item without `value`, if the same walk covers it).
     Check the audit first: a tool whose handler really treats a "required" argument as optional has a
     wrong schema, and that gets fixed in the schema, not by skipping the check.
   - Also check a nested object when its own schema says `additionalProperties: false` (the `settings`
     items `{key, value}`, `configs`), if that stays a small walk over `properties` and `items`.
   - Not a JSON Schema validator: no type, range or enum checks in this prompt.
   - Refuse `arguments` that is not an object.
   - The refusal is JSON-RPC -32602, like an unknown tool. Its message names the tool, the unknown
     argument(s) and the arguments the tool does take, e.g. `scale_object has no argument "scale". Its
     arguments: object_id, x, y, z, uniform, include_preview, preview_resolution, preview_views.`
   - The bridge refuses unknown arguments to its own two tools the same way, from the same schemas
     (the golden file), not from a second hand-written list.
   Present the audit and the design, then stop and wait for a yes.
3. **Related occurrences.** List the tools that return success while changing nothing because an
   argument that selects the change was left out (for example `scale_object` with none of `x`, `y`,
   `z`). Don't fix those in this prompt; report them, and the orchestrator decides.
4. **Tests.**
   - A test over the whole registry: for every app tool, a call with an extra argument
     (`"not_an_argument_of_this_tool": 1`) is refused with -32602 and the handler never runs. No app
     needed: the check comes before the handler. Likewise for every tool with a `required` list, a call
     that leaves one out is refused, naming it.
   - A nested case (`set_object_config` with a `settings` item carrying an extra key), a non-object
     `arguments`, and a call with only declared arguments that reaches the handler.
   - Python: the bridge refuses an unknown argument to `wait_for_slice`, and still accepts `timeout_s`.
5. **Docs.** CLAUDE.md ("Tool list", what the tests enforce) and `docs/tools/reference.md` (the error
   an unknown argument returns). Regenerate the golden file if any schema changed.

## How to work

Verify the bug, audit, design, then stop and present the audit and the design. Wait for a yes. After
that: create branch `rel2506/07c-unknown-arguments` off the latest `mcp`, implement test-first, check
it live (the `scale_object` call above is refused and names `x`, `y`, `z`; a correct call still
scales; a handful of everyday calls with their usual arguments still work), run the suites, and
report back.

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

- Every tool refuses an argument it doesn't take, naming it and the arguments it does take, and a
  call that leaves out a required argument, naming it.
- No handler reads an absent key through a const `operator[]`.
- No tool loses an argument it really reads: the audit shows every read is declared.
- The check lives in one place for the app, and the bridge's two tools use the same schemas.

## Report back

- **Audit:** hidden parameters found and what you did with each; what the bridge and clients send;
  every const `operator[]` read of a key that may be absent, and what you did with it.
- **The check** and where it lives.
- **Related occurrences** (item 3), not fixed.
- **Tests** and **live result.**
