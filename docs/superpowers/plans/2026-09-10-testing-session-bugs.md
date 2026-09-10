# Testing session bug log — 2026-09-10

Found while driving the slicer over MCP with the user. **Not fixed yet** — the user
asked to collect them first and fix in a batch.

Machine: Flashforge Creator 5 Pro (`C5P`, 10.0.0.10). Branch `sync-upstream-2.5`.

---

## T1 — `apply_config` rejects a JSON array for a list-typed key

**Severity: high.** This blocks the obvious way to do a common thing, and the
error tells the caller nothing.

`get_valid_config_keys {category: "project"}` reports:

```json
{"key": "filament_colour", "type": "strings"}
```

A caller reads "strings" (plural) and passes an array. It is rejected:

```
apply_config {"settings":[{"type":"project","key":"filament_colour",
                           "value":["#00FFFF","#FF00FF","#FFFF00","#808080"]}]}
-> {"status":"partial","applied_keys":[],"invalid_keys":["filament_colour"]}
```

The same call with the values joined by `;` succeeds:

```
value: "#00FFFF;#FF00FF;#FFFF00;#808080"
-> {"status":"success","applied_keys":["filament_colour"]}
```

So the key is valid, the values are valid, and only the *shape* is wrong — but
nothing in the response says so. `invalid_keys` is used both for "no such key" and
for "wrong value shape", which are different problems for a caller.

**Where to look:** `ApplyConfig` in
`src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp`. It almost certainly dumps
the JSON value to a string and hands it to `ConfigOption::deserialize()`, which for
a `ConfigOptionStrings` expects `a;b;c` and gets `["a","b","c"]`.

**Fix direction:** for any list-typed option (`strings`, `ints`, `bools`, `floats`
— all of which `get_valid_config_keys` already reports), accept a JSON array and
join it with `;` before deserializing. Keep accepting the joined string. Also split
the failure signal so a caller can tell "unknown key" from "value not accepted",
and say what shape was expected.

**Check for related occurrences:** every list-typed project key has this shape —
`filament_map`, `filament_is_mixed`, `flush_volumes_vector`, `flush_volumes_matrix`,
`filament_mixed_*`. Also check whether `set_object_config` and the `print`/
`filament`/`printer` branches of `apply_config` share the code path.

**Note:** the colour validation added by sweep item I runs on `GUIType::color` keys
after deserializing. Confirm it is not what rejects the array — the observed
behaviour is consistent with a plain deserialize failure, but item I is new and
touches exactly this key, so rule it out before assuming.

---

## T2 — `get_presets` still overflows the client, and its filters are unreachable

**Severity: medium.** Sweep item H genuinely worked; this is what is left.

Unfiltered, the response is now ~54,600 characters, down from ~1.9 MB. Still over
the MCP client's per-result limit, so it gets spilled to a file instead of
answered. 318 filament presets are compatible with this printer.

Item H's filters *are* in the running server — the response echoes them:

```json
"query": {"compatible_with_selected_printer_only": true, "name_contains": "",
          "summary": true, "type": null, "vendor": "",
          "counts": {"filamentPresets": 318, "printProcessPresets": 6,
                     "printerPresets": 9}}
```

But the MCP client advertises the **old, parameter-free schema** for `get_presets`
(`"properties": {}`), so the filters cannot be sent from this session. The tool
built to make this response small cannot be told to filter.

**Two separate things here:**

1. **Stale tool schema after an app update.** The client cached the tool list from
   before the app was rebuilt. If a stale schema survives an app restart, every
   user who updates OrcaMCP keeps the old tool signatures until they restart their
   MCP client — and new parameters silently do not exist for them. Establish
   whether a client reconnect clears it; if the bridge caches, fix the bridge. At
   minimum this needs documenting in the upgrade notes.
2. **The unfiltered default is still too big.** Even with `summary: true`, 318
   presets is more than a client will take. Options: cap the default result and
   report the total with a "narrow this with `type`/`vendor`/`name_contains`" hint,
   or default to returning counts plus the query shape and require a filter for
   the list itself.

**Workaround used during the session:** query the spilled file with `jq`.

---

## Not bugs, recorded so they are not re-investigated

- Slot 1 held bronze PETG from the previous print while the project was being set
  up as PLA. Expected: the project and the machine disagree until the filament is
  physically changed or `match_project_to_printer` is run. This is the case that
  tool exists for.

---

## T3 — the bridge falsely reports "OrcaMCP is not running" under concurrent calls

**Severity: high.** It is a false negative that tells an agent to restart a running
application.

Eight `suggest_color_mix` calls were issued in one batch. Six answered normally;
the last two came back with:

```
OrcaMCP is not running. Use the 'start_orca' tool to start it, then try again.
```

The app was **still running** — `pgrep` confirmed the process, there was no crash
report in `~/Library/DiagnosticReports`, and the identical two calls succeeded
immediately afterwards when sent one at a time.

So the liveness check in `scripts/orcamcp-bridge.py` reports "not running" for what
is really a timeout or a refused connection while the server is busy. An agent
following that advice would call `start_orca` on an already-running instance.

**Where to look:** the liveness/health check in `scripts/orcamcp-bridge.py`, and
how many concurrent connections `HttpServer` accepts. Two separate questions:

1. Does the bridge distinguish "connection refused" (really down) from "timed out"
   or "server busy"? It must — the advice it gives differs completely.
2. Does the embedded HTTP server serialise requests such that a burst queues behind
   `run_on_main_thread`? If so, a slow batch is expected and the client needs a
   longer timeout, not a "not running" verdict.

**Reproduce:** issue 8 `suggest_color_mix` calls in one batch against a running
instance.

---

## T4 — `suggest_color_mix` returns an error when the target needs no mixing

**Severity: low, but it breaks palette loops.**

```
suggest_color_mix {"target_color": "#00FFFF"}
-> {"status":"error",
    "message":"target_color already matches physical filament 1 (#00FFFF); no mix needed"}
```

That is a correct and useful answer — "load slot 1, no mix required" — reported as
a failure. An agent walking a set of target colours hits `status: "error"` partway
through and has to special-case the message text to carry on.

**Fix direction:** return `status: "success"` with the recipe expressed as a single
component (the matching slot, ratio 100) and `delta_e: 0`, plus a flag such as
`"exact_match": true`. Keep the message. Reserve `status: "error"` for calls that
could not be answered.

---

## Not a bug: the mix model averages, it does not mix like pigment

Worth recording because it looks like a bug and is not, and because it should be in
the Creator 5 docs.

A mixed slot **alternates layers** of its components, so the result is close to a
weighted average of the RGB values — not subtractive pigment mixing. With cyan,
magenta and yellow loaded:

- magenta + yellow at 40/60 predicts `#FA8B6C` (a salmon), because averaging
  `#FF00FF` and `#FFFF00` gives roughly `#FF9966`. It cannot give red.
- cyan + magenta gives lavender (`#BA44ED`), not blue.

So a CMY set does **not** behave like printer inks, and the reds and blues of the
colour wheel are outside the achievable gamut. Targets near cyan/green/magenta
resolve well (ΔE 3.6–14); pure red was ΔE 54.

Two consequences:

1. `docs/printers/flashforge-creator-5.md` should say what the mix model actually
   is, so nobody buys CMY filament expecting ink behaviour.
2. `get_color_palette` / `suggest_color_mix` responses would be more honest if an
   out-of-gamut result said so, rather than returning a confident recipe with a
   ΔE of 54. Consider a `"gamut": "outside"` marker above some threshold.
