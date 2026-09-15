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

---

## T10 — painting a high-poly mesh blocks the whole MCP server past the bridge timeout

Found 2026-09-11 while painting a ComfyUI-generated figurine, live, with the user.

The model carries **4,319,160 facets** (the print-bed scraper used for the Plan 2 acceptance run
has 1,898 — about 2,300x smaller). On it:

- `paint_object` with 12 bands along Z took roughly **three minutes**.
- The bridge gave up at its 120 s default and returned *"OrcaSlicer did not answer within 120s"*.
- The operation had **not** failed. It completed, correctly, after the error was reported.
- While it ran, `OrcaSlicer` sat at **97.5% CPU** and the HTTP port accepted no connection at all:
  a bare `curl` to `/mcp` timed out. Every other tool was unreachable for the duration, because
  handler bodies run inside `run_on_main_thread()` and the server serves one request at a time.

So the agent is told the call failed, cannot poll to find out otherwise, and cannot do anything
else until it finishes. An agent that retries on that error will queue a second three-minute paint
behind the first.

Three separable problems:

1. **The work is too slow for the mesh size.** The whole-branch review already logged the shape of
   this as M10 and M11 (`get_object_paint` with no `mode` builds four `TriangleSelector`s;
   `paint_object` computes `facet_centroids` even for `selection: "all"`, which needs none). Neither
   was investigated for cost at this scale, because nothing tested at this scale.
2. **A long operation is indistinguishable from a dead one.** There is no progress, and no way to
   ask "is a paint still running?" — `get_slicing_status` covers slicing only.
3. **The timeout is not the operation's to know.** `ORCAMCP_TIMEOUT` is a bridge-side default of
   120 s and nothing in the tool description warns that a large mesh will exceed it.

Worth noting what did **not** go wrong: the result was correct, the response was correct once it
arrived, and a later `box` selection on the same 4.3 M-facet mesh completed inside the timeout. It
is the band path over millions of facets that is slow, not painting in general.

## T11 — nothing can select a feature, so "paint her bag" needs a ruler and three renders

Same session. The user asked for the figurine's backpack to be painted blue.

`paint_object` selects by band, box, sphere, or the whole volume. None of those name a *feature*.
Finding the bag took:

1. four renders to see which way the figure faced and where the bag sat,
2. a **12-band paint used purely as a measuring ruler**, rendered and read back to convert image
   pixels into plate millimetres,
3. a cross-check of that reading against the object's bounding box from a second view,
4. a box paint, and a render to confirm it landed.

It worked — the bag came out blue with no spill onto the skirt or hair — but it cost about ten
minutes and five renders for what the GUI does with one click.

**The tools that would collapse this to a single call**, in order of value:

- **Seed / bucket fill from a 3D point.** `TriangleSelector` already implements it
  (`seed_fill_select_triangles`, `bucket_fill_select_triangles`, `TriangleSelector.hpp:331-345`);
  it selects a connected region bounded by a sharp-edge angle, which is exactly what a bag,
  a sleeve or a shoe is. Plan 2 excluded it because it needs a hit point on the mesh, reasoning
  that a hit point is a mouse concept. That reasoning was wrong in one direction: an agent cannot
  click, but it can supply a 3D point or a facet index, and the gizmo's own entry point takes a
  facet index, not a pixel.
- **A picking tool** — "what facet, and what connected component, is at this point / along this
  ray". Without it an agent can see a feature in a render and still not address it.
- **Render metadata.** `render_plate_view` auto-frames and returns no camera matrix, so a picture
  cannot be converted back to coordinates. Returning the projection, or offering an orthographic
  mode with a stated scale, is cheap and would have removed step 2 entirely.

Also relevant, both already planned: `split_object` (Plan 3) would address the bag directly if it
is a separate shell, and `measure_object` (Plan 4) would replace the ruler with a query.

---

## T12 — `move_object` moves the object but not its plate membership, and then measures it against the wrong plate

Found 2026-09-14 during a real four-plate ABS print, with the user.

A clasp was on plate 1. It belonged on plate 4, whose region is x[307,563] y[-307,-51], so:

```
move_object {"object_id": 15, "relative": false, "x": 462, "y": -105}
```

The object landed at exactly those coordinates — squarely inside plate 4's area. But:

- `get_scene_info` still listed it under **plate 1**, and
- the response claimed `"placement_warning": "Object positioned outside printable area"` and
  `"on_bed": false`, for a position that is not outside anything.

Left alone this slices the part onto the plate it used to be on, with no error. On a multi-plate
job that is a part printed in the wrong colour, on the wrong plate, discovered after the print.

**Two distinct defects, one call.**

**1. The plate is never re-homed.** `OrcaMCPServer.cpp:3268-3287` translates the object,
calls `invalidate_bounding_box()` and `plater->update()`, and stops. It never calls
`PartPlateList::notify_instance_update(object_idx, instance_idx)`, which is what moves an
instance onto the plate whose area now contains it.

Everything else that moves geometry does call it:

- The GUI's own drag: `Selection.cpp:552` → `Selection::notify_instance_update` (`:1833`) →
  `plate_list.notify_instance_update` (`:1856`, `:1864`, `:1878`, `:1882`).
- Our own `clone_object`: `OrcaMCPServer.cpp:3966`, for each new instance, with the third
  argument `true`.

So `move_object` is the outlier, and the fix is the call the neighbouring endpoint already makes.

**2. `on_bed` is computed against the wrong plate.** `OrcaMCPServer.cpp:3296-3297` fetches
`get_partplate_list().get_curr_plate()` — *the currently selected plate* — and compares the
moved object's bounding box to that plate's box. When the caller moves an object to a plate that
is not the selected one, the check is against a region the object was never meant to be in, so a
correct move reports `outside printable area` and an incorrect one could report success. The
comparison has to be against the plate the object actually landed on, which is only knowable
after defect 1 is fixed.

**Shape of the fix (not applied).** After the translate, call
`notify_instance_update` for every instance of the object, then resolve the object's plate from
the plate list and compute `on_bed` against *that* plate's box. Report the resolved plate index
in the response — a caller that moved an object across plates needs to be told where it ended up,
and right now nothing in the response carries it.

**Related occurrences to check when fixing:** every endpoint that changes an instance transform
without re-homing. `rotate_object` and `scale_object` change the convex hull and can push an
object across a plate boundary the same way; `transform_objects` likewise. `clone_object` is
already correct and is the reference. `paint_object` deliberately does call
`notify_instance_update` (`OrcaMCPPaintTools.cpp:287`) even though paint never moves a vertex,
so the pattern is already established in this codebase.

**Workaround used during the session:** delete the object and re-load it with the destination
plate selected, since a newly loaded model lands on the current plate. That is not a fix; it
loses per-object settings and is not available for an object that was edited after loading.

## T13 — a straight-down camera makes `render_plate_view` return a degenerate view matrix

Same session, minor. Rendering a plate from directly overhead:

```
render_plate_view {"views": [{"camera_position": [128, 128, 620], "target": [128, 128, 20]}]}
```

returns `view_matrix` `[0,0,0,-0, 0,0,0,-0, 0,0,1,-620, 0,0,0,1]` — the upper-left 3x3 is all
zeros. The view direction is straight down -Z and `Camera::look_at` is called with up = +Z
(`OrcaMCPPlateUtils.cpp`, `camera.look_at(camera_position, target, Vec3d::UnitZ())`), so the
cross product of view and up is zero and the basis collapses.

The image still renders, but the camera reported alongside it is unusable: `pick_facet` fed that
matrix cannot invert it into a meaningful ray, so the see → point → fill loop silently breaks for
exactly the top-down view an agent is most likely to ask for first.

Worth either choosing a fallback up vector when the view direction is parallel to it (the usual
convention is +Y), or refusing the view with a message telling the caller to tilt the camera.
`unproject_pixel_to_ray` already rejects a non-invertible matrix, so the failure is at least
loud at the pick, not silent — but the render that produced it looks fine, which makes the
diagnosis confusing.

---

## T12 resolved — 2026-09-14

Both halves fixed. `move_object` now calls `notify_instance_update` for every instance (with
`is_new=true`, which is what keeps the spiral-mode `MessageDialog` out of `run_on_main_thread`),
resolves the plate the object actually landed on, measures `on_bed` against *that* plate, and
reports it as `plate_index`. Both halves live in `rehome_and_report_placement()` in
`OrcaMCPCommon.cpp`, beside the pure `object_within_plate()` that decides the fit
(`tests/slic3rutils/test_object_placement.cpp`).

The related occurrences named above were swept in the same batch: `rotate_object`, `scale_object`,
`transform_objects` and `mirror_object` all shared the wrong-plate measurement and none of them
re-homed; all four call the same helper now. `arrange_objects`, `auto_orient` and `flatten_object`
were deliberately left alone — they hand the work to ArrangeJob/OrientJob, which re-home through
`rebuild_plates_after_arrangement` themselves, and the tools reply before the job has run, so there
is no placement to report truthfully at reply time. `cut_object` was left alone too: it adds and
removes objects rather than transforming an instance, so its plate bookkeeping is an object-index
question and needs its own reproduction.

## T14 — `slice_all` sliced only the current plate

Found 2026-09-14, same session. With four plates and plate 4 selected, `slice_all` returned
`slicing_started` and left plates 1-3 with no slice result and no error; each had to be selected and
sliced by hand. The handler called `Plater::reslice()`, which slices the plate the background
process is pointed at.

Fixed by dispatching `EVT_GLTOOLBAR_SLICE_ALL` — the event the GUI's Slice All button posts, and the
only way to reach the per-plate chaining behind the private `Plater::priv::m_slice_all`.
`all_plates=false` keeps the single-plate behaviour. The chain walks the plate selection to the last
plate and switches the app to the G-code preview, so `slice_all` now records the plate that was
selected, `get_slicing_status` restores it when the run ends (`restored_selected_plate`), and the
previously visible view is put back. `get_slicing_status` also reports every plate's result
(`plates`, `plates_sliced`, `plates_total`), which a multi-plate run had no way to express before.

## T15 — `get_print_estimate` reported `total_toolchanges: 0` on a plate that changes tools

Found 2026-09-14, same session; also seen days earlier on an unrelated multi-filament print, so it
was never project-specific. 85 g of ABS plus 1 g of PETG support interface, interleaved, on a 4-head
toolchanger: `total_toolchanges: 0`.

Not a missing value — the wrong field. `GCodeProcessor::process_filament_change` keeps two counters
and increments them on different conditions: `total_filament_changes` only when a nozzle is loaded
with a *different* filament, `total_extruder_changes` only when the printer switches to a different
physical extruder. The G-code preview's legend shows them as "Filament change times" and "Tool
changes" (`GCodeViewer.cpp`). We reported the first under the name of the second. On a toolchanger
whose heads each keep their own filament, `total_filament_changes` is legitimately 0; on a
single-nozzle AMS/MMU machine `total_extruder_changes` is legitimately 0 instead. Both are now
reported under their own names, `filament_changes` and `extruder_changes`; `total_toolchanges` is
gone rather than redefined, since summing them double-counts under the multi-nozzle model.

## T13 resolved — 2026-09-15

`RenderThumbnail` no longer passes a bare `Vec3d::UnitZ()` to `Camera::look_at`. It passes
`OrcaMCP::stable_camera_up(camera_position, target)`, which returns +Z for every view whose
direction is not parallel to it and falls back to +Y when it is. A plan view now comes back with an
invertible view matrix, so `pick_facet` works on the image `render_plate_view` returned alongside it.

The choice was a fallback rather than a refusal because a plan view is the obvious thing to ask for
when checking a plate layout, and answering "tilt the camera" to the most natural request would have
been a worse API than quietly picking the conventional up vector. The threshold is on the length of
`up × view` rather than on exact parallelism: the basis is untrustworthy well before that cross
product reaches exactly zero, and Eigen's `normalized()` returns a zero vector unchanged instead of
failing, which is why the original bug was silent.

Covered by `tests/slic3rutils/test_plate_occupancy.cpp`, including an assertion that the old up
vector really does produce a zero-length cross product for the reported camera, and assertions that
every oblique view — the turntable previews among them — still gets +Z, so no existing render moved.

## T16 — nothing reported the prime tower, so an agent placed parts around an invisible obstacle

Found 2026-09-15, from the same four-plate session. An agent read `get_scene_info`, got exact
bounding boxes for every object, computed the free bands on a plate correctly, moved a part into one
of them and was told "Prime Tower is too close to others". It moved the part again and got the same
answer. The tower was in none of the three places it could have looked: `get_scene_info` listed only
model objects, no tool could move the tower, and `OrcaMCPPlateUtils.cpp` deliberately skipped
`vol->is_wipe_tower` when rendering, so it was absent from `render_plate_view` too.

A "find me free space" query was considered and rejected: an agent holding the complete occupancy
list can compute free space itself, and packing policy belongs in the agent. The fix is to make the
occupancy list complete.

Each plate in `get_scene_info` now carries `occupancy` — one entry per thing standing on the bed, in
plate millimetres, the same frame the object bounding boxes beside it use. It holds the model
objects (footprint grown by the brim their settings will print), the prime tower (footprint grown by
`prime_tower_brim_width`) when one is printed on that plate, and the printer's `bed_exclude_area`
rectangles. `plates[].prime_tower` carries the detail, including a `reason` token when no tower is
printed, so "no tower here" is distinguishable from "tower at X". `set_prime_tower_position` writes
the per-plate `wipe_tower_x` / `wipe_tower_y`, validating that the tower plus its brim stays inside
the range the arranger clamps to, and reporting overlaps rather than refusing them.

Two of the three occupants report an *estimate* rather than an exact number, and say so:
`brim_type: auto_brim` (the default) has no width until the object is sliced, so each object carries
both `brim.extent_mm` and `brim.extent_upper_bound_mm` and an `extent_is_exact` flag. The tower's
brim is a configured width and is exact.

The renderer now draws the tower too, in a fixed light grey. The structured data is the fix — a
picture an agent has to eyeball is a weaker answer than exact rectangles — but a plan view that
shows a clear band where a tower is standing is its own trap.

---

## T14 — slicing a plate that was already sliced segfaulted the slicer (fixed)

Found 2026-09-15 preparing a real four-plate ABS print. The user sliced one plate by hand in the
GUI while an MCP `slice_all` walk was running; OrcaSlicer died with `SIGSEGV` and the unsaved
project — four plates, the per-object filament assignments, the user's own plate-2 arrangement —
was lost. Only a `.3mf` saved earlier made it recoverable.

**Root cause: an unguarded null pointer, not a data race.** My first write-up of this finding
called it a re-entrancy race between the GUI slice and the MCP one. That was wrong, and the crash
report says so plainly.

`GCode::do_export` (`GCode.cpp:2440`) returns early — and reports success — when the G-code export
step is already done and the file is still on disk:

```cpp
if (print->is_step_done(psGCodeExport) && boost::filesystem::exists(boost::filesystem::path(path)))
    return;
```

On that path `_do_export` never runs, so `GCode::m_print` (declared `Print *m_print{nullptr}`,
assigned only at `GCode.cpp:2892`) stays null. `Print::export_gcode` then calls
`gcode.export_layer_filaments(result)` unconditionally on the next line (`Print.cpp:2931`), and
that function read:

```cpp
result->used_mixed_filaments = m_print->get_slice_used_mixed_filaments();
```

with no null check. `get_slice_used_mixed_filaments()` returns a reference to a member of `*m_print`,
so through a null `m_print` it yields the constant address `offsetof(Print, m_slice_used_mixed_filaments)`,
and `std::vector<unsigned int>::operator=` then reads through it.

**The crash report matches that chain exactly.** From
`~/Library/Logs/DiagnosticReports/OrcaSlicer-2026-09-15-004703.ips`:

- Fault: `EXC_BAD_ACCESS`, `KERN_INVALID_ADDRESS at 0x0000000000004878` — a small constant, i.e. a
  member offset from a null base, not a wild or freed pointer.
- Faulting thread 25, named `bbl_BgSlcPcs`:
  `vector<unsigned int>::operator=` ← `GCode::export_layer_filaments` ← `Print::export_gcode` ←
  `BackgroundSlicingProcess::process_fff`.
- That assignment is the **only** `vector<unsigned int>` assignment in the function.

The earlier crash the same evening (`20:58`) is an unrelated signature — `GLCanvas3D::~GLCanvas3D`
during app teardown — and is not evidence for anything here.

**Why the user's action triggered it.** A plate that has just finished slicing is left in exactly
the state the early return tests for: `psGCodeExport` done, file present. Slicing that plate again
before anything invalidates the step takes the early return, and the crash follows
deterministically. The `slice_all` walk had already exported the plate the user then sliced by
hand. Nothing about two threads was required; the same input crashes single-threaded.

**Confirmed by reproduction, not by inference.** `tests/fff_print/test_mixed_filament.cpp` now
exports one print twice to the same path. With the guard removed the test dies with
`SIGSEGV - Segmentation violation signal` at the second export; with the guard it passes.

**The fix** (`GCode.cpp`) returns early when there was no export. That is the correct answer and
not merely the safe one: without an export `m_sorted_layer_filaments` is empty, so continuing would
have cleared `result`'s filament- and nozzle-change sequences and rebuilt them empty — silently
blanking bookkeeping the caller already held. The three sibling accessors directly below
(`get_extruder_id`, `get_filament_config_index`, `get_nozzle_config_index`) already guard `m_print`
for the same reason; this one site had been missed.

**Related occurrences checked.** `export_layer_filaments` has exactly one caller
(`Print.cpp:2931`), and it is the only `GCode` method `Print::export_gcode` calls after `do_export`
— every other `m_print->` dereference in `GCode.cpp` sits inside the `_do_export` pipeline, where
`m_print` is assigned on entry. `Print::export_gcode_from_previous_file` reaches the same
"step already done" state via `set_gcode_file_ready()` but drives a `GCodeProcessor` and never
calls this function, so it is unaffected.

**Left alone deliberately.** `slice_all` still has no entry guard against being called while a
slice is in flight. With the null dereference fixed, a second slice is no longer a crash, and
adding a guard for a hazard that no longer exists would be speculative scope. Worth revisiting only
if overlapping slices produce a wrong result rather than a dead application.

**Still true regardless:** `slice_all` now runs for minutes on a multi-plate project, and a crash
during it loses everything unsaved. Checkpointing a long run is worth considering on its own merits.

---

## T15 — `get_print_estimate` silently ignored `plate_index` and answered about a different plate

Found 2026-09-15 while collecting the four plates' estimates for the ABS print. Asking for plate 1:

```
get_print_estimate {"plate_index": 1}
  -> {"plate_index": 0, "estimated_time": "7h 26m 37s", ...}
```

The tool declared **no parameters at all** (`{"properties", nlohmann::json::object()}`) and read
`plate_list.get_curr_plate()`. Schema `required` is not enforced server-side and an unknown key was
dropped in silence, so the call succeeded and returned the *selected* plate's numbers.

**Why this is worse than a plain missing feature.** The response is not obviously wrong. It carries
`status: "success"` and a full, internally consistent set of figures. The only tell is the
`plate_index` field echoing back a different number than the one asked for — which an agent
comparing four plates has no particular reason to re-read. Quoting a 7-hour plate as a 5-hour one
is the kind of error that reaches a human as a confident wrong answer.

**Fixed.** `plate_index` is now a real optional parameter: omitted it reports the selected plate, as
before; given, it reports that plate without changing the selection; out of range it returns an
error naming the valid range rather than falling back. A non-integer is rejected rather than
coerced. The reported `plate_index` is now the plate actually read, and the "no valid slice result"
error names the plate too.

**Related occurrences checked.** Six call sites read `get_curr_plate()`:

- `get_object_info` — genuinely wrong, see **T16** below.
- `get_slicing_status` — uses it only for the top-level `slice_result_valid`, and also reports every
  plate in `plates[]`, each labelled. Not misleading; left alone.
- `OrcaMCPPlateUtils::GetCurrentProject` — reads the plate only for bed dimensions, which every
  plate shares. Left alone.
- `OrcaMCPPrinterUtils:473` and `OrcaMCPPrinterTools:445` — the send/match paths, where "the plate
  you are looking at" is the intended subject and `send_to_printer` has its own `all_plates`. Left
  alone.

---

## T16 — `on_bed` was computed against the selected plate, not the object's own plate

Found in the same audit as T15, and it is the more dangerous of the two.

`get_object_info` reported:

```cpp
auto plate = plater->get_partplate_list().get_curr_plate();   // the SELECTED plate
BoundingBoxf3 bed_box = plate->get_plate_box();
bool on_bed = bbox.min.x() >= bed_box.min.x() && ... ;
```

Plates do not share a coordinate range — in this project plate 1 spans x 0..256 and plate 2 spans
x 307..563. So asking about an object that sits perfectly on plate 4 while plate 1 is selected
returned `on_bed: false`, and an object dangling off plate 1 could read `true` from plate 2's box.
The answer depended on the GUI selection, which the asking agent may never have set.

This matters because `get_server_info` explicitly instructs agents to *"use on_bed to verify
placement"*. It is the documented placement check, and it was answering about the wrong plate.

**Same family as T12.** The transform tools had this bug and it was fixed there by
`rehome_and_report_placement`, which finds the object's own plate via
`PartPlateList::find_instance` and tests against that. `get_object_info` was never updated, because
it is a query tool and could not call that helper — the helper also *mutates*, re-homing instances.

**Fixed** by splitting the helper: `report_placement` does the read-only reporting (plate_index,
on_bed, placement_warning) and `rehome_and_report_placement` now re-homes and then calls it. The
query tool gets the correct answer without the write. `get_object_info` additionally now reports
`plate_index` and `placement_warning`, matching what the transform tools already return.

**Still true, and separate:** `on_bed` only tests "within XY and not sunk below Z". It says nothing
about collisions with other objects or the prime tower, and a part floating 84 mm above the bed
still passes. That limitation is unchanged by this fix and remains on the deferred list.

---

## T17 — strict integer checks rejected calls the caller made correctly

Found 2026-09-15, immediately, by the first live call to the parameter added in **T15**:

```
get_print_estimate {"plate_index": 7}
  -> {"status": "error", "message": "plate_index must be an integer"}
```

The handler used `nlohmann::json::is_number_integer()`. That is false for `7.0`, and a client whose
JSON layer widens numbers — or one working from a cached tool schema that sends `"7"` — hands the
server exactly those spellings. The caller named the plate correctly and was refused.

**This one is mine, introduced in the T15 fix an hour earlier.** The project already has
`parse_integer_param` (`OrcaMCPCommon.cpp:42`) for precisely this: it accepts the integer, the
whole-valued float, and the decimal string, and rejects everything else. `paint_object` carries a
comment saying so, and `select_plate` uses it — which is why `select_plate` worked with the same
client that `get_print_estimate` refused. I wrote a new check instead of using the existing one.

**Related occurrences checked.** Five other sites test caller input with `is_number_integer()`; four
are the same defect and are now fixed:

| Site | Parameter | Tool |
|---|---|---|
| `OrcaMCPFilamentUtils.cpp:91` | `components[]` | `set_mixed_filament` |
| `OrcaMCPFilamentUtils.cpp:95` | `ratios[]` | `set_mixed_filament` |
| `OrcaMCPPrinterUtils.cpp:186` | `material_mappings[].tool_id` / `.slot_id` | `send_to_printer`, `match_project_to_printer` |
| `OrcaMCPPrinterTools.cpp:731` | `nozzles[].tool` | `printer_control` `set_temperature` |

The `printer_control` case is the clearest evidence it was an oversight rather than a decision: the
sibling `nozzles[].temp` in the *same entry* is checked with the permissive `is_number()`, so one
half of a pair accepted a spelling the other half refused.

`OrcaMCPPrinterUtils.cpp:104` (`filament_tool_id`) is **left alone deliberately**: it reads the
`tool_id` the slicer itself wrote into the project-filament payload a few lines above, not caller
input, so widening it would only hide a malformed internal payload.

**A second defect found while fixing the first.** `set_mixed_filament` validated `components` and
`ratios` in one loop and then re-read them with `get<unsigned int>()` / `get<int>()` in another. Two
spellings of the same rule, only one of which decides the answer — and after the fix they would have
disagreed about what a float means. The parse now happens once and the parsed values are kept.

**Lesson worth keeping.** When adding a parameter, use the codebase's existing parameter parser.
Writing a fresh type check produces a tool that is stricter than its neighbours in a way no schema
documents, and the failure lands on a caller who did nothing wrong.

---

## T18 — ABS printed with the chamber heater switched off (fixed)

Found 2026-09-15 by the user, seconds after the first real ABS print started: *"we are printing
ABS. are we heating the chanber?"* We were not. The printer reported `chamberTargetTemp: 0` while
printing ABS, and it would have run all four plates — 26 hours — that way.

**Root cause.** The shipped Creator 5 machine profile's `machine_start_gcode` contained a literal

```gcode
M191 S0 ; Chamber temp. max65C
```

That is harmful twice over. It commands the chamber to 0 and waits, and — the non-obvious half —
`GCode.cpp:3639` only emits the slicer's own `M191` when `custom_gcode_sets_temperature()` finds no
`M141`/`M191` already in the start G-code:

```cpp
if (activate_chamber_temp_control && max_chamber_temp > 0){
    int temp_out = 0;
    if(!custom_gcode_sets_temperature(machine_start_gcode, 141, 191, false, temp_out))
        file.write(m_writer.set_chamber_temperature(max_chamber_temp, true));
}
```

So a literal `M191` in the profile silently suppresses the correct command as well as overriding
the outcome.

**The filament side was never wrong.** `Flashforge ABS Basic @FF C5P` — and every ABS/ASA variant
for the Pro — ships `chamber_temperature: 60` with `activate_chamber_temp_control: 1`. The evidence
that the slicer knew all along: `M141 S0;set chamber_temperature` was present at the end of the
exported G-code the whole time, and that line is gated on *exactly the same*
`activate_chamber_temp_control && max_chamber_temp > 0` condition as the start-of-print `M191` that
never appeared. The chamber was being switched off at the end of a print it was never switched on
for.

**Fix.** `M191 S[overall_chamber_temperature]` in all six Creator 5 / 5 Pro profiles.
`overall_chamber_temperature` is set at `GCode.cpp:3541` to `max_chamber_temp`, the maximum
`chamber_temperature` across the extruders **actually used in this print** — the right quantity for
one shared chamber.

**Verified in exported G-code, not by reading the profile.** Plate 1 (gray ABS, tool 1) and plate 4
(red ABS, tool 2) both now emit:

```gcode
M140 S110                              ; bed commanded, no wait
M191 S60 ; Chamber temp. max65C        ; chamber heats in parallel, waits
...
M141 S0;set chamber_temperature        ; chamber off at end of print
```

The ordering matters and is why the line stays *inside* the start G-code rather than being deleted:
`M140` (no wait) precedes `M191` (waits), so bed and chamber come up together. Deleting the line
would have let the slicer emit its own `M191` *before* `machine_start_gcode`, heat-soaking the
chamber against a cold bed.

**A wrong fix that looks right, recorded so nobody repeats it.**
`M191 S{chamber_temperature[initial_extruder]}` resolves to **0**. The config block of that very
export shows `chamber_temperature = 0,60,60,0` and `T1` selected, so the value was present and the
index should have been 1. Indexing a per-filament vector by that variable did not evaluate as
expected. `overall_chamber_temperature` is a scalar, is what the slicer itself uses, and is already
the idiom in other shipped profiles.

**Low-temperature materials are unaffected.** A PLA or PETG print has `max_chamber_temp` 0, so the
line still emits `M191 S0` and the chamber is still explicitly turned off. Deleting the line would
have lost that safety.

### T18a — a profile fix does not reach projects that already exist

Found while verifying T18, and it wasted two full slice-and-export cycles: after fixing the six
profiles and restarting the app, the exported G-code was **byte-for-byte identical** and still said
`M191 S0`.

A 3MF carries its own complete configuration in `Metadata/project_settings.config`, and
`load_project` restores that **over** the system profile. Confirmed by unzipping the project:

```
Metadata/project_settings.config:M191 S0 ; Chamber temp. max65C
```

So a shipped-profile fix helps every *new* project and every user who starts fresh, and does
nothing at all for a project saved before the fix. Existing projects need the value re-applied
(`apply_config`) and the project re-saved.

**This generalises well beyond the chamber:** any vendor profile correction we ship is invisible to
already-saved projects. Worth remembering before concluding "the profile is fixed, therefore the
problem is gone" — and worth telling users at release time.

The byte-identical export size was the tell. Two exports of a supposedly changed configuration
producing exactly 23,255,835 bytes is not a subtle hint.

---

## T19 — CI went red without us changing anything: the external test suite is not pinned

Run `34969491326` ("Build all", commit `fe606d65a7`) failed. **18 jobs passed, 10 skipped, 1
failed** — every platform built successfully. The single failure was the Linux-only step
*"Run external slicer regression tests"*: `11 failed, 71 passed, 4 skipped, 7 xfailed`.

**Not our regression.** The failing cases are all mixed-filament and filament zero-fill:

```
mixed-filament-defined-on-cli-slices
mixed-filament-filament-colour-leaves-mixed-flush-cells-empty
mixed-filament-prime-tower-kept-with-identical-presets
mixed-filament-slot-without-filament-rejected
mixed-filament-type-mismatch-rejected
partial-load-filaments-variant-key-zero-filled
variant-key-zero-filled-on-full-filament-load
vector-override-single-value-multi-filament
```

`build_orca.yml:631` clones the suite fresh on every run, unpinned:

```bash
git clone --depth 1 https://github.com/OrcaSlicer/orca-test-repo.git "$test_repo_dir"
```

Upstream commit `7d417de` (2026-09-14, *"Mark the Mixed Filament and Zero-Fill Cases Fixed"*)
flipped exactly these cases from expected-failure to `status: fixed` — seven `+status: fixed` lines
across the same YAML files whose names match the failures one for one. They had been passing here
only because they were *tolerated as expected failures*. Upstream fixed the underlying bugs in
OrcaSlicer, marked the cases as must-pass, and our fork has not merged those fixes — so they now
fail hard.

**Two separate things follow, and they should not be conflated.**

1. **The CI design is fragile.** An unpinned `--depth 1` clone means this branch can go red
   overnight with no commit on our side, and the failure looks like ours. Pin the suite to a known
   SHA and bump it deliberately, or at minimum print the cloned SHA into the log so a red build can
   be attributed in seconds rather than by cloning the repo and reading its history.

2. **The gaps are real.** Upstream fixed mixed-filament CLI rules and per-filament variant
   zero-fill; we are behind on both. Mixed filament is not a peripheral feature here — agent-driven
   colour mixing is a headline capability of this fork. These want an upstream sync, not a
   suppression.

**Not a release blocker by itself** — nothing built or tested on our side regressed, and the
failures predate tonight's work (the run tested `fe606d65a7`, before the chamber fix landed). But
"CI is green" cannot be the release gate until the suite is pinned, because today it measures
upstream's test repo as much as it measures us.

---

## T20 — the last red CI case: X1C filament-change purges land 1 mm outside the plate check (open)

Found 2026-09-16 while making CI green for the release. Of the eight cases upstream flipped to must-pass
(T19), seven are fixed by porting #15636, #15438 and #15639 — confirmed by Linux CI run `35005568253`:
**1 failed, 81 passed**. This entry is the one that is left.

**Case:** `mixed-filament-defined-on-cli-slices` — bare STL, CLI, mixed filament of two identical PLA
presets, X1C profile. Exit 154 = `CLI_GCODE_PATH_IN_UNPRINTABLE_AREA`, `error_code = 4` (plate-area bit).

**What it was not, in the order I eliminated them — each by measurement of the produced G-code:**

1. *The tower off the back edge* (Y 276.9 on a 256 mm plate). Real, and fixed by porting the wipe-tower
   estimator chain in upstream's order — `f88fa6bfc7`, `bebd54362b`, `ca0becfbc0`, `f844a64850`,
   `4def1a09a8` — after which the clamp places a tower ending at Y 254.6, inside tolerance. Porting the
   placement commit *before* the estimator refactor segfaults (`get_extruders(bool)` → `wxGetApp()` null in
   the CLI); the crash report named it, and it is why upstream's own order matters.
2. *Y 265 travels.* The X1C macro parks at the back; those are travels and the check only counts extrusions.
   A grep for `Y265` also matched the config block's comments — 406 hits, almost none of them moves.
3. *A missing Custom role tag on sub-layer swaps.* The swap sits inside the wipe-tower block like any other,
   and `GCode.cpp` writes the Custom tag only around start/end G-code in *both* trees.

**What it is:** a replica of the processor's check, run over the G-code, finds exactly one class of
offending positions — **1188 extruding moves per tool at X 20, Y −3**, role Prime tower, once per filament
change. They are the X1C `change_filament_gcode` macro's own front-edge purge:

```gcode
G1 X20 Y50 F21000
G1 Y-3
M620.1 E F523.843 T240
T0
...
G1 E18 F523.843
```

Y −3 is one millimetre past the check's +2 mm tolerance. That macro, the filament profiles and the process
profile the test datadir is generated from are **byte-identical to upstream's** (md5). `GCode.cpp` differs
from upstream by 33 lines and `GCodeProcessor.cpp` by 15, none touching role tagging or this check. Neither
tree treats `M620…M621` as a custom region.

**So the open question is precise but unanswered:** upstream's build must exclude (or not emit) these purge
extrusions on this case, and the mechanism is not in any diff small enough to read. The next step is to build
`upstream/main` and slice the identical case, then diff what the processor records. Not started: it is an
investigation, not a port, and it was 3 a.m. before a release.

**Impact if shipped open:** CLI-only (`-102` is the CLI's post-slice check; GUI and MCP never run it), and only
for mixed-filament plates on Bambu profiles whose change macro purges off-plate. The Creator 5 path is not
involved.

**Also learned, expensively:** the external suite is meaningless on macOS. `record_exit_reson` is compiled
`#if defined(__linux__)`, so `result.json` never exists and ten of eleven local "failures" were that. Only the
Linux CI step is authoritative for this suite.
