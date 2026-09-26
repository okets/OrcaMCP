# 04 — `match_project_to_printer` on Flashforge, and slot colours across a printer switch

You are working on OrcaMCP: a fork of OrcaSlicer with an embedded MCP server, so AI agents can drive
the slicer. Repo: `/Users/hanan/Projects/OrcaMCP`, default branch `mcp`. The server's C++ lives in
`src/slic3r/GUI/OrcaMCP/`; the Flashforge integration is in `src/slic3r/Utils/`.

The user's printer is a Flashforge Creator 5 Pro: a 4-head toolchanger at 10.0.0.100, preset "C5P".
It is reached only through its open LAN HTTP API on port 8898, authenticated by serial number and
check code. Read "Running the live printer test" in CLAUDE.md, and its credential rule.

This is prompt 04 of the v2.5.0.6-dev release. Prompt 01 must be merged first. It adds a single
source of truth for tool text: categories, a generated `get_server_info`, and a golden tools file.

## What happened

These come from the session of 2026-09-26. The transcript is
`~/.claude/projects/-Users-hanan-Documents-3D-Prints/69f5c2a1-ff9b-43a1-aaac-1c1649d85dbc.jsonl`;
times are UTC.

1. **The printer check failed while the printer showed online.**
   - At 04:35:19, `match_project_to_printer {dry_run: true}` returned
     `{"status":"error","message":"curl:Couldn't connect to server:\n\n[Error 7]"}`.
   - Five seconds earlier, at 04:35:14, `get_printers` had reported the C5P online and idle at
     10.0.0.100.
2. **The agent believed the printer switch reset its colours.**
   - Each time a `load_model` swapped in a Bambu printer preset (prompt 02 fixes that), the agent
     then called `select_preset {type: printer, name: C5P}`. Then it wrongly concluded that
     "switching printers reset the slot colours", and set all four slot colours again.
   - What really happens is upstream behaviour. The printer switch applies the colours remembered
     for that printer. Those were whatever was last saved for C5P, which was not what the agent had
     just set with `set_filament_color`.

## Facts (verified 2026-09-26; re-check the lines)

**1. The connection failure.**

- `match_project_to_printer` resolves the Flashforge and calls `ff->fetch_status`
  (`OrcaMCPPrinterTools.cpp:~963-969`), the same path as `get_printer_status` (`~682`).
- That is a POST to `http://<host>:8898/detail` with a 15 s timeout (`Flashforge.cpp:544-550`,
  `679-703`). The host is the edited printer preset's `print_host`
  (`OrcaMCPPrinterUtils.cpp:~252`); in `C5P.json` that is a bare `"10.0.0.100"`, so the URL is
  well-formed.
- `get_printers`' `is_online` is a different, cached signal: DeviceManager's
  `MachineObject::m_is_online` (`OrcaMCPPrinterTools.cpp:~210`). The agent's poll loop never marks
  the printer offline (`FlashforgePrinterAgent.cpp:767-772`), so it can be stale.
- Port 8898 accepted TCP from a shell on 2026-09-26.
- `request_local_api_json` does not log failures, and the log has no Flashforge HTTP lines.
- The cause is **unconfirmed**. The candidates are:
  - a transient error;
  - the firmware refusing a connection while the agent's own poll was in flight;
  - the macOS Local Network permission for a freshly built binary.
- There is a latent bug, not triggered here. `Flashforge::extract_host_name`
  (`Flashforge.cpp:705-712`) keeps a `:port` on a host written without `http://`, producing
  `http://ip:port:8898/…`. The agent's `host_name_of` strips it correctly
  (`FlashforgePrinterAgent.cpp:44-55`).
- Reading the loaded materials already works. `detail.matlStationInfo.slotInfos[]` is parsed
  (`FlashforgeApi.cpp:198-205`, `433-437`) and exposed by `get_printer_status` as
  `material_station.slots`. The agent also caches slots as AMS-style trays
  (`FlashforgeApi.cpp:300-305`).

**2. The colours.**

- `select_preset` for a printer calls `DiscardCurrentPresetChanges`, then `Tab::select_preset`
  (`OrcaMCPPresetConfigUtils.cpp:548-557`).
- With `remember_printer_config: true` (the user's `OrcaMCP.conf`), `Tab.cpp:6914-6918` runs
  `PresetBundle::update_selections`. That overwrites the plate's `filament_colour` with the new
  printer's remembered colours from app config (`PresetBundle.cpp:3035-3041`). The GUI's printer
  combo takes the same path (`Plater.cpp:12291-12294`). **This is upstream behaviour: keep it.**
- The MCP-specific gap: `select_preset` with a filament slot saves the colours per printer
  (`OrcaMCPPresetConfigUtils.cpp:~589`), but `set_filament_color` does not. So colours set through
  `set_filament_color` are lost on the next printer switch.
- `select_preset {type: printer}` returns only `{"status":"success"}`, so the agent could not see
  which colours it got.

## Your task

1. **Diagnose the connection failure first.**
   - Add failure logging to `request_local_api_json`: the URL without secrets, the curl code, the
     HTTP status, and the elapsed time.
   - Run `match_project_to_printer {dry_run: true}` and `get_printer_status`
     against the real printer. Both are read-only; `dry_run` must stay true.
   - Try to reproduce. Check whether it fails only for a freshly built binary (Local Network
     permission), or only while the agent's poll is in flight.
   - Report what you found, even if it doesn't reproduce.
2. **Make the failure recoverable and actionable.**
   - Retry once on `CURLE_COULDNT_CONNECT`.
   - Put `host:port` and a next step in the error. For example: "the printer did not accept a
     connection on 10.0.0.100:8898; if this is a freshly built app, check System Settings > Privacy
     > Local Network".
   - If the live read fails, fall back to the agent's cached material-station slots. Label them
     `source: cached` with their age.
3. **Fix `Flashforge::extract_host_name`** for hosts given as `ip:port` without a scheme. Better
   still, share one host parser with `host_name_of`, so the two cannot drift. Unit-test it in
   `tests/slic3rutils/test_flashforge_api.cpp` or next to it.
4. **`set_filament_color` persists per printer**, the same way `select_preset`'s slot form does, so
   a printer switch brings back the colours the agent set. Keep the save in one shared helper
   (DRY).
5. **`select_preset {type: printer}` reports the result:** the `filaments` array (slot, preset,
   colour), and whether colours came from the printer's remembered set. Update its description,
   then regenerate the golden tools file.
6. **Tests.**
   - Host parsing.
   - Colour persistence: `tests/libslic3r/test_preset_bundle_loading.cpp:~226` already uses
     per-printer app-config fixtures.
   - The retry decision, as a pure function.

**Out of scope:**

- anything that starts, pauses or cancels a print;
- sending files;
- changing upstream's remember-per-printer behaviour.

**Flashforge safety, beyond the ground rules:**

- `FF_CHECK_CODE` and the preset's `printhost_apikey` are credentials. Never echo, log, commit or
  paste them.
- The live printer test (`[flashforge-live]`) is safe only on an idle machine with the user
  present. Ask before running it.

## How to work

1. **Verify.** Read the code at every file:line above; lines may have moved since 2026-09-26.
   Confirm each fact before building on it. If one is wrong, say so.
2. **Design, then stop.** Present a short design to the user: for each item, the approach, the
   files you'll touch and the tests. Include what you plan to try live for item 1. Then wait for
   their explicit yes before writing code. If you are a subagent, end your turn with the design as
   your report; you will be resumed with the answer.
3. **Branch.** Create `rel2506/04-printer-and-colours` off the latest `mcp`, in the main checkout.
4. **Implement test-first:** failing test, change, passing test, commit.
5. **Check live** (see the ground rules). Then run the full `slic3rutils`
   suite and the Python tests.
6. **Report back** in the format at the end.

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

- A connection failure logs its cause and returns an error naming host, port and a next step. A
  transient failure succeeds on the retry.
- `ip:port` hosts build correct URLs.
- The colours set with `set_filament_color` survive a switch to another printer and back.
- `select_preset {type: printer}` shows the resulting slot colours.

## Report back

- **Change list** mapped to the task numbers: done, deferred (with reason) or won't fix (with reason).
- **Commits:** hash and subject.
- **Test results:** new tests and full-suite counts.
- **Diagnosis:** what item 1 found, and whether the original failure reproduced.
- **Bugs found** outside your area.
