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
