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

## Not a bug: the mix model is neither ink nor an RGB average

Worth recording because it looks like a bug and is not, and because it should be in
the Creator 5 docs.

**Corrected 2026-09-11 (Plan 1 final fix wave).** The first version of this note
said the mixer "averages RGB" and that a CMY set "cannot reach red or blue". Both
were my inference from a handful of results, and both are wrong. `blend_color_multi`
(`FilamentMixerModel.hpp`) is a fitted polynomial blend model. Measured over all 18
pairwise mixes of cyan/magenta/yellow/gray (verified independently by the final
re-reviewer from the code):

- magenta + yellow at 30/70 → `#F9A05A`, hue 26.4°, **inside the red sector**.
- cyan + magenta at 50/50 → `#8077F0`, hue 244.5°, **inside the blue sector**.
- the sectors no pairwise mix reaches are **orange (30–60°) and azure (210–240°)**;
  with three-component mixes, no sector is empty.

What *is* true: the saturated primaries are out of reach — pure red is ΔE 54 from
the nearest mix — and targets near cyan/green/magenta resolve well (ΔE 3.6–14). A
sector being "reached" says nothing about how far a specific target is; that is what
`suggest_color_mix`'s `gamut` / `delta_e` are for.

Two consequences:

1. `docs/printers/flashforge-creator-5.md` should say what the mix model actually
   is, so nobody buys CMY filament expecting ink behaviour.
2. `get_color_palette` / `suggest_color_mix` responses would be more honest if an
   out-of-gamut result said so, rather than returning a confident recipe with a
   ΔE of 54. Consider a `"gamut": "outside"` marker above some threshold.

---

## T5 — there is no MCP tool that can paint a model. This is the missing half of the flagship feature.

**Severity: highest of anything found so far.** Not a bug — a capability gap, and
it undercuts the feature this project leads with.

**What was asked:** paint the scraper in 14 evenly spaced sections along its
length, from the scraping edge to the hanging hole.

**Why it could not be done:**

| Tool | What it does | Why it does not help |
|---|---|---|
| `set_object_filament` | one filament per object, or per *part/volume* | the scraper is a single volume; there is nothing to address |
| `cut_object` | cuts at a **Z height** only | the length runs along Y (122 mm), lying flat. Z-cuts band the 6 mm thickness |
| `set_object_layer_range` | settings per **Z range** | same axis problem: bands through the 6 mm thickness, ~30 layers total |
| `set_object_config` | per-object override | whole object only |

Verified against the source, not from memory:

```
grep -rln "mmu_segmentation\|FacetsAnnotation\|EnforcerBlockerType" src/slic3r/GUI/OrcaMCP/
-> no matches
```

The MCP layer never touches paint data at all. The GUI's multi-material paint
gizmo has no MCP equivalent.

**Why this matters more than the other items.** The stated headline feature of this
project is agentic colour tooling — suggest a mix for a named colour, generate a
palette *to choose from when painting*. `suggest_color_mix` and `get_color_palette`
both work well (proven this session: 16 targets resolved, 14 mixed slots created,
all rendering correctly). But an agent can propose a palette and then cannot apply
it. The user has to pick up the mouse for the actual painting, which is exactly the
step the feature exists to automate.

**Fix direction:** a `paint_object` tool writing `mmu_segmentation` facet
annotations on the `ModelVolume`, the same data the paint gizmo writes. Worth
supporting at least:

- **bands along an axis** — `{axis: "x"|"y"|"z", sections: [{filament, from, to}]}`
  or a simple even split across N filaments. This alone covers the request above.
- **a whole volume** to one filament (already possible, but belongs in the same
  tool for symmetry).
- reading current paint back, so an agent can verify what it did.

Later, and harder: paint by geometric selection (a sphere/box region), or by
surface feature. Bands are the 80% case and are unambiguous to specify.

**Do not** ship the rotation workaround as if it were the feature: standing the
part on end so its length becomes Z makes layer ranges work, but turns a flat 6 mm
part into a 122 mm tower on a 40x6 mm footprint. It slices, it demonstrates the
banding, and it is not a print anyone would run.

---

## T6 — MCP tool coverage audit against the GUI toolbar

Prompted by the user asking what else is missing besides painting. Enumerated from
source, not from icons: `GLGizmosManager::EType`
(`src/slic3r/GUI/Gizmos/GLGizmosManager.hpp`) and the top-toolbar item names in
`GLCanvas3D.cpp`, checked against the 70 registered tool names.

### Top toolbar

| Item | MCP | Notes |
|---|---|---|
| add | `load_model` | covered |
| addplate | `add_plate` | covered |
| orient | `auto_orient` | covered |
| arrange | `arrange_objects` | covered |
| more / fewer | `clone_object` (partial) | adds copies; no "remove one instance" |
| **splitobjects** | **none** | split a multi-body mesh into separate objects |
| **splitvolumes** | **none** | split into parts — *this is what would have made the scraper paintable per part* |
| layersediting | `apply_adaptive_layer_height` (partial) | automatic only; no manual variable-height painting |
| assembly_view | none | view mode, low value for an agent |

### Gizmos

| Gizmo | MCP | Notes |
|---|---|---|
| Move / Rotate / Scale / Flatten | covered | `move_object`, `rotate_object`, `scale_object`, `flatten_object` |
| Cut | `cut_object` (**partial**) | MCP cuts on a **Z plane only**. The gizmo does arbitrary planes, keep-both, and dovetail/connector joints |
| **MeshBoolean** | **none** | union / difference / intersection |
| **FdmSupports** | **none** | support painting |
| **Seam** | **none** | seam painting |
| **FuzzySkin** | **none** | fuzzy skin painting |
| **MmSegmentation** | **none** | colour painting — see T5 |
| **BrimEars** | **none** | brim ear placement |
| **Emboss** | **none** | text on a model |
| **Svg** | **none** | SVG emboss |
| **Measure** | **none** | dimensions/distances between features |
| Assembly | none | view mode |
| **Simplify** | **none** | mesh decimation |

### The efficient observation

**Five of the missing gizmos are the same mechanism.** MmSegmentation, FdmSupports,
Seam, FuzzySkin and BrimEars all write per-triangle facet annotations on a
`ModelVolume` — different annotation, identical machinery. One `paint_object` tool
with a `mode` parameter (`color` | `support` | `seam` | `fuzzy_skin` | `brim_ear`)
closes five gaps at roughly the cost of one.

That makes the build order fairly clear:

1. **`paint_object`** — five gaps, and it completes the colour feature (T5).
2. **`split_object`** (to objects / to parts) — cheap, and it unlocks per-part
   filament assignment, which `set_object_filament` already supports but nothing can
   currently produce parts to use it on.
3. **`cut_object` arbitrary plane** — upgrade the existing tool rather than add one.
4. **`simplify_object`**, **`boolean_object`** — mesh editing.
5. **`measure`** — read-only, useful for an agent checking its own work.
6. **`emboss_text`** / **`emboss_svg`** — creation, largest surface area, least
   essential to the printing workflow.

Assembly view and the manual layer-height painting are view/interaction features
with little agent value; explicitly out of scope unless asked for.

---

## T7 — MCP mutations are not undoable, yet `undo`/`redo` are shipped tools

**Severity: high.** Found 2026-09-10 while writing the batch-2 spec, after Plan 4's
author flagged that the spec's own advice was wrong.

The entire MCP layer contains exactly **two** `take_snapshot` calls:

```
src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:4080   (set_object_printable)
src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.cpp:151  ("Change Filaments")
```

`move_object` translates the `ModelObject` directly and calls `plater->update()`.
No snapshot. The same is true of rotate, scale, mirror, flatten, cut, clone,
delete, arrange, auto_orient, the layer-range tools and the per-object config
tools.

`undo` and `redo` are registered MCP tools. So an agent that makes a mistake cannot
take it back, and calling `undo` does not fail — it silently reverts whatever the
*user* last did in the GUI instead, which is worse than doing nothing.

**Fix:** `plater->take_snapshot(<description>)` before mutating, in every tool that
mutates. `set_object_printable` is the correct existing pattern. Sweep the whole
tool surface, not just the transforms.

**Note:** the batch-2 spec originally told plan authors to copy `move_object` for
this. Corrected in place; every plan now points at `set_object_printable`.

---

## T8 — `apply_config` silently corrupts list values (widens T1)

Found by Plan 1's author while grounding T1 in the code. **T1 is worse than the
session showed.** The rejection we saw was the *safe* case.

- `value.dump()` (`OrcaMCPPresetConfigUtils.cpp:264`) turns a JSON array into the
  literal `["#00FFFF",…]`. `unescape_strings_cstyle` (`Config.cpp:149`) finds no
  separator, stores **the whole literal as one string**, and returns `true` — no
  throw. For `filament_colour` the colour check added by sweep item I catches it and
  restores the old value. That is why we saw a clean rejection.
- **Non-colour `coStrings` keys have no such check.** `filament_notes` and
  `filament_mixed_components` store the array literal as a single garbage string and
  report `status: "success"`.
- **Worse, `ConfigOptionFloats::deserialize` (`Config.hpp:911`) returns `true`
  unconditionally** (line 935) after storing `0` for the element carrying the `[`.
  So `flush_volumes_vector` and `flush_volumes_matrix` accept an array, report
  success, and store **wrong numbers**.

So sweep item I did not cause T1 — it *exposed* it, for one key, by accident.

**Also corrected:** the batch-2 spec said to join array elements with `;`. That is
right only for `coStrings`. `coInts` (`Config.hpp:1089`), `coFloats` (`:911`) and
`coBools` (`:1959`) split on `,`. The separator must come from the option type.

---

## T2 resolved: the stale schema is the bridge, not the client

`scripts/tools_schema.py` is a **checked-in, hand-regenerated** file. Its
`get_presets` entry still reads `'properties': {}`. `orcamcp-bridge.py:354` answers
the first `tools/list` from that file when a 0.1 s probe of the app fails — the
normal case at startup — and `initialize` advertises `"tools": {}` with no
`listChanged`, so the client never asks again.

And **T3's root cause is confirmed exactly:** `check_orcaslicer_connection` probes
with a 0.3 s GET, collapses every failure into "not connected" via a bare
`except Exception`, and caches that `False` for 3 s. It cannot answer in 0.3 s
under load, because `HttpServer` runs a single `io_service.run()` thread
(`HttpServer.cpp:210`) and every handler blocks it inside `run_on_main_thread`'s
`future.get()`. A burst serialises by design.

---

## T9 — smaller findings from grounding the plans in the code

All found by plan authors reading the real source, not from testing.

**`cut_object` has never taken an undo snapshot.** It adds objects, calls
`plater->remove()` and `plater->update()`, with no `take_snapshot`. Worse than the
rest of T7, because `get_server_info` actively advises "use undo if result is
wrong" — the tool tells the agent to rely on something that does not work. Covered
by Plan 3 Task 5.

**`reference.md` documents `cut_object`'s `keep` default as `"both"`; the code
defaults to `"below"`.** The code is the published contract, so the doc is what
changes.

**`GLGizmoAdvancedCut.cpp/.hpp` is dead code.** Not listed in
`src/slic3r/CMakeLists.txt` and no longer compilable — it references
`ModelObjectCutAttribute::CutToParts` and `ModelObject::get_connector_mesh`,
neither of which exists any more. Nobody should read cut behaviour out of it.
Deleting it is a separate janitorial change, not part of batch 2.

**`Plater::split_object(int, bool)` is declared (`Plater.hpp:737`) and never
defined.** Linking against it fails. Plan 3 avoids it and goes to the `Model` API
directly.

**`ModelObject::make_boolean` is unusable for an MCP boolean tool.** It calls
`this->mesh()`, which merges once per instance, and applies no instance transform
to either operand.

---

## T6 addendum — `merge` and `merge_volumes` were missing from the audit

The coverage audit listed what the toolbar and gizmo bar expose. It missed the
inverse of `split_object`: OrcaSlicer can **merge** objects into one, and merge
volumes into one. Real APIs exist. They belong in the same tool family as
`split_object` and were simply not on the toolbar row that prompted the audit.

Not added to any batch-2 plan — raise with the user, since scope for this batch was
agreed as "the twelve missing tools" and this is a thirteenth and fourteenth.
