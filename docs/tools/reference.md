# OrcaMCP Tools Reference

Reference for the MCP tools available in OrcaMCP. The authoritative tool count and the
command that regenerates it live in `CLAUDE.md`, so it is not repeated here.

The table below is every tool, grouped by the category each one declares in the registry; the
same grouping, with a one-line summary per tool, is what `get_server_info` returns. Several tools
it lists -- `get_filaments`, `set_mixed_filament`, `get_flush_volumes` among them -- have no
dedicated section below yet.

**A call that changes nothing leaves the scene alone.** Setting what is already there -- the
filament an object already prints with, the printable state it already has, a move by zero, a
rotation of 0 degrees, a scale of 1, the prime tower's own position, the brim ears or paint a
volume already carries, a slot's own colour, flush volumes or mixed-filament recipe, the name it
already has -- takes no undo step (one would drop the redo stack) and marks no plate unsliced, so a
finished slice stays finished. The object and scene tools say which it was with `changed`
(`set_object_printable`, `set_object_filament`, `rename_object`, `set_brim_ears`, `move_object`,
`rotate_object`, `scale_object`, `mirror_object`, `transform_objects` per entry,
`set_prime_tower_position`, `set_mixed_filament`), the painting tools with `annotation_changed`, the
resets with their counts.

**A call is held to the tool's schema.** An argument a tool does not take, a required one left out,
or arguments that are not an object are refused before the tool runs, with JSON-RPC error -32602
naming the problem and what the tool takes -- never ignored, so a misspelled argument cannot turn
into a call that reports success and changes nothing. See [Error Handling](#error-handling).

## Quick Reference Table

| Category | Tools |
|----------|-------|
| **Scene** | `get_scene_info`, `new_project`, `load_project`, `save_project`, `export_3mf` |
| **Models** | `load_model`, `auto_orient`, `arrange_objects`, `get_object_info`, `get_mesh_health`, `repair_mesh`, `get_object_components`, `rename_object`, `set_object_printable` |
| **Transforms** | `move_object`, `rotate_object`, `scale_object`, `mirror_object`, `flatten_object`, `clone_object`, `cut_object`, `delete_object`, `transform_objects` |
| **Plates** | `add_plate`, `select_plate`, `delete_plate`, `set_prime_tower_position` |
| **Config** | `get_presets`, `get_edited_presets`, `get_config_values`, `select_preset`, `apply_config`, `clone_preset`, `save_preset`, `delete_preset`, `reset_preset`, `get_valid_config_keys` |
| **Per-Object** | `get_object_config`, `set_object_config`, `reset_object_config` |
| **Layer Ranges** | `get_object_layer_ranges`, `set_object_layer_range`, `delete_object_layer_range` |
| **Filaments & colour** | `get_filaments`, `set_object_filament`, `set_mixed_filament`, `delete_mixed_filament`, `set_filament_color`, `get_flush_volumes`, `set_flush_volumes`, `auto_calc_flush_volumes`, `get_toolchanger_config`, `suggest_color_mix`, `get_color_palette` |
| **Painting** | `paint_object`, `remap_paint`, `get_object_paint`, `clear_object_paint`, `set_brim_ears`, `pick_facet` |
| **Slicing** | `slice_all`, `wait_for_slice` (bridge-only), `get_slicing_status`, `export_gcode`, `get_print_estimate` |
| **Visualization** | `render_plate_view`, `get_preview_base64`, `set_gcode_view_type` |
| **Printers** | `get_printers`, `select_printer`, `add_physical_printer`, `discover_printers`, `send_to_printer`, `get_printer_status`, `printer_control`, `list_printer_files`, `print_printer_file`, `match_project_to_printer` |
| **Adaptive** | `apply_adaptive_layer_height`, `clear_adaptive_layer_height` |
| **History** | `undo`, `redo` |
| **Info** | `get_server_info`, `quit_app`, `start_orca` (bridge-only) |

---

## Information Tools

### get_server_info
The tool catalogue and the server's documentation. The catalogue is generated from the tool
registry on every call, so it names every tool the build has, bridge-only ones included.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `section` | string | No | `concepts`, `suggested_flows`, `tool_examples`, `warnings_and_best_practices`, `settings`, or `all`. Omit for the default response. |

**Returns:** Without `section` (about 5.6 KB): `server` (name, version from `version.inc`),
`quick_start`, `tools` (every tool's one-line summary, grouped by category), `bridge_only` (tools
the bridge answers itself), and `sections` (each section's name and size in bytes). With a
section name, just that section; with `all`, everything (about 25 KB).

**Examples:**
```json
{"name": "get_server_info", "arguments": {}}
{"name": "get_server_info", "arguments": {"section": "suggested_flows"}}
```

---

### set_filament_color
Set the colour of a filament slot **as the plate shows it** — the sidebar swatch, the 3D view and
the flush calculation all read the project's per-slot colour. `apply_config` with `filament_colour`
edits the filament *preset* instead, which is why "paint it white" could not be completed before
this tool existed.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `slot` | integer | Yes | Filament slot, 1-based |
| `color` | string | Yes | `#RRGGBB` or `#RRGGBBAA` |

**Example:** `{"name": "set_filament_color", "arguments": {"slot": 4, "color": "#FFFFFF"}}`

**Returns:** `{"status": "success", "slot": 4, "color": "#FFFFFF", "previous_color": "#BEBEBE"}`,
plus `"flattened": true` when the slot held a gradient (multi-colour) that this flat colour replaced.
Pair it with `select_preset` (`type: filament`, `slot`) to put a material on the spool, then
`paint_object` or `set_object_filament` to use it.

The colour is saved for the selected printer, the way the sidebar's colour picker saves it. That
matters because a printer switch (with *Remember printer configuration* on, the default) replaces
every slot's colour with the ones last saved for the printer switched to; `select_preset
{type: printer}` reports which it applied. Switching away and back therefore brings back the colour
set here.

---

### quit_app
Quit OrcaMCP cleanly with no dialog. An agent has to be able to close the app; a signal skips the
shutdown path and AppleScript raises the "save changes?" prompt, which MCP dialog suppression does
not cover because the close did not come through MCP.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `discard_changes` | boolean | No | Default `true`: unsaved project changes are discarded, and a dialog the app is showing is closed unanswered. `false` refuses while the project is dirty (call `save_project` first) or a dialog is open, and names the dialog. |

A dialog the app is showing (the startup "restore unsaved items?" prompt after a crash, a dialog the
user opened) is closed first, innermost first, each with its own "no": its Cancel button, else its No
button, else what its close box does. Each one is named in `info_messages`. The restore prompt keeps
its backup, so the next launch asks again. A system file chooser or alert cannot be closed by the app:
`quit_app` then refuses until the user closes it.

If a dialog is still open 10 s later, the quit gives up: the app keeps running with its unsaved
changes, and every tool's `active_warnings` carries an `error` of type `QuitFailed` saying what is
open, until the next `quit_app`. An agent that got `"quitting"` and still finds the app answering
reads it there.

**Returns:** `{"status": "quitting"}`, with `info_messages` naming any dialog it closes; the app exits within a few seconds, also while other calls are
in flight (an agent's parallel calls, a poller). From the moment the app starts closing, every tool call
is answered with JSON-RPC error -32002, "OrcaMCP is quitting, so this call was not run", and its work
is not run; a call already running on the GUI thread is let finish, and one waiting on the network
(`discover_printers`, a printer request) gives up within about a second with its tool error ("Request
cancelled", "Printer discovery was cancelled"). `quit_app` is itself served after the call ahead of it:
calls are answered one at a time. A running slice is cancelled first: usually within a second, but
organic tree supports check for a cancel only between phases, so on the -O0 dev build the quit can
wait up to about a minute for one.

---

### get_scene_info
Get current project state including plates, objects, and positions.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `with_model_object_features` | boolean | No | Adds `features` to every object, `unplaced_objects`' too: the mesh-health numbers behind `mesh_warning` (facets, shells, open_edges, manifold, repaired, errors_repaired, repaired_errors, volume_mm3), as `get_mesh_health`'s `summary`. No overhang analysis: slice to see where support is needed. |
| `include_preview` | boolean | No | Include turntable preview path |

**Example:**
```json
{"name": "get_scene_info", "arguments": {"with_model_object_features": false}}
```

**Returns** (captured from a real response on 2026-09-28: one object, a column with an arm, a hole
and a stray shell; the object list's warning icon shows for it):

<!-- get_scene_info example: test_mcp_scene_description.cpp holds its keys to what the response writes -->
```json
{
  "hash_code": "03fcaebe3e7327f107f130f92019a838",
  "sequential_print_enabled": false,
  "bed": {"origin": "corner", "min_x": -0.0001, "min_y": -0.0001, "max_x": 256.0001, "max_y": 256.0001, "max_z": 256.0001},
  "plates": [{
    "name": "",
    "plate_index": 0,
    "index": 0,
    "is_current": true,
    "bounding_box": {"min": {"x": -0.0001, "y": -0.0001, "z": -0.0001}, "max": {"x": 256.0001, "y": 256.0001, "z": 256.0001}},
    "model_objects": [{
      "object_id": 0,
      "object_index": 0,
      "internal_id": "65",
      "name": "gallows.stl",
      "instance_count": 1,
      "instances_on_plate": [0],
      "volume_count": 1,
      "position": {"x": 128.0, "y": 128.0, "z": 12.5},
      "rotation_degrees": {"x": 0.0, "y": -0.0, "z": 0.0},
      "scale": {"x": 1.0, "y": 1.0, "z": 1.0},
      "bounding_box": {"min": {"x": 104.0, "y": 123.0, "z": 0.0}, "max": {"x": 152.0, "y": 133.0, "z": 25.0},
                       "size_x": 48.0, "size_y": 10.0, "size_z": 25.0},
      "brim": {"type": "no_brim", "extent_mm": 0.0, "extent_upper_bound_mm": 0.0, "extent_is_exact": true},
      "printed_footprint": {"min_x": 104.0, "min_y": 123.0, "max_x": 152.0, "max_y": 133.0, "size_x": 48.0, "size_y": 10.0},
      "printed_footprint_includes_brim": false,
      "vlh_enabled": false,
      "vlh_profile_points": 0,
      "extruder_id": 1,
      "filaments_used": [1],
      "filament_override_count": 0,
      "mesh_warning": true,
      "mesh_warning_reason": "Error: 3 non-manifold edges."
    }],
    "prime_tower": {
      "printed": false,
      "reason": "single_filament_plate",
      "reason_detail": "This plate uses a single filament, so no tower is generated even though the project is multi-filament.",
      "frame": "plate_mm",
      "stored_position": {"x": 165.0, "y": 225.09014892578125, "frame": "plate_local_mm"}
    },
    "excluded_areas": [],
    "occupancy_frame": "plate_mm",
    "occupancy": [
      {"kind": "object", "name": "gallows.stl", "object_index": 0, "instances_on_plate": [0],
       "footprint": {"min_x": 104.0, "min_y": 123.0, "max_x": 152.0, "max_y": 133.0, "size_x": 48.0, "size_y": 10.0},
       "includes_brim": false, "footprint_is_exact": true, "height_mm": 25.0}
    ]
  }],
  "unplaced_objects": [],
  "open_dialogs": [],
  "system_dialog_open": false,
  "untracked_modal_loop": false,
  "active_warnings": {"count": 1, "warnings": [{
    "level": "warning", "type": "MeshErrors", "object_id": 0, "object_name": "gallows.stl",
    "message": "Error: 3 non-manifold edges. repair_mesh repairs it with the app's own repair, on every platform: it splits the mesh into its parts, drops parts with no volume and closes each part's holes; painting is cleared unless keep_painting keeps it. Slicing also closes each layer's outline across gaps of up to 2 mm, so a hole that small usually prints closed; a wider one can leave that outline out of a layer, so check the sliced preview there. Details: get_mesh_health {object_id: 0}; repair: repair_mesh {object_id: 0}."}]},
  "next_steps": [
    {"tool": "get_mesh_health", "arguments": {"object_id": 0},
     "why": "object 0 (\"gallows.stl\") shows the mesh warning icon: Error: 3 non-manifold edges."},
    {"tool": "get_object_components", "arguments": {"object_id": 0},
     "why": "part \"gallows.stl\" of object 0 (\"gallows.stl\") has 2 shells: a loose part or stray fragment may be one of them"}
  ]
}
```

With a prime tower printed, `prime_tower` also carries `position` (the front-left corner of the tower
body), `position_is`, `size`, `brim_width_mm`, `body`, `footprint`, `footprint_includes_brim` and a
`note`, and `occupancy` a `prime_tower` entry. `with_model_object_features` adds `features` to every
object; `include_preview` adds `preview_path` and `preview_hint`.

#### Object and plate numbers

Every object description -- `model_objects`, `unplaced_objects`, and `load_model`'s `loaded_objects`
-- carries `object_id`, the 0-based index every tool's `object_id` parameter takes. `object_index` is
the same number, kept for older readers. `internal_id` is the app's own number for the object: stable
while the app runs, never saved in the project, and taken by no tool (it was called `id`, and an agent
reading `"id": "71"` beside `"object_index": 0` passed 71). An object's `object_id` shifts when an
object before it is deleted.

Every plate carries `plate_index` (what `select_plate`, `render_plate_view` and the other plate tools
take; `index`, the same number, is kept) and `is_current`: true for the plate per-plate tools act on
(`export_gcode`, `get_print_estimate` without `plate_index`, `send_to_printer`), whose printable area
`bed` gives.

#### Which filament an object prints with

Each `model_objects` entry carries three filament fields. `extruder_id` is the object's own
setting. A volume's own slot beats it (`ModelVolume::extruder_id`), so `filaments_used` — every
slot the object's parts, modifiers, painted facets and layer ranges print with — is the field to
read, and `filament_override_count` says how many parts and modifiers carry their own slot. The two
agree only when that count is 0. `filaments_used` is the per-object half of the rule the plate
applies for `prime_tower`; an object whose modifiers are pinned to another slot keeps the plate
multi-filament however `extruder_id` reads.

#### Unplaced objects

`unplaced_objects` lists every object with an instance no plate holds: `object_id`, `object_index`, `internal_id`, `name`,
`instance_count`, `unplaced_instances` (the instances on no plate), `position` (their box's centre)
and `reason`. Deleting a plate leaves what stood on it there, and a move can carry an instance off
every plate. An object whose other instances are on plates is also listed under those plates, by the
instances there. Before v2.5.0.6 only an object whose instance 0 was on no plate was listed, so a copy
on no plate went unreported.

#### Mesh warnings

Every object, in `model_objects` and in `unplaced_objects`, carries `mesh_warning`: `true` when the
object list shows its warning icon, because its mesh has open edges or a 3MF recorded repairs to it.
Then `mesh_warning_reason` is the list's one-line reason (`"Error: 25 non-manifold edges."`), and
this response's `active_warnings` carries a `MeshErrors` entry for the object saying what an agent can
do about it. `get_mesh_health` has the numbers and each volume's share; `with_model_object_features`
adds the object's numbers here.

#### Occupancy: everything standing on the plate

`model_objects` lists the models. It is **not** the list of what occupies the bed. `occupancy` is:
it carries one entry per occupant, in **plate millimetres** — the same frame `bounding_box` and
`get_object_info` use — with the same four numbers for each. Use it, not `model_objects`, to work
out where there is free space.

| `kind` | What it is |
|--------|------------|
| `object` | A model object, its footprint grown by the brim its settings will print |
| `prime_tower` | The prime tower, its footprint grown by `prime_tower_brim_width`. Present only when a tower is actually printed on that plate |
| `excluded_area` | Bed the printer will not print on (`bed_exclude_area` in the printer config — the filament-cutting corner on an X1, for instance). Empty on most printers |

Every `footprint` in `occupancy` **includes the brim**, because the brim is printed plastic and a
part placed flush against a bounding box collides with it. `includes_brim` says whether the entry
actually has one, and `footprint_is_exact` is `false` when the slicer decides the real brim width
itself at slice time.

**Brim, and why it is sometimes an estimate.** `brim_type` `auto_brim` (the default), `brim_ears`
and `painted` do not have a width until the object is sliced — `auto_brim` recomputes it per volume
group, capped at 18 mm. Each object therefore reports both `brim.extent_mm` (the best estimate
before slicing, from the configured `brim_width`) and `brim.extent_upper_bound_mm` (the worst case).
`printed_footprint` uses `extent_mm`; a caller that would rather over-reserve bed than collide can
expand by `extent_upper_bound_mm` itself. `outer_only` and `outer_and_inner` are exact;
`no_brim` and `inner_only` reach 0 mm past the object. Per-object overrides win over the print
preset's values.

**The prime tower.** `position` is the **front-left corner of the tower body** (not its centre —
model objects report their bounding-box centre, the tower reports a corner, because that is what
the `wipe_tower_x` / `wipe_tower_y` config keys are). `body` is the tower alone; `footprint` is
`body` plus the brim on all four sides, and is what a part must stay clear of. `stored_position` is
the same corner in plate-**local** millimetres, which is what those two config keys hold.

When no tower is printed, `printed` is `false` and no geometry is reported — only `reason`, one of:

| `reason` | Meaning |
|----------|---------|
| `printed` | There is a tower; the geometry is reported |
| `not_fff` | Not an FFF printer |
| `disabled` | `enable_prime_tower` is off in the print preset |
| `single_filament_project` | The project has one filament, so nothing needs priming |
| `gcode_only_mode` | A G-code-only project |
| `sequential_multi_object` | This plate prints by object and has more than one |
| `single_filament_plate` | This plate uses one filament, even though the project is multi-filament |
| `empty_plate` | Nothing on this plate |

A smooth timelapse or wrapping detection forces a tower even on a single-filament plate; the
reasons above account for that.

**Open dialogs.** `open_dialogs` lists the titles of the dialogs the app is showing, innermost first
(`[]` when none), `system_dialog_open` is `true` while a system file chooser or alert is open, and
`untracked_modal_loop` is `true` while a modal window no dialog accounts for runs (`quit_app` refuses
for both). A
tool opens one only where its entry says so (`send_to_printer`'s Bambu dialog, or `direct: false`),
but the user can open one, and after a crash the app starts with its "restore unsaved items?" prompt
(`"OrcaMCP - Restore"`). Every modal dialog counts, the plain ones too (the flushing-volumes dialog,
the filament map). Such a dialog waits for the user; tool calls still run while it is open (but see
`new_project`), and every tool's `active_warnings` carries an `OpenDialog` warning naming it.
`quit_app` closes the app's own dialogs unanswered.

---

### get_slicing_status
Check slicing progress, for the selected plate and for every plate. To wait for a slice to finish,
call `wait_for_slice` rather than polling this.

**Parameters:** None

**Returns:**
```json
{
  "is_slicing": false,
  "state": "done",
  "status": "idle",
  "plate_index": 0,
  "slice_result_valid": true,
  "plates": [
    {"plate_index": 0, "index": 0, "slice_result_valid": true, "percent": 100, "gcode_check": {"ok": true}},
    {"plate_index": 1, "index": 1, "slice_result_valid": true, "percent": 100,
     "gcode_check": {"ok": false, "problems": ["above_printable_height"],
                     "message": "a toolpath is above the printer's printable height (the highest layer prints at 20.5 mm; the printable height is 20 mm)",
                     "highest_layer_z_mm": 20.5, "printable_height_mm": 20,
                     "hint": "The app's check before slicing measures each object without its raft, so a raft lifts an object that fits by the raft's thickness; fewer raft_layers, or a lower object, keeps its top layer within the printable height."}}
  ],
  "plates_sliced": 2,
  "plates_total": 2,
  "stage": null,
  "busy": false,
  "busy_reason": null,
  "ui_job": null,
  "slice_run": {"ended_early": false, "scope": "all_plates", "plates": [0, 1], "skipped": [], "outcome": "done"},
  "active_warnings": {"count": 0, "warnings": []}
}
```

| Field | Meaning |
|-------|---------|
| `state` | `slicing` (in progress); `done` when the last `slice_all` run is done (`slice_run.outcome`) and the selected plate is sliced or empty -- before any `slice_all`, or once none of its plates exists (a new project), when the selected plate is sliced; `idle` otherwise (never sliced, an edit invalidated a result, or the run is not done) |
| `is_slicing` | Background process running right now. During a `slice_all` run over every plate it stays true from the first plate to the last |
| `status` | Legacy field, `slicing` or `idle` only - use `state` |
| `slice_result_valid` | The current plate's own slice-result flag, the same one the GUI's Print/Export buttons use |
| `plates` | Every plate's slice-result flag and `percent`, so a multi-plate run can be followed plate by plate (and a plate that failed can be identified). `percent` is 0-100 while a plate slices and 100 once it has a result; `null` for a plate with no result that nothing is slicing. The GUI drops progress updates while another job (an arrange, an orient) runs, so a percent can stand still then. An MCP change to an object's settings, layer ranges, adaptive layers, name, filament or printable state, or to the project's colours, flush volumes, mixed filaments or a plate's prime tower, marks the plates it touches not sliced at once, the unselected ones too (the app alone would find out only when the plate is next selected). Slicing again takes back the result of a plate the change did not affect without slicing it |
| `plates[].gcode_check` | The check the plate's slice ran on its own G-code, `null` without a slice result: `{"ok": true}`, or `ok: false` with `problems` and a `message` in words. The problems: `outside_bed` (a toolpath outside the bed's printable area), `above_printable_height` (above the printer's printable height: the highest layer an extrusion prints at is above it, and both are given in mm as `highest_layer_z_mm` / `printable_height_mm`, with a `hint`. The check before slicing already refuses an object whose own top layer is above the printable height, so what gets here is almost always lifted by a raft, which that check leaves out; a `printable_height` of 0 sets no limit), `above_extruder_height` (above the printable height of the extruder the filament is on), `outside_extruder_area` (outside the area that extruder can reach), `in_wrapping_area`, `over_printed_mass` (heavier than the printer's maximum printed mass), `toolpath_outside` (outside the printable volume), `filament_bed_conflict` (a filament the plate type has no first-layer bed temperature for), and `check_bit_<n>` for a check this list does not name yet. The GUI keeps a failed plate's Print, Send and Export buttons off, and `export_gcode` and `send_to_printer` refuse it. Until v2.5.0.6 the height checks never tripped on a non-Bambu printer |
| `busy` / `busy_reason` | Whether the slicing pipeline is busy, and with what: `slicing` (a slice or Slice All run), `exporting` (the background process writes G-code), `uploading` (it sends G-code to a printer) or `stopping` (the last slice finished or was cancelled, and its completion is not taken in yet). `slice_all` starts nothing while it is busy, and `wait_for_slice` waits until it is not. `is_slicing` is only the first of these |
| `ui_job` | A job holding the app apart from slicing: `arranging` or `orienting` (one a tool started, still running past its wait), `other` (one the GUI started), or `null`. Kept out of `busy`: `slice_all` answers `busy_job` while one runs, and `wait_for_slice` does not wait for it |
| `stage` | While slicing: the app's progress text for the running slice ("Generating walls", "Generating support", ...), in the app's language. `null` when nothing is slicing |
| `plates_sliced` / `plates_total` | How many of the plates have a valid result |
| `restored_selected_plate` | Present only on the poll that ends a `slice_all` run over every plate: the plate that was selected when `slice_all` was called has been selected again |
| `slice_run` | How the last `slice_all` run stands. `scope` (`all_plates`, `current_plate`, or `null` before the first `slice_all`) and `plates`: the current indexes of the plates it asked for that still exist. `skipped`: those of them with no printable object, which have nothing to slice (Slice All skips them). `outcome`: `running`, `done` (every one of them with something on it has a result), `ended_early`, `incomplete` (the run is over and some of the plates still there have no result -- a plate-list change during the run cancels Slice All, and `message` then says so -- or none of its plates had anything to slice, `message` "nothing to slice ..."), or `null` with no run or once none of its plates is left (after `new_project` or `load_project`). A plate deleted after the run does not make it incomplete; `message` says which plates and why when it is `ended_early` or `incomplete`. `ended_early: true`, with `stopped_at_plate` and `reason`, when the last Slice All run stopped before its last plate because another job (an arrange, an orient) was running; that plate and the ones after it are not sliced. `active_warnings` carries a `SliceAllEndedEarly` warning then too. Cleared when the next Slice All starts; call `slice_all` again. `wait_for_slice` ends its wait on this |

**Usage:** After `slice_all`, call `wait_for_slice` (it polls this for you), or poll every 2-3
seconds until `slice_run.outcome` is no longer `running`, then call `get_print_estimate`. `is_slicing: false` on its own does **not** mean the slice finished - it is
also false before slicing ever started. `state` follows the last run, so an empty plate selected at
the end of it reads `done`; `plates_sliced` / `plates` say which plates have a result.

---

## Project Tools

### new_project
Create a new empty project. Unsaved project changes are discarded without asking. A running slice is cancelled first (see `quit_app` for how long that can
take). Refused while the startup "restore unsaved items?" prompt waits (`get_scene_info`'s
`open_dialogs`): the new project would take over the app's record of the backup that prompt offers,
and a later launch would not offer it again. Answer the prompt, or `quit_app`, which keeps it. An
error dialog the app raised on the way is listed in `error_messages`; the call is `success` whenever
the new project was started.

**Parameters:** None

**Example:**
```json
{"name": "new_project", "arguments": {}}
```

---

### load_project
Open a project file (.3mf) as the project: its objects, plates **and presets** replace the scene and
the current settings, and unsaved project changes are discarded without asking. To add a model's
geometry without changing any setting, use `load_model`. A running slice is cancelled first.
Refused while the startup "restore unsaved items?" prompt waits, as `new_project` is.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | Yes | Path to .3mf file |
| `include_preview` | boolean | No | Return a turntable preview path |

**Example:**
```json
{"name": "load_project", "arguments": {"file_path": "/path/to/project.3mf"}}
```

A project that fails to load (no objects) is `status: "error"`, with the app's error dialogs' words
as `message` and `error_messages` (captured; no dialog is left open). A project that opened is
`success`, with `project_renamed_to`, and any error dialog it raised on the way in `error_messages`.

Every instance of every object is on the plate it stands on once the project has opened. Before
v2.5.0.6 only each object's first instance was: an object with copies on two plates opened with the
second plate empty, in the GUI too, so that plate sliced nothing.

**Returns:**
```json
{"status": "success",
 "file": "/path/to/project.3mf",
 "project_renamed_to": "/path/to/project.3mf",
 "info_messages": ["The project is now named /path/to/project.3mf: save_project and the GUI's Save both write there from now on."],
 "active_warnings": {"count": 0, "warnings": []}}
```

The project takes the loaded file's name, so `save_project` with no `output_path` writes back to
it - `project_renamed_to` says which file that is.

---

### save_project
Save the current project. Without `output_path` it **overwrites the file the project is named after**
(the last `load_project`, `export_3mf` or `save_project` path); with it, it saves there and names the
project after it, as `export_3mf` does.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `output_path` | string | No | Path of the `.3mf` to save to. **Required while the project has no file name.** Also acts as Save As. |

The old `save_as` flag is gone: it was ignored, so `save_as: true` without `output_path` saved in
place over the current file. It is now refused like any argument the tool does not take (see
[Error Handling](#error-handling)); pass `output_path` to save under a new name.

A project that already has a file name (it was loaded with `load_project`, or named by a previous
`export_3mf` / `save_project`) is saved in place when `output_path` is omitted. A project with no
name cannot be saved without one: naming it would need a file dialog, and MCP never opens modal
dialogs, so the call returns

```json
{"status": "error",
 "message": "This project has no file name yet, and MCP cannot open the file dialog that would ask for one. Call save_project again with output_path set to the .3mf path to save to."}
```

**Returns:**
```json
{"status": "success", "filename": "/Users/me/prints/bracket.3mf",
 "project_renamed_to": "/Users/me/prints/bracket.3mf",
 "info_messages": ["The project is now named /Users/me/prints/bracket.3mf: save_project and the GUI's Save both write there from now on."]}
```

`project_renamed_to` appears only when this call changed the project's name.

---

### export_3mf
Write the project to a `.3mf` file. **This is this API's Save**: it also names the project, exactly
as the GUI's Save As does.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `output_path` | string | Yes | Path of the `.3mf` to write. No dialog is ever opened, so it cannot be omitted. |

**Returns:**
```json
{"status": "success",
 "output_path": "/Users/me/prints/bracket.3mf",
 "project_renamed_to": "/Users/me/prints/bracket.3mf",
 "info_messages": ["The project is now named /Users/me/prints/bracket.3mf: export_3mf is this API's Save, so save_project and the GUI's Save both write there from now on."]}
```

`project_renamed_to` is present whenever the call changed the project's name. The rename is not
cosmetic: it retitles the window, adds the file to Recent Projects, and makes both `save_project`
and a Cmd-S in the GUI overwrite that file.

---

## Model Tools

### load_model
Import a 3D model file, adding its objects to the scene and **keeping the current presets**: this is
how to re-import a model without losing settings. To open a 3MF as the project, with its presets, use
`load_project`. `next_steps` names `get_mesh_health` for a loaded object with the mesh warning icon and
`get_object_components` for one with a part made of several shells (see [Next Steps](#next-steps)).

A 3MF is always imported as geometry only, whatever the app's "load behaviour" setting says and
whether the scene is empty or not: its printer, filament and process presets are not applied, your
unsaved preset edits are kept, and the project keeps its name. Use `load_project` to open a 3MF as
a project.

A G-code file (`.gcode`, `.g`) or a sliced 3MF bundle (`.gcode.3mf`) does not add objects: it
replaces the scene with a preview of its G-code, and the project takes the file's name. So
`load_model` loads one only onto an empty scene, and refuses it with an error (touching nothing)
when the plate has objects: `save_project`, then `new_project`, then load it. While the scene is a
G-code preview, model files are refused too; `new_project` returns to an editable scene.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | Yes | Path to STL, OBJ, 3MF, STEP, AMF, SVG or DRC file |
| `include_preview` | boolean | No | Return turntable preview path |
| `multipart` | string | No | `merge` (default) or `separate`. When the file holds several objects at different heights, the slicer asks whether they are one object's parts: `merge` joins them into one object named after the file (as the GUI suggests), `separate` keeps each as its own object. Nothing splits a merged object again, so choose here. |

**Example:**
```json
{"name": "load_model", "arguments": {"file_path": "/path/to/model.3mf", "multipart": "separate"}}
```

**Returns:**

| Field | Description |
|-------|-------------|
| `status` | `success`, or `error` when the file failed to load, **loaded but added no objects** (for example a ZIP, whose file picker cannot open under MCP), did not produce a G-code preview (unreadable G-code), or was refused (see above) |
| `file` | The path loaded |
| `loaded_objects` | One entry per object the load added, in the same shape as `get_scene_info`'s `model_objects`: `object_id` (the index every tool takes), `object_index` (the same number, kept for older readers), `internal_id` (the app's own number, which no tool takes), `name`, `instance_count`, `volume_count`, `position`, `rotation_degrees`, `scale` `{x,y,z}` of its first instance, `bounding_box` `{size_x, size_y, size_z, min, max}` and `mesh_warning` (with `mesh_warning_reason` when the object list shows its warning icon). A merged multi-part file shows as one object with several volumes; a model scaled to fit the bed shows its scale. Empty for a G-code preview. |
| `filaments_added` | Filament slots the import added, because the model uses more filaments than the scene had (0 when none) |
| `project_renamed_to` | Present only if the project's name changed: never for a model file, and always for a G-code preview (named after the file, so a later `save_project {}` writes there) |
| `info_messages` | What happened, then what the slicer would have shown. A 3MF import says whether the file carried presets that were not applied. A prompt that offered a choice ends with the answer given, e.g. `"Object too large: ... scale it down to fit the print bed automatically? (auto-answered Yes)"`; the multi-part question also names the other `multipart` value |
| `error_messages` | The error dialogs the load raised, captured instead of shown: an STL the reader cannot parse ("Loading of a model file failed."), G-code with no valid moves, a 3MF with an invalid configuration. No dialog is left open. A load that added objects is `success` whatever it raised -- the objects are in the scene, and loading again would add them twice -- and lists these as warnings; only a load that added nothing is `error`, with their words as `message` |
| `active_warnings` | As for every scene tool, plus a `MeshErrors` warning for each object it added that the object list flags with its warning icon |

A 20 mm cube exported 1000 times too large, on a 256 mm bed:

```json
{
  "status": "success",
  "file": "/tmp/cube_20m.stl",
  "loaded_objects": [
    {"object_id": 0, "object_index": 0, "internal_id": "65", "name": "cube_20m.stl", "instance_count": 1, "volume_count": 1,
     "position": {"x": 128.0, "y": 128.0, "z": 127.0},
     "rotation_degrees": {"x": 0.0, "y": 0.0, "z": 0.0},
     "scale": {"x": 0.0127, "y": 0.0127, "z": 0.0127},
     "bounding_box": {"size_x": 254.0, "size_y": 254.0, "size_z": 254.0,
                      "min": {"x": 1.0, "y": 1.0, "z": 0.0}, "max": {"x": 255.0, "y": 255.0, "z": 254.0}},
     "mesh_warning": false}
  ],
  "filaments_added": 0,
  "info_messages": ["Object too large: Your object appears to be too large, do you want to scale it down to fit the print bed automatically? (auto-answered Yes)"],
  "active_warnings": {"count": 0, "warnings": []}
}
```

---

### auto_orient
Orient every object on the current plate for printing, as the plate's **Auto Rotate** does.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `include_preview` | boolean | No | Include a preview, drawn once the orient has been applied |

Answered once the orient has been applied (see [Waiting for the job](#waiting-for-the-job)):

```json
{"status": "success",
 "objects": [{"object_id": 1, "name": "cube20.stl", "position": {"x": 188.0, "y": 128.0, "z": 10.0},
              "rotation_degrees": {"x": 90.0, "y": 0.0, "z": 0.0}, "scale": {"x": 1.0, "y": 1.0, "z": 1.0},
              "changed": true, "plate_index": 0, "plate_indices": [0], "on_bed": true,
              "instance_placement": [{"instance_id": 0, "plate_index": 0, "on_bed": true,
                                      "position": {"x": 188.0, "y": 128.0, "z": 10.0}}]}],
 "active_warnings": {"count": 0, "warnings": []}}
```

`objects` lists every object that was on the current plate, `changed` saying whether the orient
turned it. Before v2.5.0.6 this answered `orient_started` as soon as the orient was queued, and a
`get_object_info` right after still showed the old rotation.

---

### arrange_objects
Arrange every object on the current plate, as the plate's **Arrange** does.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `include_preview` | boolean | No | Include a preview, drawn once the arrange has been applied |

Answered once the arrange has been applied, with `objects` as `auto_orient` gives them: each one's
`position`, `plate_index` and `changed` where the arrange put it (see
[Waiting for the job](#waiting-for-the-job)).

### Waiting for the job

`arrange_objects`, `auto_orient`, `flatten_object` and `clone_object` start a job the app runs in the
background (an arrange or an orient) and wait for it before they answer, so their answer, and its
preview, show the placement the job left, and no other call can land before it has been applied.

| `status` | When |
|----------|------|
| `success` | The job was applied: the placement it left |
| `arrange_started` / `orient_started`, `finished: false`, `ui_job` | Still running when the wait ran out: "Still arranging after 105.0 s: get_slicing_status's ui_job stays \"arranging\" until it has finished; then get_scene_info reads the result." The wait is the bridge's cap: 15 s below `ORCAMCP_TIMEOUT` (105 s at the default 120 s), a quarter below it under 60 s, none under 2 s. The bridge sends it with every tool call as `params._meta["orcamcp/wait_cap_s"]`; without it the app waits up to 105 s |
| `arrange_started` / `orient_started`, `finished: false` | The app began quitting during the wait |
| `cancelled` | The app cancelled the job before applying it (another job, a deleted object or a new project cancels it), or another job replaced it before it started: nothing moved, and its undo step restores nothing |
| `error` | The job failed; `message` says why |

While another job (an arrange or an orient) holds the app, each of these refuses to start: "another
job (an arrange or an orient) is running: poll get_slicing_status until ui_job is null, then call
auto_orient again".

---

### get_mesh_health
Check an object's mesh for problems: holes and open edges (non-manifold), repaired facets, and loose
parts or stray shells, for one object and each of its volumes -- the errors behind the object list's
warning icon.

The icon state and its tooltip come from the object list's own code (`mesh_errors_info` in
`GUI_ObjectList.cpp`), so this reports what the GUI shows: the icon appears when a mesh has open
edges or recorded repairs. `tooltip` is the icon's tooltip in the app's language, as the list builds
it, less its last line: the GUI's "Click the icon to repair model object", which agents passed on to
users as an instruction. `mesh_warning_reason` is the one line the sidebar shows. A flagged object
also gets `advice`, what an agent can do: `repair_mesh` repairs open edges, and slicing closes each
layer's outline across gaps of up to 2 mm (`TriangleMeshSlicer`), so a small hole usually prints
closed and a wider one may not. An object with open edges also gets `next_steps`, naming
`repair_mesh`. A mesh that is only repaired (no open edges) prints as it is, and a repair leaves its
recorded repairs as they are.

- An object's `open_edges` and repairs count every volume, modifiers included, as the list does. Its
  `facets`, `shells` and `volume_mm3` count model parts only.
- Repair counts exist only for a mesh loaded from a 3MF that recorded them (its `mesh_stat`). An STL
  is repaired silently on import and records nothing, so a repaired STL reports clean.
- A model part with more than one shell also gets `shell_list`: the 10 shells with the most
  facets, most first, each with its area and bounding box in plate millimetres (instance 0). Facet
  count is not size: a coarse body can have fewer facets than a fine fragment, so read `area_mm2`
  and `bounding_box` to tell which shell is the stray one. The ids are `get_object_components`' ids, which
  `paint_object {selection: "component"}` takes; `get_object_components` lists every shell.
  Listing shells runs a flood fill off the GUI thread; on millions of facets it takes seconds.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |

**Example:**
```json
{"name": "get_mesh_health", "arguments": {"object_id": 0}}
```

**Response** (a part with a hole and a stray shell):
```json
{
  "status": "success",
  "object_id": 0,
  "object_name": "bracket",
  "mesh_warning": true,
  "tooltip": "Remaining errors:\n\t3 non-manifold edges",
  "mesh_warning_reason": "Error: 3 non-manifold edges.",
  "advice": "repair_mesh repairs it with the app's own repair, on every platform: it splits the mesh into its parts, drops parts with no volume and closes each part's holes; painting is cleared unless keep_painting keeps it. Slicing also closes each layer's outline across gaps of up to 2 mm, so a hole that small usually prints closed; a wider one can leave that outline out of a layer, so check the sliced preview there.",
  "summary": {"facets": 1215, "shells": 2, "open_edges": 3, "manifold": false, "repaired": false,
              "errors_repaired": 0,
              "repaired_errors": {"edges_fixed": 0, "degenerate_facets": 0, "facets_removed": 0,
                                  "facets_reversed": 0, "backwards_edges": 0},
              "volume_mm3": 1000.5},
  "volumes": [
    {"volume_id": 0, "name": "bracket", "type": "part",
     "facets": 1215, "shells": 2, "open_edges": 3, "manifold": false, "repaired": false,
     "errors_repaired": 0, "repaired_errors": {"edges_fixed": 0, "degenerate_facets": 0,
     "facets_removed": 0, "facets_reversed": 0, "backwards_edges": 0},
     "mesh_warning": true,
     "tooltip": "Remaining errors:\n\t3 non-manifold edges",
     "mesh_warning_reason": "Error: 3 non-manifold edges.",
     "shell_list": {"total": 2, "listed": 2, "coordinate_frame": "plate", "instance_id": 0,
                    "shells": [{"component": 0, "facet_count": 1203, "area_mm2": 600.2,
                                "bounding_box": {"min": {"x": 118.0, "y": 123.0, "z": 0.0},
                                                 "max": {"x": 128.0, "y": 133.0, "z": 10.0}}},
                               {"component": 1, "facet_count": 12, "area_mm2": 6.0,
                                "bounding_box": {"min": {"x": 137.0, "y": 127.5, "z": 0.0},
                                                 "max": {"x": 138.0, "y": 128.5, "z": 1.0}}}],
                    "note": "Most facets first, at most 10; area_mm2 and bounding_box say which is small. get_object_components lists every shell; its ids are these, the ones paint_object {selection: \"component\"} takes."}}
  ],
  "next_steps": [
    {"tool": "repair_mesh", "arguments": {"object_id": 0},
     "why": "object 0 (\"bracket\") has 3 open edges: repair_mesh closes its holes, after splitting each volume into its shells and dropping those with no volume; painting is cleared unless keep_painting keeps it"}
  ]
}
```

Every row, the object and each volume, has `mesh_warning`; `tooltip` and `mesh_warning_reason` are
there only when it is `true`, the same shape `get_scene_info` and `load_model` use for their objects.

---

### repair_mesh
Repair an object's mesh: close its holes (open edges, non-manifold) with the app's own repair -- the
object list's Repair (`ObjectList::fix_through_cgal`, CGAL), on every platform, and the same code.

What it does, as the object list's Repair does:
- Each volume made of several shells is split into one volume per shell, named `<name>_1`, `<name>_2`
  ..., each with the filament the whole had. The object keeps its place on the plate.
- A shell with no volume -- flat, empty, or thinner than 1e-4 mm -- is deleted. A closed stray shell,
  however small, is kept, as a volume of its own (`get_object_components` lists shells).
- Every shell with open edges has its holes closed. The repaired mesh records no repairs, so its
  warning goes; a closed one-shell mesh whose warning shows repairs recorded at load is left as it is.
- The object is then dropped onto the bed.
- Painting is cleared unless `keep_painting` keeps it: remapped onto the repaired mesh, which the app
  calls experimental. Left out, the app's "Keep painted feature after mesh change" setting decides,
  and `keep_painting_from` says `app_setting`.

It changes nothing when there is nothing to repair (`changed: false` and a `message`), when it is
refused, when a part's repair fails, when it would leave the object no part to print, or when the
repair takes longer than this call may wait (a little under the bridge's `ORCAMCP_TIMEOUT`). Otherwise
it is one undo step, "Repairing model object". When there is a repair to apply, an open toolbar tool
(gizmo) is closed first, as the Repair needs, and named in `closed_toolbar_tool`; closing a painting
tool records its own undo step, as when the user closes it. A `volume_id` the object does not have,
negative included, is refused.

The CGAL work, the remap of kept painting and the repaired parts' convex hulls run off the app's main
thread, so the app stays responsive; the main thread only applies the result (the split, with a
convex hull per new part). On the unoptimised dev build a 20,000-facet sphere with a hole took 0.8 s,
100,000 facets 3.9 s, 500,000 facets 28 s, of which 0.2 s on the main thread. Keeping painting is slow:
about 12 minutes for 10,000 painted facets on that build, all of it off the main thread.

Refused while a slice, an export or an upload runs (`wait_for_slice` first), while an arrange or orient
runs, and while the app's own Repair runs.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `volume_id` | integer | No | Repair only this volume, as `get_mesh_health`'s `volumes` number them. Left out: every volume |
| `keep_painting` | boolean | No | Remap the painting onto the repaired mesh instead of clearing it. Left out: the app's setting |

**Example:**
```json
{"name": "repair_mesh", "arguments": {"object_id": 0}}
```

**Response** (a 20 mm cube with a flat stray sheet beside it: the sheet is dropped, and the cube stays
where it was -- `position` is the box centre, which the sheet had widened):
```json
{
  "status": "success",
  "changed": true,
  "object_id": 0,
  "object_name": "Object_1",
  "keep_painting": false,
  "keep_painting_from": "app_setting",
  "volumes_before": 1,
  "volumes_after": 1,
  "parts": {"split": 2, "dropped": 1, "repaired": 0},
  "before": {"facets": 14, "shells": 2, "open_edges": 4, "manifold": false, "repaired": false,
             "errors_repaired": 0,
             "repaired_errors": {"edges_fixed": 0, "degenerate_facets": 0, "facets_removed": 0,
                                 "facets_reversed": 0, "backwards_edges": 0},
             "mesh_warning": true, "volumes": 1, "painted": false,
             "position": {"x": 128.0, "y": 128.0, "z": 10.0}},
  "after": {"facets": 12, "shells": 1, "open_edges": 0, "manifold": true, "repaired": false,
            "errors_repaired": 0,
            "repaired_errors": {"edges_fixed": 0, "degenerate_facets": 0, "facets_removed": 0,
                                "facets_reversed": 0, "backwards_edges": 0},
            "mesh_warning": false, "volumes": 1, "painted": false,
            "position": {"x": 118.0, "y": 128.0, "z": 10.0}},
  "volumes": [
    {"volume_id": 0, "name": "Object_1_1", "type": "part", "facets": 12, "shells": 1, "open_edges": 0,
     "manifold": true, "repaired": false, "errors_repaired": 0,
     "repaired_errors": {"edges_fixed": 0, "degenerate_facets": 0, "facets_removed": 0,
                         "facets_reversed": 0, "backwards_edges": 0},
     "mesh_warning": false}
  ],
  "plate_index": 0,
  "plate_indices": [0],
  "on_bed": true,
  "instance_placement": [{"instance_id": 0, "plate_index": 0, "on_bed": true,
                          "position": {"x": 118.0, "y": 128.0, "z": 10.0}}],
  "active_warnings": {"count": 0, "warnings": []}
}
```

- `parts.split`: the parts the splits made (0: no volume had several shells); `dropped`: the parts
  with no volume deleted; `repaired`: the parts whose holes were closed.
- `before` / `after`: `get_mesh_health`'s numbers for the object, `mesh_warning`, how many `volumes`,
  whether any is `painted`, and `position` (the centre of its box, plate mm).
- `volumes`: `get_mesh_health`'s rows after the repair; with `volume_id`, `volume_ids_after` lists the
  volumes that one became.
- The placement fields are the transform tools'.

---

## Transform Tools

### move_object
Move an object to a new position.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `x` | number | No | Plate X position/offset (mm) |
| `y` | number | No | Plate Y position/offset (mm) |
| `z` | number | No | Plate Z position/offset (mm), height above the bed |
| `relative` | boolean | No | Relative move (default: true) |
| `include_preview` | boolean | No | Include preview |

**Coordinate frame.** `x`, `y` and `z` are plate millimetres along the *plate's* axes — the same
frame `get_object_info` reports `position` and `bounding_box` in, and the frame this tool's own
`position` comes back in. They are not the object's local axes, so a rotated object still moves,
turns and scales along the plate's X, Y and Z. Before v2.3.2 these tools transformed the object's
*mesh*, beneath the instance transform, so on an object whose instance carried a 90° X rotation a
−84 mm Y move came out as a +84 mm Z move and left the part floating 84 mm above the bed, sliced
that way with no error.

Every instance of the object moves by the same amount, so a multi-instance object keeps its
arrangement and the reported `position` — the whole object's bounding-box centre — is the one the
caller asked for.

**Examples:**
```json
// Relative move: shift 10mm in X
{"name": "move_object", "arguments": {"object_id": 0, "x": 10}}

// Absolute position: center of bed
{"name": "move_object", "arguments": {"object_id": 0, "x": 155, "y": 155, "relative": false}}

// Move onto plate 4 (whose area is x[307,563] y[-307,-51]): the object is re-homed onto plate 4
// and the response reports "plate_index": 3
{"name": "move_object", "arguments": {"object_id": 15, "x": 462, "y": -105, "relative": false}}
```

**Placement in the response.** Every transform re-homes each instance onto the plate whose area
now contains it, then answers about the plate each one is on:

| Field | Meaning |
|-------|---------|
| `instance_placement` | One entry per instance: `instance_id`, `plate_index` (the plate it is on, or `null`), `on_bed` (whether its own box is inside that plate's printable area) and `position` (that box's centre) |
| `plate_index` | The plate instance 0 is on after the transform, or `null` when it is on no plate |
| `plate_indices` | Every plate one of the object's instances is on, ascending |
| `on_bed` | Whether every instance is inside the plate it is on |
| `placement_warning` | Present only when `on_bed` is false: which instance is off its plate or on none. A one-instance object's reads as before: "Object positioned outside the printable area of plate N" or "Object is not on any plate" |

Before v2.5.0.6 `on_bed` measured the box around every instance against instance 0's plate, so an
object with a copy inside each of two plates read `on_bed: false`, "outside the printable area of
plate 0".

Before v2.3.2 `on_bed` was measured against whichever plate happened to be *selected*, so a correct
move into another plate's area was reported as "outside printable area"; `move_object` also left the
object registered on its old plate, which sliced it onto the wrong plate with no error.

---

### rotate_object
Rotate an object.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `x` | number | No | Rotation about the plate's X axis (degrees) |
| `y` | number | No | Rotation about the plate's Y axis (degrees) |
| `z` | number | No | Rotation about the plate's Z axis, the vertical (degrees) |
| `relative` | boolean | No | `true` (the default): `x`, `y` and `z` are the change in degrees. `false` is refused: absolute rotation is not supported |
| `include_preview` | boolean | No | Include preview |

**Example:**
```json
{"name": "rotate_object", "arguments": {"object_id": 0, "z": 45}}
```

Every rotation is a change from where the object is. To reach an orientation, subtract its
`rotation_degrees` (from `get_object_info`) from the one wanted and pass the difference.
`relative: false` used to be accepted and applied as a change anyway; it is now refused with
"absolute rotation is not supported: pass the change in degrees, relative to rotation_degrees from
get_object_info".

**Coordinate frame.** `x`, `y` and `z` are plate millimetres along the *plate's* axes — the same
frame `get_object_info` reports `position` and `bounding_box` in, and the frame this tool's own
`position` comes back in. They are not the object's local axes, so a rotated object still moves,
turns and scales along the plate's X, Y and Z. Before v2.3.2 these tools transformed the object's
*mesh*, beneath the instance transform, so on an object whose instance carried a 90° X rotation a
−84 mm Y move came out as a +84 mm Z move and left the part floating 84 mm above the bed, sliced
that way with no error.

Rotations are applied X, then Y, then Z, about the object's bounding-box centre, so the object turns
in place. The `rotation_degrees` in the response are the instance's own — the same numbers
`get_object_info` reports — and now change to reflect what was asked; before v2.3.2 they never moved,
because the rotation went into the mesh instead.

**The object stays on the bed.** After the turn, the object is dropped back onto Z = 0, as the GUI
does after a rotation, unless it was sinking below the bed before; a sunk object stays sunk.
Instances with `auto_drop` off (set in some 3MF files) are left where the turn put them. Before
v2.5.0.6 a rotation about the centre left the object partly under the bed or floating above it.
`on_bed` measures the object's exact geometry; before v2.5.0.6 it measured the mesh's own box turned
with the object, whose corners sit below a tilted part, so a part standing on the bed read
`on_bed: false`.

**Placement in the response.** Every transform re-homes each instance onto the plate whose area
now contains it, then answers about the plate each one is on:

| Field | Meaning |
|-------|---------|
| `instance_placement` | One entry per instance: `instance_id`, `plate_index` (the plate it is on, or `null`), `on_bed` (whether its own box is inside that plate's printable area) and `position` (that box's centre) |
| `plate_index` | The plate instance 0 is on after the transform, or `null` when it is on no plate |
| `plate_indices` | Every plate one of the object's instances is on, ascending |
| `on_bed` | Whether every instance is inside the plate it is on |
| `placement_warning` | Present only when `on_bed` is false: which instance is off its plate or on none. A one-instance object's reads as before: "Object positioned outside the printable area of plate N" or "Object is not on any plate" |

Before v2.5.0.6 `on_bed` measured the box around every instance against instance 0's plate, so an
object with a copy inside each of two plates read `on_bed: false`, "outside the printable area of
plate 0".

Before v2.3.2 `on_bed` was measured against whichever plate happened to be *selected*, so a correct
move into another plate's area was reported as "outside printable area"; `move_object` also left the
object registered on its old plate, which sliced it onto the wrong plate with no error.

---

### scale_object
Scale an object.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `x` | number | No | Scale factor along the plate's X axis (must be > 0) |
| `y` | number | No | Scale factor along the plate's Y axis (must be > 0) |
| `z` | number | No | Scale factor along the plate's Z axis, the vertical (must be > 0) |
| `uniform` | boolean | No | Apply X scale to all axes. With `uniform`, give `x`: a `y` or `z` without it is refused ("uniform scales every axis by x: give x"), since it used to be ignored |
| `include_preview` | boolean | No | Include preview |

**Examples:**
```json
// Uniform scale: 150%
{"name": "scale_object", "arguments": {"object_id": 0, "x": 1.5, "uniform": true}}

// Non-uniform: double the height above the bed, whatever the object's rotation
{"name": "scale_object", "arguments": {"object_id": 0, "z": 2.0}}
```

**Coordinate frame.** `x`, `y` and `z` are plate millimetres along the *plate's* axes — the same
frame `get_object_info` reports `position` and `bounding_box` in, and the frame this tool's own
`position` comes back in. They are not the object's local axes, so a rotated object still moves,
turns and scales along the plate's X, Y and Z. Before v2.3.2 these tools transformed the object's
*mesh*, beneath the instance transform, so on an object whose instance carried a 90° X rotation a
−84 mm Y move came out as a +84 mm Z move and left the part floating 84 mm above the bed, sliced
that way with no error.

Scaling is about the object's bounding-box centre, so it grows in place, and the `scale` in the
response is the instance's own factor — the number `get_object_info` reports. The object is then
dropped back onto the bed (Z = 0), as the GUI does, unless it was sinking below it before. Before
v2.5.0.6 it was not: a uniform 1.49× scale of a 99 mm figurine left its feet 24 mm under the bed.
Factors must be positive: zero makes the instance transform singular, and a negative factor is a mirror, which
`mirror_object` does properly.

A *non-uniform* scale along plate axes on an object whose rotation is not a multiple of 90° is a
shear, and nothing can make it otherwise. It is applied, and the response carries a `skew_warning`
saying so. The GUI avoids this by refusing world coordinates for such an object; use `uniform: true`,
or unrotate the object first. Uniform scale is frame-independent and always exact.

**Placement in the response.** Every transform re-homes each instance onto the plate whose area
now contains it, then answers about the plate each one is on:

| Field | Meaning |
|-------|---------|
| `instance_placement` | One entry per instance: `instance_id`, `plate_index` (the plate it is on, or `null`), `on_bed` (whether its own box is inside that plate's printable area) and `position` (that box's centre) |
| `plate_index` | The plate instance 0 is on after the transform, or `null` when it is on no plate |
| `plate_indices` | Every plate one of the object's instances is on, ascending |
| `on_bed` | Whether every instance is inside the plate it is on |
| `placement_warning` | Present only when `on_bed` is false: which instance is off its plate or on none. A one-instance object's reads as before: "Object positioned outside the printable area of plate N" or "Object is not on any plate" |

Before v2.5.0.6 `on_bed` measured the box around every instance against instance 0's plate, so an
object with a copy inside each of two plates read `on_bed: false`, "outside the printable area of
plate 0".

Before v2.3.2 `on_bed` was measured against whichever plate happened to be *selected*, so a correct
move into another plate's area was reported as "outside printable area"; `move_object` also left the
object registered on its old plate, which sliced it onto the wrong plate with no error.

---

### mirror_object
Mirror an object along an axis.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `axis` | string | Yes | Plate axis to mirror across: "x", "y", or "z" |
| `include_preview` | boolean | No | Include preview |

**Coordinate frame.** `x`, `y` and `z` are plate millimetres along the *plate's* axes — the same
frame `get_object_info` reports `position` and `bounding_box` in, and the frame this tool's own
`position` comes back in. They are not the object's local axes, so a rotated object still moves,
turns and scales along the plate's X, Y and Z. Before v2.3.2 these tools transformed the object's
*mesh*, beneath the instance transform, so on an object whose instance carried a 90° X rotation a
−84 mm Y move came out as a +84 mm Z move and left the part floating 84 mm above the bed, sliced
that way with no error.

The reflection is across a plate plane through the object's bounding-box centre, so the object stays
where it is, and a resting object stays on the bed (it is dropped to Z = 0 afterwards, as in the GUI,
unless it was sinking). Before v2.3.2 it reflected the mesh about the volume origin, which moved an asymmetric
object by its own width — and about the object's local axis, so on a rotated object "mirror z" was
not a vertical flip at all.

**Placement in the response.** Every transform re-homes each instance onto the plate whose area
now contains it, then answers about the plate each one is on:

| Field | Meaning |
|-------|---------|
| `instance_placement` | One entry per instance: `instance_id`, `plate_index` (the plate it is on, or `null`), `on_bed` (whether its own box is inside that plate's printable area) and `position` (that box's centre) |
| `plate_index` | The plate instance 0 is on after the transform, or `null` when it is on no plate |
| `plate_indices` | Every plate one of the object's instances is on, ascending |
| `on_bed` | Whether every instance is inside the plate it is on |
| `placement_warning` | Present only when `on_bed` is false: which instance is off its plate or on none. A one-instance object's reads as before: "Object positioned outside the printable area of plate N" or "Object is not on any plate" |

Before v2.5.0.6 `on_bed` measured the box around every instance against instance 0's plate, so an
object with a copy inside each of two plates read `on_bed: false`, "outside the printable area of
plate 0".

Before v2.3.2 `on_bed` was measured against whichever plate happened to be *selected*, so a correct
move into another plate's area was reported as "outside printable area"; `move_object` also left the
object registered on its old plate, which sliced it onto the wrong plate with no error.

---

### flatten_object
Orient one object to lay flat on its best face, the way the GUI's **Orient** does for a selection.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `include_preview` | boolean | No | Include preview |

The object replaces the current selection and is turned, every instance of it; no other object
moves. Before v2.5.0.6 this oriented every object on the current plate. It answers once the orient
has been applied, with the object's placement as `rotate_object` reports it (`status: success`,
`position`, `rotation_degrees`, `scale`, `changed` and the placement fields; see
[Waiting for the job](#waiting-for-the-job)). One `undo` puts the object back; it comes back selected.

Refused, with nothing selected or oriented, when the job would not orient this object alone: another
job (an arrange or an orient) is still running; the object is marked not printable, which the orient
job leaves out (and, finding nothing selected, would orient every other object instead); an instance
of it is on a locked plate (the job does not turn it, but drops the whole object by its first
instance's new bottom, which moves the locked instance up or down, maybe into the bed; the message
names the instances); or the 3D view has not caught up with the object (it postpones its scene
reloads while another tab is shown, and this call first asks it to catch up), so the selection is
not exactly this object. Each message says what to do instead.

---

## Object Operations

### clone_object
Create copies of an object.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `count` | integer | No | Number of clones, 1 or more (default: 1). A count below 1 is refused before anything runs: it used to make no copy and still rearrange the plate |
| `duplicate` | boolean | No | Independent copies (true) vs linked instances (false) |
| `destination_plate` | integer | No | Target plate (default: current plate) |
| `include_preview` | boolean | No | Include preview |

**Example:**
```json
{"name": "clone_object", "arguments": {"object_id": 0, "count": 3, "duplicate": true}}
```

The copies are placed by an arrange of the destination plate, and the answer comes once it has been
applied: besides `copies_created`, `new_object_ids` (duplicates) or `total_instances` (instances), it
lists `objects`, every object on that plate with its placement, as `arrange_objects` does (see
[Waiting for the job](#waiting-for-the-job)). While another job runs it is refused before anything is
copied.

---

### cut_object
Cut an object at a specified Z height.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `z_height` | number | Yes | Cut height in plate mm, measured from the bed |
| `keep` | string | No | "below" (the default), "above", or "both". Any other value is refused, listing these: it used to be cut as "below" |

**Example:**
```json
{"name": "cut_object", "arguments": {"object_id": 0, "z_height": 25, "keep": "below"}}
```

`z_height` is in plate millimetres, the same frame `get_object_info` reports, and the object's own
rotation is accounted for: `Cut` brings each mesh into the cut plane's frame with
`get_matrix_no_offset()`, so the instance's rotation and scale are already applied there and the
handler only has to subtract the instance's Z offset.

---

### delete_object
Remove an object from the project.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |

---

### rename_object
Rename an object. The name is in the G-code (object labels, `EXCLUDE_OBJECT` names,
`{first_object_name}`), so the plates holding the object are marked not sliced, and the next slice
writes their G-code again with the new name.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `new_name` | string | Yes | New name |

---

### transform_objects
Apply transforms to multiple objects in batch.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `transforms` | array | Yes | Array of transform operations |

Each entry takes `object_id` plus any of `position` (absolute, unspecified axes preserved), `rotation`
(degrees, incremental) and `scale` (factors, or `{"uniform": v}`).

**Example:**
```json
{
  "name": "transform_objects",
  "arguments": {
    "transforms": [
      {"object_id": 0, "position": {"x": 10, "y": 0}},
      {"object_id": 1, "rotation": {"z": 90}},
      {"object_id": 2, "scale": {"uniform": 1.5}}
    ]
  }
}
```

All three are in the plate's frame, exactly as `move_object`, `rotate_object` and `scale_object`
apply them — see the coordinate-frame note on `move_object`. A rotation or scale drops a resting
object back onto the bed, as `rotate_object` and `scale_object` do, unless the entry gives
`position.z`: an explicit Z is kept as given, as `move_object` keeps it.

**All or nothing.** Every entry is checked before any is applied. If one is rejected -- an
`object_id` that is not an object, a value that is not a number (`{"z": "90"}`), a `position`,
`rotation` or `scale` that is not an object, or scale factors that are not all positive -- nothing is applied,
`status` is `error`, and `results` lists each rejected entry with its position in the batch
(`entry`), its `object_id` and the reason. A batch that applied the good entries and rejected the
rest used to leave an object moved but unreported, still counted on its old plate. One undo step
undoes the whole batch; a batch that changes nothing takes none.

On success `results` has one entry per transform, in order, with `entry`, `object_id`, `position`,
`changed` and the same placement fields the single-object transforms return (`instance_placement`,
`plate_index`, `plate_indices`, `on_bed`, `placement_warning`), each instance measured on the plate
it landed on. An object named in several
entries reports where it ended up in each.

---

## Plate Tools

### add_plate
Add a new build plate to the project.

**Parameters:** None

**Returns:** New plate index

---

### select_plate
Switch to a different plate.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `plate_index` | integer | Yes | Plate to select (0-indexed) |

---

### delete_plate
Remove a plate from the project. A slice in progress is cancelled first, and a Slice All run with it:
the response then carries `slice_cancelled: true` and an `info_messages` line saying so; call
`slice_all` again. Plates sliced before keep their results. A call that deletes nothing (an index out
of range, the last plate) leaves the slice running.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `plate_index` | integer | Yes | Plate to delete |

---

### set_prime_tower_position
Move a plate's prime tower. The tower is printed plastic occupying bed area; without this tool an
agent could see a prime-tower collision and had no way to resolve it except asking the user to drag
the tower in the GUI.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `x` | number | Yes | Tower body front-left corner X, **plate millimetres** |
| `y` | number | Yes | Tower body front-left corner Y, **plate millimetres** |
| `plate_index` | integer | No | Plate to move the tower on (0-based). Default: the selected plate |

`x` / `y` are in the same frame `get_object_info` and `get_scene_info` report object bounding boxes
in, and are exactly what `get_scene_info` returns as `plates[].prime_tower.position`: read it,
adjust it, write it back. They are a **corner**, not a centre — see the note under `get_scene_info`.
The tool converts to the plate-local `wipe_tower_x` / `wipe_tower_y` project keys itself.

**Validation.** The position is refused when the tower **plus its brim** would not fit inside the
plate's printable area; the error carries `allowed_range` in plate millimetres. That range is the
one OrcaSlicer's own arranger clamps to, so a position this tool accepts is one the slicer keeps.
A tower too large for the plate at all is refused with a message saying so.

Overlapping an object or an excluded area is **not** refused — an agent rearranging a plate moves
things through each other's way on purpose. The move is applied and the overlaps come back in
`conflicts`, with `conflict_note` warning that slicing will report a clearance error until it is
resolved.

`undo` puts the tower back: the tool takes a snapshot after validating and before writing.

**Example:**
```json
{"name": "set_prime_tower_position", "arguments": {"plate_index": 2, "x": 40.0, "y": 210.0}}
```

**Returns:**
```json
{
  "status": "success",
  "plate_index": 2,
  "previous_position": {"x": 165.0, "y": 250.0},
  "position": {"x": 40.0, "y": 210.0},
  "allowed_range": {"min_x": 4.0, "max_x": 189.0, "min_y": 4.0, "max_y": 209.0, "frame": "plate_mm"},
  "prime_tower": { "...": "the same object get_scene_info reports" },
  "conflicts": [],
  "active_warnings": {"count": 0, "warnings": []}
}
```

---

## Preset & Config Tools

### get_presets
List the presets available for the **selected printer**. The list is exactly what the preset combo
boxes show: visible presets compatible with the current printer, system and user alike.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type` | string | No | `printer`, `filament`, `print`, or `all` (default) |
| `vendor` | string | No | Only presets from this vendor (case-insensitive substring) |
| `name_contains` | string | No | Only presets whose name contains this (case-insensitive) |
| `summary` | boolean | No | Default `true`: identifying fields only. `false` adds every config key of every match. |
| `limit` | integer | No | Max presets per type. Default 25 with `summary`, 5 without. `0` = no cap. |

**Why it is capped and summarised:** the unfiltered full-config response is ~1.9 MB and the
unfiltered `summary` response is still ~54,600 characters — both over an MCP client's per-result
limit, so the tool could not be answered at all. `summary` decides whether each preset carries its
`config` blob; `limit` decides how many presets of each type come back. The response shape is
otherwise unchanged (the same `printerPresets` / `filamentPresets` / `printProcessPresets` arrays).

When the cap dropped anything, the response carries a top-level `hint` naming the filters, and
`query.truncated` is `true`. `query.counts` still reports **every** preset that matched;
`query.returned` reports how many are in the arrays.

**Example - find a PETG profile for the current printer in one call:**
```json
{"name": "get_presets", "arguments": {"type": "filament", "name_contains": "PETG", "vendor": "Flashforge"}}
```

**Returns:**
```json
{
  "filamentPresets": [
    {
      "name": "Flashforge PETG Pro @FF C5P",
      "is_default": false,
      "is_selected": true,
      "is_system": true,
      "vendor": "Flashforge",
      "version": "02.03.00.01",
      "filament_type": "PETG"
    }
  ],
  "query": {
    "type": "filament",
    "vendor": "Flashforge",
    "name_contains": "PETG",
    "summary": true,
    "limit": 25,
    "compatible_with_selected_printer_only": true,
    "counts": {"filamentPresets": 4},
    "returned": {"filamentPresets": 4},
    "truncated": false
  }
}
```

`filament_type` (filaments) and `printer_model` (printers) are included whenever the preset has
them, so material searches do not need the full config.

**Returns (unfiltered, truncated):**
```json
{
  "filamentPresets": ["... 25 presets ..."],
  "query": {
    "type": "filament",
    "vendor": "",
    "name_contains": "",
    "summary": true,
    "limit": 25,
    "compatible_with_selected_printer_only": true,
    "counts": {"filamentPresets": 318},
    "returned": {"filamentPresets": 25},
    "truncated": true
  },
  "hint": "Showing 25 of 318 filamentPresets. Narrow it with type, vendor or name_contains, raise limit, or pass limit: 0 for the whole list."
}
```

---

### get_edited_presets
Every setting of the selected printer, print and filament presets, unsaved ones marked: 25-48 KB. For
a few settings, or which presets are selected, use `get_config_values` (under 1 KB).

**Parameters:** None

**Returns:** Current presets with `dirty_options` arrays showing modified settings. Every key of
three presets: 25-48 KB. For a few settings, or which presets are selected, use `get_config_values`.

---

### get_config_values
Which presets are selected, and the values of just the settings you name. A cheap read an agent
can repeat as it works.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `keys` | array of strings | No | Setting keys to read. Omit for the selected presets alone |
| `dirty_only` | boolean | No | Only settings whose value differs from the saved preset. With no `keys`: every unsaved change. Default `false` |

**Returns (no arguments):** the selected presets, about 400 bytes with four slots:
```json
{
  "status": "success",
  "presets": {
    "printer": {"name": "Flashforge Creator 5 Pro 0.4 nozzle", "dirty": false},
    "print": {"name": "0.20mm Standard @FF C5P", "dirty": true},
    "filaments": [
      {"slot": 1, "name": "Flashforge PLA @FF C5P", "dirty": false},
      {"slot": 2, "name": "Flashforge PETG Pro @FF C5P", "dirty": false}
    ]
  }
}
```

**Returns (`keys`):** each value grouped under where it lives, which is the `type` `apply_config`
takes for it. A filament setting has one value per slot.
```json
{
  "status": "success",
  "values": {
    "print": {"support_type": "tree(auto)", "support_threshold_angle": "30"},
    "filament": {"filament_type": ["PLA", "PETG"]},
    "project": {"filament_colour": "#FFFFFF;#1A1A1A"}
  },
  "dirty": {"print": {"support_type": {"saved": "normal(auto)"}}}
}
```

| Field | Meaning |
|-------|---------|
| `values` | `print`, `filament`, `printer` and `project` groups, present only when a key lives there. Values are the slicer's text, as `get_edited_presets` shows them and `apply_config` accepts them. Credentials (`printhost_apikey`, `printhost_password`, `flashforge_obico_token`) are never shown: a set one reads `"<redacted>"`, an empty one `""` |
| `dirty` | The keys whose value differs from the saved preset, with the saved value; a changed credential is `{changed: true, secret: true}`, with neither value. For a filament key: `slots`, the slots whose preset the Filament tab is editing, and their `saved` value. Omitted when nothing differs. With `dirty_only` and no `keys`, every unsaved change as `{key: {value, saved}}` (filament: `{slots, value, saved}`) |
| `not_judged` | `{keys, reason}` for the project settings asked for: they have no saved preset to compare with, so neither `dirty` nor its absence says anything about them. Listed with `dirty_only` too, rather than dropped |
| `not_in_presets` | Known keys that belong to no preset type nor the project (an object-only key such as `extruder`) |

A key's group comes from the preset type that defines it, so a filament setting is still recognised
when a slot's preset no longer exists (its value there is `null`). A key no preset type lists but a
preset carries all the same, such as `print_settings_id`, comes from the selected preset that carries
it.

A dozen keys cost well under 1 KB. An unknown key is an error naming it, in `unknown_keys`;
`get_valid_config_keys` lists the valid ones.

---

### select_preset
Switch to a different preset. To change individual settings, use `apply_config`.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type` | string | Yes | "printer", "filament", or "print" |
| `name` | string | Yes | Preset name |
| `slot` | integer | No | With `type: filament`: the 1-based filament slot to set, like the sidebar combo |

**Returns (`type: printer`):** what the switch left in the filament slots, because upstream's
*Remember printer configuration* (on by default) replaces every slot's colour on a printer switch:
```json
{
  "status": "success",
  "printer": "C5P",
  "colors_source": "mixed",
  "filaments": [
    {"slot": 1, "preset": "Flashforge PETG Pro @FF C5P", "type": "PETG", "color": "#1A1A1A",
     "previous_color": "#D4AF37", "color_source": "remembered", "is_mixed": false},
    {"slot": 2, "preset": "Flashforge PLA Silk @FF C5P", "type": "PLA", "color": "#26A69A",
     "color_source": "default", "is_mixed": false}
  ]
}
```
Each slot's `color_source` is observed, by comparing the colours before and after the switch:
`unchanged`, `remembered` (the colour last saved for that printer), `default` (`#26A69A`, upstream's
fill for a slot nothing was saved for) or `other`. `colors_source` sums them up: `unchanged`, the
one source every changed slot shares, or `mixed`. `set_filament_color` and `select_preset {slot}`
save the colours for the selected printer, so a switch away and back returns them. An unknown printer name is an error.

---

### apply_config
Change settings of the selected print, filament or printer preset, or of the project, several in one
call. The change stays unsaved in the preset until `save_preset`. To switch presets, use
`select_preset`; for one object only, `set_object_config`; for a slot's colour on the plate,
`set_filament_color`.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `settings` | array | Yes | Array of setting changes |

**Setting object format:**
```json
{"type": "print|filament|printer|project", "key": "setting_name", "value": "new_value"}
```
`project` writes the project config — that is where `filament_colour` and the flush volumes live.

**Examples:**
```json
// Change layer height
{"name": "apply_config", "arguments": {
  "settings": [{"type": "print", "key": "layer_height", "value": "0.2"}]
}}

// Multiple settings
{"name": "apply_config", "arguments": {
  "settings": [
    {"type": "print", "key": "layer_height", "value": "0.15"},
    {"type": "print", "key": "wall_loops", "value": "3"},
    {"type": "print", "key": "sparse_infill_density", "value": "20%"}
  ]
}}
```

**Returns:**
```json
{
  "status": "success",
  "applied_keys": ["layer_height"],
  "invalid_keys": [],
  "unknown_keys": [],
  "rejected_values": [],
  "duplicate_keys": [],
  "active_warnings": {"count": 0, "warnings": []}
}
```

**Duplicates:** listing the same `type` + `key` twice in one call applies the **last** value (the
same as two separate calls would). Each such key is reported in `duplicate_keys` as
`{"type", "key", "occurrences", "applied_value"}` so a batch built programmatically cannot lose
half its writes silently. `set_object_config` reports the same array per object.

**Colours:** a colour-typed key (`filament_colour`, `extruder_colour`, ...) must be `#RRGGBB` or
`#RRGGBBAA`; an empty value means "no colour". Anything else (`B17C38`, `#GGGGGG`) is rejected into
`invalid_keys` with the previous value kept, instead of being stored and later decoded as black.

**Lists:** a key that `get_valid_config_keys` reports as `strings` / `ints` / `bools` / `floats` takes
a JSON array, and the joined string form keeps working:

```json
{"type": "project", "key": "filament_colour", "value": ["#00FFFF", "#FF00FF", "#FFFF00", "#808080"]}
{"type": "project", "key": "filament_colour", "value": "#00FFFF;#FF00FF;#FFFF00;#808080"}
```

Both apply the same four colours. The separator differs per type inside the slicer (`;` for string
lists, `,` for numeric and boolean lists), which is exactly why passing an array is the safer form.

**Why a key failed:** `invalid_keys` is the union of two different problems and stays that way, but
each has its own field now:

| Field | Meaning |
|---|---|
| `unknown_keys` | No such config key. Check `get_valid_config_keys`. |
| `rejected_values` | The key exists; this value was not accepted. Each entry is `{"key", "reason", "expected"}`, where `expected` names the shape that would have worked. |

```json
{
  "status": "partial",
  "applied_keys": [],
  "invalid_keys": ["layer_height"],
  "unknown_keys": [],
  "rejected_values": [
    {"key": "layer_height", "reason": "an array was given for a key that is not a list",
     "expected": "a number"}
  ]
}
```

---

### clone_preset
Clone an existing preset with a new name. Creates a user preset from any source (including system presets).

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type` | string | Yes | Preset type: "printer", "filament", or "print" |
| `source_name` | string | Yes | Name of the preset to clone |
| `new_name` | string | Yes | Name for the new cloned preset |

**Example:**
```json
{"name": "clone_preset", "arguments": {
  "type": "print",
  "source_name": "0.20mm Standard @BBL X1C",
  "new_name": "My Custom 0.20mm"
}}
```

**Returns:**
```json
{
  "status": "success",
  "message": "Preset cloned successfully",
  "new_preset": "My Custom 0.20mm"
}
```

---

### save_preset
Save current dirty changes to a preset. If name is provided, saves as a new preset. Otherwise saves to the current preset (fails for system presets).

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type` | string | Yes | Preset type: "printer", "filament", or "print" |
| `name` | string | No | Save as new preset with this name. Omit to save current. |

**Examples:**
```json
// Save changes to current preset
{"name": "save_preset", "arguments": {
  "type": "print"
}}

// Save as new preset
{"name": "save_preset", "arguments": {
  "type": "print",
  "name": "My New Preset"
}}
```

**Returns:**
```json
{
  "status": "success",
  "message": "Preset saved successfully",
  "saved_preset": "My New Preset"
}
```

**Note:** Cannot overwrite system presets. Use `clone_preset` first if you need to modify a system preset.

---

### delete_preset
Delete a user-created preset. Cannot delete system/default presets or presets with dependents.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type` | string | Yes | Preset type: "printer", "filament", or "print" |
| `name` | string | Yes | Name of the preset to delete |

**Example:**
```json
{"name": "delete_preset", "arguments": {
  "type": "print",
  "name": "My Custom Preset"
}}
```

**Returns:**
```json
{
  "status": "success",
  "message": "Preset 'My Custom Preset' deleted successfully"
}
```

**Safety:** System presets and presets with child dependents cannot be deleted.

---

### reset_preset
Discard all unsaved changes to the current preset and revert to the last saved state.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type` | string | Yes | Preset type: "printer", "filament", or "print" |

**Example:**
```json
{"name": "reset_preset", "arguments": {
  "type": "print"
}}
```

**Returns:**
```json
{
  "status": "success",
  "message": "Preset changes discarded for print"
}
```

**Use case:** Undo experimental changes made via `apply_config` without saving them.

---

### get_valid_config_keys
List the setting keys of a category, to find the key for a setting (`support_type`,
`sparse_infill_density`, ...).

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `category` | string | No | "per_object" (the default), "print", "filament", "printer", "toolchanger", "project", or "all". Any other value is refused, listing these: it used to return no keys |
| `include_descriptions` | boolean | No | Add each key's description |

---

## Per-Object Config Tools

### get_object_info
One object's position, rotation, scale, bounding box, placement, and every volume (part, modifier,
negative volume, support blocker) with its type and filament. It does not check the mesh:
`get_mesh_health` reports holes and open edges, `get_object_components` loose parts and stray shells.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |

**Placement fields:** alongside `position`, `bounding_box`, `rotation_degrees` and `scale`, the
response carries the same placement fields the transform tools return:

| Field | Meaning |
|-------|---------|
| `instance_placement` | One entry per instance: `instance_id`, `plate_index` (the plate it is on, or `null`), `on_bed` (whether its own box is inside **that** plate's printable area) and `position` (that box's centre) |
| `plate_index` | The plate instance 0 is on, or `null` if it is on none |
| `plate_indices` | Every plate one of the object's instances is on, ascending |
| `on_bed` | Whether every instance is inside the plate it is on |
| `placement_warning` | Present only when `on_bed` is false: which instance is off its plate or on none |

`position` and `bounding_box` stay the object's: for an object with copies on two plates they span
both, and `instance_placement` says where each copy is. Before v2.5.0.6 `on_bed` measured that box
against instance 0's plate, so such an object read `on_bed: false`.

Before v2.3.2 `on_bed` here was measured against whichever plate happened to be *selected*, and
`plate_index` was not reported at all. Plates do not share a coordinate range, so an object sitting
correctly on plate 4 read as off the bed whenever another plate was selected.

`on_bed` still only means "inside the plate in XY, and not sunk below Z". It does not check for
collisions with other objects or the prime tower, and a part floating above the bed passes it.

**Which box.** `position` (the box's centre), `bounding_box`, `on_bed` and, in `get_scene_info`,
the object's `printed_footprint` and `occupancy` footprint and height are all the object's exact
box: every vertex of every instance, transformed. In `get_scene_info` each plate's entry for an
object covers only the instances that plate holds, listed in `instances_on_plate` (on the entry and
on its `occupancy` item), and its `rotation_degrees` and `scale` are the first of those copies';
`instance_count` stays the object's total, and `get_object_info` stays object-wide (instance 0). Before v2.5.0.6 an object with a
copy on another plate was reported under each plate with the box spanning both, so its footprint on
plate 0 could be 347 mm wide. `move_object`'s `position` and
`transform_objects`' `position` are read and written in the same box. Before v2.5.0.6 they used the
mesh's own box turned with the object, whose corners stand off a rotated part: a T-shaped part
tilted 30 degrees and resting on the bed read `min.z` -4 mm, with a footprint several millimetres
too wide labelled `footprint_is_exact: true`. The exact box is cached on the object, so only the
first call after a change walks the mesh, and that walk costs the same as the old box's did.

**Filament fields:** `filament` is the object's own slot. `filaments_used` is every slot the object
actually prints with — volumes, painted facets and layer ranges — and `volumes` lists every volume
of the object, not just the printable parts `get_object_components` shows:

```json
"filament": 3,
"filaments_used": [1, 3],
"volumes": [
  {"volume_id": 0, "name": "Base",          "type": "part",     "own_filament": null, "effective_filament": 3},
  {"volume_id": 1, "name": "Base Modifier", "type": "modifier", "own_filament": 1,    "effective_filament": 1}
]
```

`type` is one of `part`, `modifier`, `negative_volume`, `support_blocker`, `support_enforcer`.
`own_filament` is `null` when the volume inherits the object's slot; `effective_filament` is what
prints. A volume's own slot beats the object's, so when `filaments_used` has more than one entry
this is where to look for the volume responsible — and `volume_id` here is the index
`set_object_filament` takes.

---

### get_object_config
Get per-object configuration overrides.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |

---

### set_object_config
Override settings for one object only (supports, infill, walls, layer height, ...), leaving the
presets and the other objects alone.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `settings` | array | Yes | Array of `{key, value}` pairs |

**Example:**
```json
{"name": "set_object_config", "arguments": {
  "object_id": 0,
  "settings": [
    {"key": "sparse_infill_density", "value": "30%"},
    {"key": "enable_support", "value": "1"}
  ]
}}
```

**Lists and failures:** identical to `apply_config` — a list-typed key takes a JSON array or the
joined string, `unknown_keys` holds keys that do not exist, and `rejected_values` holds
`{"key", "reason", "expected"}` for values this key would not take. `invalid_keys` remains the union
of both, per object. `settings` that is not a list of `{key, value}` -- an object such as
`{"wall_loops": 3}`, or an item without a `key` or `value` -- is refused with an error saying what is
wrong (the same check `apply_config`, whose items also need a `type`, and `set_object_layer_range`
make).

---

### reset_object_config
Clear per-object configuration overrides.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `keys` | array | No | Specific keys to reset. Omitted: every override but the object's filament (`extruder`), which stays, as the GUI's reset leaves it. `reset_count` is how many were cleared; a reset that clears nothing takes no undo snapshot |

An empty `keys`, or one that is not a list of setting names, is refused: both used to reset every
override. Leave `keys` out to reset them all.

---

## Layer Range Tools

### get_object_layer_ranges
Get layer-specific settings for an object. Range Z is measured from the object's own base, not from
the bed, so it equals plate Z only while the object sits on the bed.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |

---

### set_object_layer_range
Set settings for a specific Z height range. `z_min`/`z_max` are measured from the object's own base,
not from the bed, so they equal plate Z only while the object sits on the bed — moving the object up
does not move its ranges.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `z_min` | number | Yes | Range start (mm above the object's own base) |
| `z_max` | number | Yes | Range end (mm above the object's own base) |
| `settings` | array | Yes | Array of `{key, value}` pairs |

**Example:**
```json
{"name": "set_object_layer_range", "arguments": {
  "object_id": 0,
  "z_min": 10,
  "z_max": 20,
  "settings": [{"key": "layer_height", "value": "0.1"}]
}}
```

**Returns:**
```json
{
  "status": "success",
  "object_id": 0,
  "range": [10, 20],
  "applied_count": 1,
  "applied_keys": ["layer_height"],
  "invalid_keys": [],
  "unknown_keys": [],
  "rejected_values": []
}
```

**Lists and failures:** the same shape as `apply_config` and `set_object_config`, with two
differences — a layer range reports no `duplicate_keys`, and a call where nothing applied returns
`status: "error"` — otherwise a list-typed key takes
a JSON array or the joined string, `unknown_keys` holds keys that do not exist, `rejected_values`
holds `{"key", "reason", "expected"}` for values this key would not take, and `invalid_keys` is the
union. `applied_count` counts only what was written, so `status` is `partial` when some keys applied
and `error` when none did, or when `settings` is empty (`message` says so). A call where nothing applied leaves the object's ranges as they were.

**Every range has a `layer_height` and an `extruder`**, as the GUI's object list gives a new range
them: the object's own layer height (its override, else the selected process preset's, within what
the nozzle of the extruder that prints the range prints -- the range's own extruder, else the
object's) and extruder `0` (the object's), unless `settings` gives them. The range as stored must
print on that nozzle: an `extruder` whose nozzle cannot print the range's layer height is rejected
too, in `rejected_values`, and the range keeps its extruder. A range without a
layer height crashed the next slice, and a range a loaded file carries without one is completed the
same way when its objects enter the scene. A
`layer_height` the printer cannot print -- 0 or less, or outside its `min_layer_height` ..
`max_layer_height` for the nozzle of the filament that prints the range (its own, else the object's; each tool's own on a toolchanger; three quarters of the nozzle when the maximum is 0) -- is
rejected in `rejected_values`, as the object list's range editor refuses it.

---

### delete_object_layer_range
Remove a layer range configuration. Range Z is measured from the object's own base, not from the bed.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `z_min` | number | No | Start of the range to delete, with `z_max` |
| `z_max` | number | No | End of the range to delete, with `z_min` |

Pass both bounds to delete that one range, or neither to delete **every** range of the object.
One without the other is refused, naming the missing bound: it used to delete every range. The
response's `deleted_count` says how many went; `0`, and no undo step, when the range was not
there.

---

## Slicing Tools

### slice_all
Slice every plate in the project, one after another, exactly as the GUI's **Slice All** button does.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `all_plates` | boolean | No | `true` (default) slices every plate; `false` slices only the currently selected plate |

**Returns:**
```json
{
  "status": "slicing_started",
  "scope": "all_plates",
  "plates_to_slice": 4,
  "selected_plate_at_call": 0,
  "note": "Slicing all 4 plates. The plate selection walks to the last plate while it runs; get_slicing_status restores plate 0 when the run ends.",
  "active_warnings": {"count": 0, "warnings": []},
  "next_steps": [{"tool": "wait_for_slice", "why": "the slice runs in the background: wait_for_slice returns once it is over, with each plate's result"}]
}
```

**`status`** says what actually happened:

| `status` | Meaning |
|----------|---------|
| `slicing_started` | A slice is running (for every plate: the run is under way) |
| `not_started` | Nothing new is slicing. `reason` and `message` say why and what to do |

| `reason` | Meaning |
|----------|---------|
| `busy_slicing` | The pipeline is busy (`get_slicing_status`'s `busy`): a slice or Slice All run, a G-code export, an upload, or the last slice still stopping. `message` says which, e.g. "Slice All is slicing plate_index 2 of 5 plate(s)" or "a G-code export is running". Nothing was started: call `wait_for_slice`, which waits the same state out, then `slice_all` again. Started then, a slice would be stopped by the previous one's completion |
| `already_sliced` | Every plate asked for already has a valid result: nothing to do, and `wait_for_slice` reports `done` |
| `busy_job` | An arrange or orient holds the app: poll `get_slicing_status` until `ui_job` is null, then call `slice_all` again |
| `nothing_to_slice` | No printable object fully on the plates asked for: an object marked unprintable, or partly outside its plate, does not count (`get_object_info`'s `on_bed` and `placement_warning` say which), and neither does one taller than the printable height, which the app leaves out of the slice (`active_warnings`: "laid over the boundary of plate or exceeds the height limit"). A refusal the app gives words for comes first: with a `printable_height` of 0, which the build volume takes for no limit, the object stays in the slice and the app's validation refuses it, reported as `invalid` with its words ("The object ... exceeds the maximum build volume height.") |
| `invalid` | The app refuses a plate it was asked for as it stands, the way the GUI greys its Slice button, ahead of `nothing_to_slice`; `message` says which check, the first that applies: its validation (the plate's validation result, not a guess from `active_warnings`; `message` gives the app's words, e.g. "Prime Tower is partially outside the printable area"), plugins slicing needs but that are missing, a mixed filament that lost a component, a plate not ready to slice (an object partly off the plate or over its height, or a filament that cannot print where it is), or the plate's last slice having failed, which the app does not retry until something on the plate changes. A setting fixed just before the call counts: the app takes in a settings change 0.5 s after it, and `slice_all` applies one still waiting first (so do `get_slicing_status`, `get_print_estimate` and `export_gcode`), so it is not refused on the failure the fix removed |
| `unknown` | No signal explains it; `active_warnings` may |

**Note:** Async operation. Call `wait_for_slice`, which returns once the run is over; `next_steps`
names it (for `slicing_started` and `busy_slicing`), `get_slicing_status` for `busy_job`, and
`get_print_estimate` with a sliced plate's `plate_index` for `already_sliced`. `get_slicing_status` reads the state at any moment.

**Plate selection.** Slicing every plate is driven by the slicer's own per-plate chaining, which
selects each plate in turn, so the selection moves while the run is in progress. The first
`get_slicing_status` poll after the run ends selects the plate that was current when `slice_all`
was called again and reports it as `restored_selected_plate`. This matters because
`export_gcode` and `get_preview_base64` all answer about the *selected* plate.
`get_print_estimate` takes an optional `plate_index` and answers about the selected plate only when
that is omitted.

**A plate already sliced** (its slice finished, and nothing changed it since, even if a preset was
selected again) is counted as sliced without slicing it again. A plate is only ever counted as sliced
if, when its slice (or skip) is taken in, its current slice is finished and it passes validation: an
edit, an undo or an arrange in between leaves it not sliced.

**`slice_all` during a Slice All run starts nothing** (`not_started`, `busy_slicing`, naming the
plate the run is on), whatever `all_plates` says: slicing one plate would end the run half done.

**Deleting a plate during the run** cancels it: the run walks the plates by position, which the
deletion shifts. `delete_plate` says so (`slice_cancelled: true`, "the plate list changed during Slice
All; the run was cancelled, call slice_all again"), and so do `undo` and `redo`, which rebuild the
plate list; a slice that had already finished is not reported as cancelled. A plate the run cannot
start because the app is busy with another job (an arrange, say) ends the run with that plate not
sliced, rather than counting it as sliced: `get_slicing_status` says `slice_run.ended_early`. Plates sliced before keep their results. The plate
restored when the run ends is the one that was selected at the call, wherever it now stands, or none
if it was the deleted one.

Until v2.3.2, `slice_all` sliced only the current plate despite its name: a four-plate project was
left with three unsliced plates and no error.

---

### export_gcode
Export the selected plate's sliced G-code to a file. The file is written asynchronously, so a
successful call answers `status: "export_started"`, not `"success"`:

| `status` | Meaning |
|----------|---------|
| `export_started` | The app has begun writing the file in the background: not a failure. It is complete once `wait_for_slice` returns (`get_slicing_status`'s `busy` is false again, `busy_reason` was `exporting`); `next_steps` names `wait_for_slice` |
| `error` | Nothing was written; `message` says why (below) |

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `output_path` | string | Yes | Output path (a file dialog cannot open under MCP) |

`status: "export_started"` only when the app scheduled the export. An export that did not start is
`status: "error"` with the reason as `message`: the scene has no objects, "Another export job is
running." while the previous export is still writing (call it again once `busy` in
`get_slicing_status` is false), or "the plate failed validation: ..." with the app's words for the
plate being exported (the selected one, never another plate's) -- the app's export refuses such a
plate without a word, and nothing was ever written. The plate must be sliced: without a slice result
its G-code has not been checked ("... has not been checked: slice_all, then wait_for_slice, first").
A plate whose slice failed its G-code check is refused as the GUI's Export button is off, with the
problems in words ("The export did not start: plate_index 0 failed the app's check of its sliced
G-code, ...: a toolpath is above the printer's printable height. ..."); `get_slicing_status`'s
`plates[].gcode_check` lists them. An error dialog the app raised is added as `error_messages`.

---

### get_print_estimate
Get print time and material estimates for one plate. Requires a valid slice result for that plate
(`get_slicing_status` reporting `state: "done"`).

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `plate_index` | integer | No | Which plate to report, 0-based. Omitted = the currently selected plate. Reading another plate does not change the selection. |

An out-of-range `plate_index` is an error naming the valid range, never a silent fall back to the
selection. `plate_index` in the response is always the plate actually read, so it can be compared
against what was asked for.

**Returns:**
```json
{
  "status": "success",
  "state": "done",
  "plate_index": 0,
  "estimated_time": "38m 26s",
  "estimated_time_seconds": 2306.4,
  "estimated_time_silent": null,
  "printed_layers": 92,
  "object_layers": 92,
  "support_layers": 61,
  "layer_count": 92,
  "filament": {
    "total_length_mm": 4553.2,
    "total_volume_mm3": 10795.3,
    "total_weight_grams": 13.71,
    "total_cost": 0.34,
    "per_filament": [
      {"filament": 1, "volume_mm3": 10795.3, "length_mm": 4553.2, "weight_grams": 13.71, "cost": 0.34}
    ]
  },
  "filament_changes": 0,
  "extruder_changes": 128,
  "time_by_feature": {
    "outer_wall": 402.1, "inner_wall": 511.8, "overhang_wall": 12.4, "gap_fill": 20.3,
    "infill": 716.0, "support": 188.2, "support_interface": 41.7, "brim": 18.9, "skirt": 0.0,
    "prime_tower": 96.5, "travel": 204.6, "tool_changes": 71.3, "other": 22.4, "unattributed": 0.2
  },
  "active_warnings": {"count": 0, "warnings": []}
}
```

The numbers are read from the plate's own slice result, so they match the G-code's
`; estimated printing time (normal mode)` and `; total filament used [g]` comments. `filament`
entries are `null`, never `0`, when the slicer did not record the property they need (a filament
with no configured density has an unknown weight). `filament` is 1-based, as in every other
filament tool.

**Layers.** `printed_layers` is the G-code's own layer count (`; total layers count`, the
`total_layer_count` placeholder): the distinct heights the plate prints at, object and support
layers together, heights closer than 0.0001 mm counted once. `object_layers` and `support_layers`
count each kind the same way on its own, so with support synchronised to the object's layers
`printed_layers` equals `object_layers`, not their sum. When printing by object, each object's
heights count once per instance, as the G-code counts them. The three are `null` for a plate with
no sliced objects.

`layer_count` is deprecated in favour of `printed_layers`. It is still an integer, and it now counts
printed layers too (`0` when there are none to count). This is a correction: before v2.5.0.6 it was
the tallest object's layer count *plus* its support layer count, so a 70 mm part at 0.2 mm with
support read 649 layers instead of 350.

**Time by feature.** `time_by_feature` splits `estimated_time_seconds` (normal mode) into seconds
per feature, to a tenth: the same per-move times the preview legend sums by line type.

| Key | Time spent on |
|-----|---------------|
| `outer_wall`, `inner_wall`, `overhang_wall`, `gap_fill` | The walls; together, the layer plan's `perimeters` |
| `infill` | Sparse and internal solid infill, top and bottom surfaces, bridges, ironing |
| `support`, `support_interface` | Support (with its transition layers), and its interface |
| `brim`, `skirt`, `prime_tower` | Those extrusions |
| `travel` | Moves without extrusion |
| `tool_changes` | What the `filament_changes` / `extruder_changes` below cost: load, unload and tool-change time |
| `other` | Retracts, wipes, seams, pauses, custom G-code and custom extrusions |
| `unattributed` | The total less every move's time: what the processor adds to its total without a move (a trailing filament change), and rounding |

Every key is always present (`0` when unused), and the parts add up to the total. **To explain why
two slices differ**, read `time_by_feature` after each and subtract key by key: the keys whose
seconds changed account for the difference. A dwell (G4) or wait in custom G-code is counted with the
move after it, as the preview's legend counts it.

**Tool changes are two counters, not one.** They are the same pair the G-code preview's legend
shows, and they mean different things:

| Field | GUI label | Counts |
|-------|-----------|--------|
| `extruder_changes` | Tool changes | The printer switched to a different physical extruder / tool head |
| `filament_changes` | Filament change times | A nozzle was loaded with a *different* filament |

On a toolchanger whose heads each keep their own filament, `filament_changes` is legitimately `0`
while every tool change is counted in `extruder_changes`. On a single-nozzle AMS/MMU printer it is
the other way round. Before v2.3.2 this tool reported `filament_changes` under the name
`total_toolchanges`, which is why a four-head toolchanger interleaving ABS with a PETG support
interface was told it made no tool changes at all; the `total_toolchanges` key is gone.

**Other statuses:**

| Status | State | Meaning |
|--------|-------|---------|
| `in_progress` | `slicing` | The background slicer is still running |
| `error` | `idle` | The current plate has no valid slice result - run `slice_all` first |

---

## Visualization Tools

### render_plate_view
Picture a plate so an agent can read it: parts in distinct colours on a light background, the
plate outline, a 10 mm grid, the origin with X/Y, and each object's index painted on it. Every
view also returns the numbers that make the picture checkable without looking at it.

**Coordinates are bed millimetres** — the same frame `get_scene_info` reports positions and
`plates[].bounding_box` in. Plate N sits at `plates[N].bounding_box`; plate 2 of a 256 mm bed
starts at x ≈ 307. A camera aimed at another plate's area returns a flat image and says so.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `plate_index` | integer | Yes | Plate to render (0-based). Its objects are drawn, including any hanging over its edge. |
| `views` | array | No | Views to render. **Omit for a contact sheet** of `iso`, `top` and `front` fitted to the plate, composed side by side into one image. |
| `save_to_file` | boolean | No | Write a PNG to the system temp directory and return its path (recommended) instead of inline base64. |
| `resolution` | integer | No | Pixels per view side (default 512, 32–2048). Prefer `fit` to an object over more pixels. |
| `image_format` | `"png"` / `"jpeg"` | No | Default `png` for files, `jpeg` for inline base64. |
| `overlays` | boolean / object | No | `true` (default) draws all; `false` none; or `{outline, grid, origin, labels, excluded}` booleans. |
| `layer_view` | `"first_layer"` / object | No | A top-down plan instead of a 3D render: the **first-layer plan**, or **any sliced layer** as `{layer: N}` or `{z: mm}` — see below. Ignores `views`. |

**A view** is one of:
```json
{"preset": "iso" | "top" | "front" | "back" | "left" | "right" | "low", "fit": "plate" | {"object_index": 8}}
{"camera_position": [x, y, z], "target": [x, y, z], "frame": "bed_mm" | "plate_local"}
```
`fit` defaults to the plate (its footprint at the height of what is on it). Fitting an object
frames it and zooms to it: a closer camera, not more pixels. It frames the object's instances on
the requested plate; an object that plate does not hold is an error naming the plate(s) it is on.
Before v2.5.0.6 it framed instance 0 whichever plate that was on. `low` looks at the first layers from
the front, slightly downward — brims, support feet, bottom edges. `frame: "plate_local"` lets an
explicit camera be given relative to the plate's front-left corner.

**Examples:**
```json
{"name": "render_plate_view", "arguments": {"plate_index": 1, "save_to_file": true}}
{"name": "render_plate_view", "arguments": {"plate_index": 1, "save_to_file": true,
  "views": [{"preset": "low", "fit": {"object_index": 8}}, {"preset": "top"}]}}
{"name": "render_plate_view", "arguments": {"plate_index": 1, "save_to_file": true, "layer_view": "first_layer"}}
{"name": "render_plate_view", "arguments": {"plate_index": 0, "save_to_file": true,
  "layer_view": {"z": 10.0, "features": ["support", "support_interface"], "color_by": "filament"}}}
```

**Returns** an array with one entry per view (or one contact-sheet entry):
```json
[{
  "file_path": "/private/var/folders/xx/T/orcamcp_render_1790422180_48213_1_0.png",
  "frame": "bed_mm",
  "plate_origin": [307.2, 0.0],
  "objects_in_frame": [
    {"object_index": 8, "name": "Top Frame-SOLID-2", "screen_bbox": [206, 184, 437, 315], "clipped": false}
  ],
  "scene_current": true,
  "uniform_image": false,
  "overlays": {"outline": true, "grid": true, "origin": true, "labels": true, "excluded": true},
  "camera": {"preset": "low", "fit": {"object_index": 8}, "input_frame": "bed_mm",
             "camera_position": [...], "target": [...], "view_matrix": [...], "projection_matrix": [...],
             "viewport": [0, 0, 512, 512], "type": "perspective", "pixel_origin": "top_left"}
}]
```
- `objects_in_frame` lists every volume that projects into the view, with its bounding box in
  image pixels (top-left origin). `clipped` means the box extends past the frame or behind the
  camera.
- `uniform_image: true` means the picture is a single flat colour, and `hint` says why: the 3D
  scene has no model volumes at all, none of them are printable on this plate (with a pointer to
  `get_scene_info`), or they were drawn and the camera looked elsewhere (with the plate's extent to
  aim at). Check it before reading the image.
- Pictures are drawn from the 3D scene whichever tab the app is showing, and the tab is left as it
  is. `scene_current` says whether that scene was up to date: `false`, with a `warning` line, means
  the hidden 3D view could not be refreshed (possible on Linux, where a hidden canvas may refuse
  its GL context), so the picture may not show the latest changes. Before v2.5.0.6 they were drawn from the canvas on screen: after a slice switched the app to
  Preview, every render came back blank with "no printable volumes".
- A preset or `fit` frames the whole box it fits, height included, with a little room on every
  side, so a fitted object is never `clipped`. Before v2.5.0.6 the zoom ignored the height and was
  sized for the wrong view direction, so a tall object came back `clipped` with a full-frame box.
- Files go to the system temp directory (`$TMPDIR` on macOS, `%TEMP%` on Windows, usually `/tmp`
  on Linux), not a literal `/tmp`. When the app starts it removes render and preview images older
  than an hour; younger ones may belong to another OrcaMCP running at the same time.
- `camera` is what `pick_facet` needs to turn a pixel back into a ray; pass it unchanged. On a
  contact sheet each entry under `views` carries its own `camera`, `column` and `x_offset` to add
  to a pixel's x first.

**First-layer plan** (`layer_view: "first_layer"`): a top-down, orthographic plan drawn from the
plate's sliced first layer — each object's footprint in its colour, its brim loops as darker
lines, the support first layer hatched grey, the wipe tower in grey — plus the overlays. It shows
what prints at the plate's lowest height: an object on a raft is drawn as its raft (hatched like
support, outlined in the object's colour, `on_raft: true`), since its own first layer prints on
top of the raft, and support that starts higher up, standing on the part, is left out. On an
unsliced plate it falls back to model footprints with the configured brim width as a ring and
reports `source: "footprints"` instead of `"sliced"`. The entry adds `has_brim` and `on_raft` per
object, `support_present` (a raft counts), `wipe_tower_present` and `camera.mm_per_pixel`. This is the view for
"is the brim wide enough" and "where do the support feet land".

**Sliced layer plan** (`layer_view: {layer: N}` or `{z: mm}`): one layer of the plate's sliced
G-code, top-down, drawn from the same moves the Preview draws, each line at its real width. It is
the view for "does the interface cover this overhang", "which tool prints the support" and "what
changed on layer 400". An unsliced plate (or one whose slice is out of date) is an error saying to
call `slice_all` and `wait_for_slice` first.

| Key | Type | Description |
|-----|------|-------------|
| `layer` | integer | Layer number from 1, as the Preview's layer slider numbers layers: `1..printed_layers` of `get_print_estimate`. |
| `z` | number | A height in mm. The nearest printed layer is drawn; a height exactly between two goes to the lower. Give `layer` or `z`, not both. |
| `features` | array | Draw only these: `perimeters` (walls and gap fill), `infill` (sparse, solid, top, bottom, bridges, ironing), `support`, `support_interface`, `brim`, `skirt`, `prime_tower` — the same names `get_print_estimate`'s `time_by_feature` uses. Default all. Start G-code purge lines are never drawn. |
| `filaments` | array | Draw only these filament slots, 1-based as `get_scene_info`'s `filaments_used`. Default all. |
| `color_by` | `"feature"` / `"filament"` | `feature` (default): the Preview's feature colours. `filament`: each slot's colour, as the Preview's Filament view. |
| `fit` | `"plate"` / `{"object_index": n}` | Frame the plate (default), or that object with everything of its own on this layer: its support lines (tree feet included) and, on the first layer, its brim and raft. |

A line's filament is the one the G-code really prints it with, so support set to "any" filament,
flushing into infill and mixed filaments show as they print. The entry adds:

```json
{
  "layer_view": "layer", "source": "gcode",
  "layer": {"number": 51, "of": 60, "z": 10.25, "requested_z": 10.2, "also_at": []},
  "filaments": [1],
  "extruded_mm2": {"perimeters": 93.22, "infill": 858.42, "support": 0.0, "support_interface": 0.0,
                   "brim": 0.0, "skirt": 0.0, "prime_tower": 0.0},
  "extruded_mm2_by_filament": {"1": {"perimeters": 93.22, "infill": 858.42, "support": 0.0, "...": 0.0}},
  "object_mm2": 951.64, "support_mm2": 0.0,
  "objects_at_height": [{
    "object_index": 0, "name": "cap_on_stem.stl",
    "object_layer": {"number": 51, "print_z": 10.25, "height": 0.2},
    "support_layer": null,
    "overhang": {"area_mm2": 829.44, "tolerance_mm": 0.2,
                 "support_below": {"z": 9.85, "gap_mm": 0.2, "support_mm2": 742.31, "interface_mm2": 742.31,
                                   "searched_to_mm": null}}
  }],
  "drawn": {"features": ["support", "support_interface"], "filaments": "all", "color_by": "feature"},
  "legend": [],
  "nothing_drawn": true,
  "hint": "Nothing on layer 51 matches the features and filaments asked for. This layer prints perimeters, infill, with filaments 1.",
  "objects_in_frame": [{"object_index": 0, "name": "cap_on_stem.stl", "screen_bbox": [64, 64, 448, 448], "clipped": false}],
  "frame": "bed_mm", "plate_origin": [0.0, 0.0],
  "camera": {"type": "orthographic", "pixel_origin": "top_left", "mm_per_pixel": 0.078, "...": "..."}
}
```

That is `{"z": 10.2, "features": ["support", "support_interface"], "fit": {"object_index": 0}}` on a
30 mm cap over an 8 mm stem, sliced on a four-head toolchanger with tree supports in filament 2: the
cap's first layer has no support in it (the 0.2 mm gap is layer 50, the interface layer 49, at
`support_below.z`), and 742 of the 829 mm² that hang past the stem have interface lines under them.

- `layer.z` is the height the layer prints at. With `z`, the entry also has `requested_z`, and
  `also_at` lists other layer numbers at the same height (a by-object print reaches each height
  once per object).
- The areas are the whole layer's, whatever the filters draw: each line's length times its width,
  so where two lines overlap the area counts twice. `object_mm2` is perimeters plus infill,
  `support_mm2` support plus interface. `filaments` is every filament printing on the layer, in
  the order they start.
- `objects_at_height` says, per object, which of its own layers print at this height. Support can
  have heights of its own (`independent_support_layer_height`), so at a support-only height
  `object_layer` is `null`: nothing of the object prints there, and its layer spanning this height
  is one number up or down. Layer numbers here count from 1 within the object.
- `overhang` (on an object layer with one below it) is polygon arithmetic on the sliced layers, not
  pixels, and it reports facts, not a verdict.
  - `area_mm2` is the part of this layer more than `tolerance_mm` beyond the layer below.
    `tolerance_mm` is half the nozzle printing this object's outer walls on this layer, so it
    differs per object and per nozzle.
  - `support_below` is the support under this overhang. Support layers belong to the whole object,
    so the one right at the overhang's bottom may have been built for another overhang elsewhere.
    The search goes down from the overhang's bottom, at most 2 mm, and stops at the first support
    layer whose lines lie under this overhang (more than 0.01 mm² of it).
    - It gives that layer's height `z`, `gap_mm` from it up to the overhang, and how much of the
      overhang its support lines (`support_mm2`) and its interface lines alone (`interface_mm2`) lie
      under, measured with the lines' width.
    - `gap_mm` is what to read first. A gap near the configured top Z distance
      (`support_top_z_distance`; 0 for a zero-gap interface) means this is the contact layer holding
      the overhang up.
  - When no support layer within those 2 mm has lines under the overhang, `support_below` is the
    nearest support layer below, such as a raft 10 mm down. It has its `z` and `gap_mm`, both areas 0,
    and `searched_to_mm: 2`: nothing touches the overhang. `searched_to_mm` is `null` whenever
    something was found.
  - `support_below` is `null` when there is no support layer below at all.
  - The areas are ribbon areas: sparse support covers only the part its lines run under, while a
    dense interface should come close to `area_mm2`.
- `legend` has one entry per colour in the picture, with the area it drew; the image shows the same
  legend in its top-right corner unless `overlays.labels` is off. `nothing_drawn: true` means the
  filter matched no line on this layer, and `hint` says what the layer does print.

---

### get_preview_base64
Read an image file OrcaMCP wrote (`render_plate_view`, `include_preview`) back as a base64 data URI,
for remote or containerized clients. It renders nothing: call `render_plate_view` first.

**When to use:** Only use this tool if you do NOT have direct filesystem access to read the `preview_path`. Agents with local filesystem access (like Claude Code CLI) should use the Read tool instead.

**Where `preview_path` comes from.** Tools that take `include_preview` (the transforms, `load_model`,
`get_scene_info` and others) add a turntable preview of the selected plate as `preview_path`. If the
preview cannot be made, the tool still succeeds -- its change has been applied -- and says why in
`preview_error` instead. Before v2.5.0.6 a failed preview could make a transform report an error for
a change it had made, so a retry applied it twice.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | Yes | Path to the preview image file (from `preview_path` in other tool responses) |

**Example:**
```json
{"name": "get_preview_base64", "arguments": {
  "path": "/private/var/folders/xx/T/orcamcp_preview_1790422180_48213_2_turntable.jpg"
}}
```

**Returns:**
```json
{
  "status": "success",
  "preview_base64": "data:image/jpeg;base64,/9j/4AAQSkZJRg...",
  "source_path": "/private/var/folders/xx/T/orcamcp_preview_1790422180_48213_2_turntable.jpg"
}
```

**Security:** Only the images this server wrote can be converted: an `orcamcp_preview_*` or
`orcamcp_render_*` PNG or JPEG named the way it names them (`<prefix><time>_<pid>_<sequence>_<tag>`),
directly inside the system temp directory, checked after `..` and links are resolved. Every other path is rejected. Before v2.5.0.6 the check only looked for the
prefix anywhere in the path, so `<anywhere>/orcamcp_render_/../<file>` passed.

---

## Adaptive Layer Height Tools

### apply_adaptive_layer_height
Enable adaptive layer height for better surface quality.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_ids` | array | No | Objects to apply to (omit for all) |
| `include_preview` | boolean | No | Include preview |

Each object's result carries `estimated_layer_count`: the object layers the slicer will cut the new
profile into (support and raft not included). A profile with fewer than two points has none to
count: it reports `0` and says why in `estimated_layer_count_note`. Before v2.5.0.6 it was the
height divided by the mean of the thinnest and thickest layer, which is not the mean layer height
of a profile that is mostly one or the other.

---

### clear_adaptive_layer_height
Disable adaptive layer height.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_ids` | array | No | Objects to clear (omit for all) |
| `include_preview` | boolean | No | Include preview |

---

## Filament & Colour Tools

### set_object_filament
Assign a filament slot to a whole object, or to one volume of it. To colour only part of a surface,
paint it: `paint_object` with `mode: color`.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `filament` | integer | Yes | Filament slot, 1-based (physical or mixed) |
| `volume_id` | integer | No | Volume index (0-based, as `get_object_info`'s `volumes` lists them; parts and modifiers only). Omit or `-1` for the whole object |
| `include_modifiers` | boolean | No | Whole-object form only. `true` (default): modifiers lose their own slot too. `false`: keep them, as the GUI's object row does |

**What the whole-object form does, and why:** a volume's own slot beats the object's
(`ModelVolume::extruder_id`). Setting only the object's slot therefore changes nothing visible when
its parts or modifiers carry their own — the parts keep printing their old slot and the plate keeps
its prime tower. So the whole-object form also erases the own slot of every part and, unless
`include_modifiers: false`, of every modifier. A modifier pinned to another slot is how a two-colour
inlay is made; pass `false` to keep those and read `other_slots` for what they still force.

**Response:**
```json
{
  "status": "success",
  "object_id": 30, "filament": 3, "volume_id": null,
  "cleared_overrides": [
    {"volume_id": 1, "name": "Base Modifier",  "type": "modifier", "was_filament": 1},
    {"volume_id": 2, "name": "Wheel Modifier", "type": "modifier", "was_filament": 1}
  ],
  "effective_filaments": [3],
  "other_slots": [],
  "single_filament": true
}
```

Read the response, not `status`. `effective_filaments` is every slot the object still prints with
(volumes, painted facets, layer ranges); the object is on one filament only when it has one entry.
`cleared_overrides` names each volume that lost its own slot and what it held. The object list rows
are refreshed too; until v2.5.0.4 they kept showing the old number for modifiers after an MCP write,
which made a correct write look like a failed one.

Written after an agent set 45 objects to slot 3, read `extruder_id == 3` back on every one and
reported success, while every object still printed in slot 1 from modifiers no tool listed.

---

### suggest_color_mix
Suggest the closest achievable 2–3 component filament mix for a target colour, from the printer's
loaded physical filaments. Optionally create the mixed slot.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_color` | string | Yes | Target colour, exactly `#RRGGBB` |
| `material_type` | string | No | Restrict components to this filament type (e.g. `PLA`). Default: the type of filament slot 1. |
| `create` | boolean | No | Create the mixed slot. Default `false`. |

**Returns:**
```json
{
  "status": "success",
  "target_color": "#BA44ED",
  "recipe": {
    "components": [1, 2],
    "ratios": [50, 50],
    "predicted_color": "#BC7FF7",
    "measured": false
  },
  "delta_e": 8.4,
  "exact_match": false,
  "gamut": "inside",
  "gamut_delta_e_threshold": 20.0,
  "slot": null
}
```

**Exact matches are successes.** When `target_color` is already one of the loaded filaments, the
answer is "load that slot, no mix required" — a single-component recipe at ratio 100,
`delta_e: 0`, `exact_match: true`, `status: "success"` and an explanatory `message`. `status:
"error"` is reserved for calls that could not be answered. With `create: true` an exact match
reports the matching slot and `created: false`; there is nothing to create.

**How the mix actually works — read this before choosing filaments.** A mixed slot **alternates
layers** of its components, so the result is close to a weighted average of the component RGB
values, *not* subtractive pigment mixing. Cyan + magenta gives lavender, not blue; magenta + yellow
gives salmon, not red. A CMY filament set does not behave like printer inks.

**Gamut.** `gamut` is `"inside"` while `delta_e` is below `gamut_delta_e_threshold` (CIE76 ΔE 20)
and `"outside"` at or above it, with a `message` explaining why. The threshold is calibrated from
real results: usable mixes land at ΔE 3.6–14, while pure red from a cyan/magenta/yellow set lands
at ΔE 54. An `"outside"` recipe is the engine's closest attempt, not an answer — load a closer
filament instead of mixing.

### get_color_palette
Enumerate an achievable palette of filament mixes (pairs, and optionally triples) from the loaded
physical filaments — a shortlist to choose from before painting.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `max_count` | integer | No | Max entries. Default 12, cap 48. |
| `max_components` | integer | No | `2` for pairs only, `3` to include triples. Default 2. |
| `material_type` | string | No | Restrict to this filament type. |

**Returns:**
```json
{
  "status": "success",
  "palette": [
    {"components": [1, 2], "ratios": [70, 30], "predicted_color": "#5FC9E8", "measured": true}
  ],
  "unreachable_hues": [
    {"hue_degrees": 30, "name": "orange"},
    {"hue_degrees": 210, "name": "azure"}
  ],
  "gamut_delta_e_threshold": 20.0,
  "active_warnings": {"count": 0, "warnings": []}
}
```

`measured: true` means the colour came from the standard recipe table rather than the blend model.
Entries within ΔE 5 of a loaded filament, or of an already-accepted entry, are dropped, and the
list is ordered by hue.

`unreachable_hues` lists the 30-degree hue sectors that **no loaded filament and no enumerated mix
reaches** — nothing this set can put on the plate lands anywhere near them. It is computed over the
whole enumeration and over the loaded filaments themselves, so `max_count` never affects it and a
colour already in the machine is never listed. With cyan, magenta, yellow and a gray loaded and the
default `max_components: 2`, orange and azure come back here; with `max_components: 3` every sector
is reached and the list is empty.

A sector is a coarse instrument: a colour can be far out of reach while its sector is not empty.
Pure red from that set is ΔE 54 away, but the 0–30 sector still counts as reached because magenta
and yellow at 30/70 land on the orange-red `#F9A05A`. Use `suggest_color_mix`’s `gamut` and
`delta_e` for one specific target; use `unreachable_hues` for “what is this set missing entirely”.

---

## Painting Tools

All four tools take and report **plate millimetres** — the same coordinate *frame*
`get_object_info` reports its `bounding_box` and `position` in. They are never object-local.
But for the *numbers*, use `paint_object`'s or `get_object_paint`'s own `bounding_box` in
their responses, not `get_object_info`'s: that one spans every instance of the object, and
matches these tools' single-instance box only when the object has one instance. Band from the box
these tools report, not from `get_object_info`. (Before v2.5.0.6 `get_object_info`'s box was also
looser under rotation: the untransformed box's corners, transformed. It is now the exact box.) A facet belongs to the band or region containing its
**centroid**, so a triangle is painted whole or not at all.

Paint is stored on the *volume*, so it applies to every instance of an object. `instance_id`
(default `0`) only decides which instance's transform your coordinates are read through; it
does not restrict which instances are painted. **Brim ears are the one exception**: they are
object-level data, not per-volume, and slicing resolves them through instance 0 only — see
`set_brim_ears` and `get_object_paint` below.

Four fields decide whether you read these responses correctly. `original_facets`, reported
**per volume** by both `paint_object` and `get_object_paint`, is that volume's own mesh
triangle count; `original_facets_total`, reported at the top level by both, is the sum over
the volumes the call addressed. (They are two names because they are two numbers: on a
single-part object they agree, on a multi-part one they do not.) `facets_selected`, reported
by `paint_object` only, counts facets the selection *covered* — including ones covered but
set to state `0` (unpainted) — so none of those is "how much of the object is painted".
`coverage_percent`, reported per state by both tools, is the honest measure: it is
area-weighted, and a paint stroke can subdivide a triangle into several leaf triangles, so a
state's `facet_count` can exceed `original_facets` and the two must never be divided one by
the other.

### paint_object
Paint an object's surface as the GUI's paint tools do: filament colours for a multi-colour print,
support enforcers and blockers, the seam, or fuzzy skin -- per-triangle paint, the same data the paint
gizmos write. To put a whole object or volume on one filament, use `set_object_filament`. Painted
support enforcers do nothing while `enable_support` is off; `next_steps` then names the
`set_object_config` call that turns it on with a `(manual)` `support_type`, for support only where
painted.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `selection` | string | Yes | `bands`, `box`, `sphere`, `all`, `connected`, `component` or `state` |
| `mode` | string | No | `color` (default), `support`, `seam`, `fuzzy_skin` |
| `volume_id` | integer | No | Part index (0-based); omit or `-1` for every model part |
| `instance_id` | integer | No | Whose transform reads your coordinates (default 0) |
| `axis` | string | `bands` | `x`, `y` or `z` — the plate axis the bands run along |
| `filaments` | array | `bands` + `color` | One 1-based slot per band, split evenly. `0` = unpainted |
| `bands` | array | `bands` | Explicit `[{from, to, filament\|state}]` in plate mm |
| `from` / `to` | number | No | Even-split range; defaults to the painted volumes' own extent. Ignored when explicit `bands` are given |
| `box` | object | `box` | `{min: [x,y,z], max: [x,y,z]}` in plate mm |
| `sphere` | object | `sphere` | `{center: [x,y,z], radius: n}` in plate mm |
| `seed` | object | `connected` | `{point: [x,y,z]}` in plate mm (snapped to the nearest surface), or `{volume_id, facet}` from `pick_facet` |
| `angle` | number | No | `connected` only: stop the fill at edges sharper than this many degrees. Default 30 — the gizmo's smart-fill default |
| `component` | integer | `component` | A shell id from `get_object_components`. Needs `volume_id` when the object has several parts |
| `filament` | integer | `box`/`sphere`/`all`/`connected`/`component`/`state` + `color` | 1-based slot; `0` = unpainted |
| `match_filament` | integer | `state` + `color` | Repaint every facet now painted with this filament (`0` = the unpainted facets) |
| `match_state` | string | `state`, non-color | Repaint every facet now in this state: `none`, `enforcer`, `blocker` (`fuzzy_skin` for `enforcer` in that mode) |
| `state` | string | `box`/`sphere`/`all`/`connected`/`component`/`state`, non-color | `none`, `enforcer`, `blocker`; in `fuzzy_skin` mode, `fuzzy_skin` is also accepted as a synonym for `enforcer` (there is no `blocker`) |
| `replace` | boolean | No | `true` (default) discards this mode's existing paint first. `selection: state` always keeps the rest; `replace: true` is refused there |

**Notes:**
- An even split (`filaments`) is colour-only. The other three modes take explicit `bands`
  with a `state`, because alternating enforcer and blocker along an axis means nothing.
- `fuzzy_skin` has no `blocker`: `EnforcerBlockerType::FUZZY_SKIN` is an alias of `ENFORCER`.
- Bands are half-open `[from, to)`, except the one band reaching furthest along the axis,
  whose `to` is closed, so a centroid sitting exactly on the outer edge is still painted. A
  box or sphere region is inclusive on every face / at the surface (`<=`/`>=`, not `<`/`>`).
- Explicit `bands` need not tile the object and may overlap; the first match wins, and
  facets outside every band keep their previous state.
- `connected` is the GUI's smart fill: the region reachable from the seed without crossing
  an edge whose dihedral angle exceeds `angle`. It is how to paint a *feature* — a bag, a
  sleeve, a wheel — without knowing its coordinates. The response carries `seed` as
  resolved: `volume_id`, `facet`, `point`, `snap_distance_mm`, `angle`.
- A `seed: {point: [...], volume_id: N}` ignores `volume_id`: a point seed always resolves
  to the nearest surface across every part in scope, and the response's `seed.volume_id`
  reports which part actually won. Pass `{volume_id, facet}` (from `pick_facet`) instead if
  you need to name the part explicitly.
- `component` ids are per volume and deterministic for a given mesh (discovery order by
  lowest facet index).
- **On a multi-part object, `connected` seeds exactly one part — the one the seed point or
  facet resolved to.** With `replace: true` (the default), every *other* part in scope has
  its existing paint for this mode cleared, the same behaviour `box` and `sphere` selections
  have always had. Because a seed inherently touches one part, this is the common case for
  `connected`, not a corner case: pass `replace: false` to paint on top instead of resetting
  the rest of the object.
- If the scene changes while the fill is being computed (an object added, removed, or moved,
  or a mesh replaced), the call fails with a message that says the scene changed and asks you
  to retry — this is a retry signal, not a rejected request; the selection was never applied.
  With `replace: false` the same applies to the paint itself: the new facets are computed on
  top of the paint the volume carried when the call started, so if that paint is edited (in
  the gizmo, or by another call) while the fill runs, the call is refused rather than writing
  a result that would silently drop the edit. `replace: true` has no base to lose and is never
  refused for this reason — it discards this mode's paint by definition.
- On meshes of millions of facets the geometry takes seconds to minutes. It runs off the GUI
  thread, so other tools keep answering meanwhile, but the bridge's `ORCAMCP_TIMEOUT`
  (default 120 s) may need raising for the paint call itself.
- Painted supports need `enable_support: true`; painted fuzzy skin needs `fuzzy_skin` set to
  something other than `disabled_fuzzy` (the default). The response says so in
  `info_messages` when they are not.
- Filament slots above 32 cannot be painted — a facet state stops at
  `EnforcerBlockerType::ExtruderMax`. Use `set_object_filament` for those.
- `state` repaints by what is already painted, not by where: every facet now in
  `match_filament` (or `match_state`) takes `filament` (or `state`), and nothing else changes.
  It works on the leaf triangles a gizmo split, so a split facet keeps exactly its parts that were
  in the matched state. It is `remap_paint` with a one-entry mapping, and answers in
  `remap_paint`'s shape (`mapping`, `facets_before`, `facets_after`, per-volume `before` / `after`),
  plus `selection: "state"`.

**Example — find a feature by eye, then fill it:**
```json
render_plate_view {"plate_index": 0, "views": [{"camera_position": [300, -200, 150], "target": [155, 155, 30]}]}
→ {"images": [{"file_path": "...", "camera": {...}}]}
// read the image, pick a pixel on the feature you want painted
pick_facet    {"object_id": 0, "pixel": [212, 134], "camera": {...}}
→ {"volume_id": 0, "facet": 1180231, "point": [129.4, 141.9, 68.2], "normal": [...], ...}
paint_object  {"object_id": 0, "selection": "connected",
               "seed": {"point": [129.4, 141.9, 68.2]}, "filament": 16}
```
A seed given directly in plate mm works the same way without a render:
```json
pick_facet    {"object_id": 0, "point": [129.5, 144, 68]}
→ {"volume_id": 0, "facet": 1180231, "point": [129.4, 141.9, 68.2], ...}
paint_object  {"object_id": 0, "selection": "connected", "seed": {"point": [129.4, 141.9, 68.2]}, "filament": 16}
```

**Example — 14 even bands along Y across mixed slots 5-18:**
```json
{"object_id": 0, "selection": "bands", "axis": "y",
 "filaments": [5,6,7,8,9,10,11,12,13,14,15,16,17,18]}
```

**Example — everything painted with filament 1 becomes filament 3:**
```json
{"object_id": 0, "selection": "state", "match_filament": 1, "filament": 3}
```

**Example — support enforcers under a Z height:**
```json
{"object_id": 0, "mode": "support", "selection": "bands", "axis": "z",
 "bands": [{"from": 0, "to": 12, "state": "enforcer"}]}
```

**Response includes:** `mode`, `selection`, `coordinate_frame` (always `"plate"`),
`bounding_box` (the plate-frame box of exactly the volumes this call addressed, for
`instance_id` — the same field name and shape `get_object_paint` reports; band from this,
not from `get_object_info`'s), `instance_id`, `replace`, `annotation_changed`,
`original_facets_total` (summed over the volumes this call addressed), `facets_selected`
(facets the selection covered, including ones set to state `0` — not "how much is painted"),
`facets_unassigned`, `info_messages`, `active_warnings`, and `volumes` — one entry per volume
this call addressed, in the shape all three painting tools that report `volumes` agree on:
`volume_id`, `name`, `original_facets` (that volume's own triangle count), a plate-frame
`bounding_box`, and `modes` (an object keyed by mode name, each value the same
`{state, label, filament, facet_count, coverage_percent}` list `get_object_paint` reports).
Here `modes` carries only the one mode this call painted — painting `color` proves nothing
about `support`, `seam` or `fuzzy_skin`, so the other three are left out rather than
fabricated — but `modes.<mode>` reads the same way every other painting tool's does. For
`selection: bands` only: `axis`, `axis_range`, and per-band `from`, `to`, `state`, `label`,
`filament`, `facet_count`. `selection: connected` only: `seed` — the resolved
`{volume_id, facet, point, snap_distance_mm, angle}` the fill actually started from, so a
point seed's snap and the part it landed on are both visible without a second call.
`info_messages` also flags when a `bands` call left facets outside every band, the usual
symptom of banding from too wide a range.

---

### remap_paint
Renumber an object's painted filaments in one step. Every facet painted with an old filament
takes the new one, all at once: `{"1": 2, "2": 3, "3": 4}` moves 1 to 2, 2 to 3 and 3 to 4 —
never 1 on to 3. That frees slot 1, for a support filament say, on a model whose colours an
exporter numbered 1 to 3.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `mapping` | object | Yes | Old filament → new filament, e.g. `{"1": 2, "2": 3, "3": 4}` |
| `volume_id` | integer | No | Part index (0-based); omit or `-1` for every model part |
| `mode` | string | No | `color` only (the default). For support, seam or fuzzy-skin states use `paint_object`'s `selection: state` |

- Filaments the mapping does not list keep their facets; two mapped to one merge.
- `0` means unpainted: as a key it paints the bare facets, as a value it unpaints.
- A new filament must be an existing slot (`get_filaments`) and at most 32. An old one may be any
  state a facet holds, so a stale one left by an import can be moved off.
- One undo step for every part, even when several change. A call that changes nothing makes no
  undo entry (`annotation_changed: false`).
- Unpainted facets print with the part's own filament, which `remap_paint` leaves as it is. When
  the mapping moves that filament, `notes` names the call that moves them too.

**Example:**
```json
{"object_id": 0, "mapping": {"1": 2, "2": 3, "3": 4}}
```

**Returns:**
```json
{
  "status": "success",
  "object_id": 0,
  "object_name": "figurine",
  "mode": "color",
  "mapping": {"1": 2, "2": 3, "3": 4},
  "annotation_changed": true,
  "facets_before": {"0": 120, "1": 5400, "2": 3100, "3": 880},
  "facets_after": {"0": 120, "2": 5400, "3": 3100, "4": 880},
  "volumes": [{"volume_id": 0, "name": "figurine", "before": [...], "after": [...]}],
  "notes": ["volume 0: 120 unpainted facets still print with filament 1, the part's own; call set_object_filament {object_id: 0, filament: 2} to move them too"]
}
```
`before` and `after` are `get_object_paint`'s per-state list: `state`, `label`, `filament`,
`facet_count`, `coverage_percent`. Facet counts count leaf triangles, so a facet a gizmo split
counts once per piece.

---

### get_object_paint
Read back what is painted, plus the object's brim ears.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `volume_id` | integer | No | Part index (0-based); omit or `-1` for every part |
| `instance_id` | integer | No | Whose transform reports plate coordinates for paint (default 0). Brim ears always report through instance 0 regardless — see below |
| `mode` | string | No | Report one mode; omit for all four |

**Response includes:** the object's plate-frame `bounding_box` and `original_facets_total`,
then `volumes` — one entry per volume, in the shape all three painting tools that report
`volumes` agree on: `volume_id`, `name`, `original_facets` (that volume's own triangle
count), a plate-frame `bounding_box`, and `modes`, an object keyed by mode name (all four,
or just the requested one), each value a list of
`{state, label, filament, facet_count, coverage_percent}` (state `0`, unpainted, is
included). `coverage_percent` is area-weighted, not facet-count-weighted,
because a facet an earlier gizmo stroke subdivided would otherwise count the same as a whole
face. Brim ears are reported through instance 0 regardless of the requested `instance_id` —
`Brim.cpp` resolves stored points through instance 0 only, so any other frame would silently
misplace the ear — and the response says so explicitly with `brim_ears_instance_id: 0`.

---

### clear_object_paint
Reset an annotation, the equivalent of the gizmo's "Remove all".

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `volume_id` | integer | No | Part index (0-based); omit or `-1` for every part |
| `mode` | string | No | Which annotation; omit to clear all four |

There is no `instance_id`: paint lives on the volume and every instance shares it, so
clearing itself is instance-independent. The response's `bounding_box` still needs *some*
instance to read plate coordinates through, so — with no `instance_id` to pick one — it is
always instance 0's, the same convention `set_brim_ears` uses. What this tool gives you over
`paint_object` is clearing **all four** annotations in one call, and not having to name a
selection. (Painting every facet with state `none` ends in the same stored data —
`TriangleSelector::serialize` stores a triangle only when it is split or not `NONE`, so an
all-`none` paint serialises to nothing at all.)

A call that clears nothing takes no undo snapshot and leaves the project's dirty state
alone, so an undo after it steps back past this call, not onto it.

**Response includes:** `volumes` — one entry per volume this call addressed, in the shape
all three painting tools that report `volumes` agree on: `volume_id`, `name`,
`original_facets`, a plate-frame `bounding_box` (instance 0's, see above), and `modes` (an
object keyed by mode name, for the mode(s) this call cleared, each value the same
`{state, label, filament, facet_count, coverage_percent}` list `get_object_paint` reports —
read *after* the clear, so it shows the result rather than requiring a second
`get_object_paint` call to confirm it); `cleared` (one entry per mode: `mode`,
`volumes_cleared`, `cleared_volume_ids` — a subset reference back into `volumes` by id, not
a second listing of the volumes themselves); `annotation_changed`; `info_messages` (only
when nothing was cleared); `active_warnings`.

---

### set_brim_ears
Place the small brim tabs at chosen points. Brim ears are **not** facet paint — they are
`BrimPoints` on the `ModelObject`, which is why they are a separate tool.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based). No `volume_id`: ears are object-level, and the object need not have a model part |
| `points` | array | Yes | `[{x, y, radius?}]` in plate mm. An empty array removes every ear, when `append` is left `false`. A point may also carry `z`, which is ignored, so the `brim_ears` a response lists can be sent back as they are |
| `radius` | number | No | Default ear radius for points without one (default 5.0 mm, range 0.1-100) |
| `append` | boolean | No | `false` (default) replaces the object's ears; `true` adds to them |
| `instance_id` | integer | No | Must be `0` (the default) — `set_brim_ears` rejects any other value |

Only `x` and `y` matter: an ear always sits on the underside of the object. Ears produce
brim only when `brim_type` is `painted`; the response says so in `info_messages` when it
is not.

No tool removes or moves a single ear. To edit them, take the `brim_ears` this tool or
`get_object_paint` returned, change the list, and send it back as `points` with `append` left
`false`; their `z` is accepted and ignored.

Brim ears are object-level data, not per-instance: `Brim.cpp` resolves stored ears through
instance 0 only when slicing, so writing through any other instance's frame would store a
point that prints somewhere else than this call's own response would suggest. Rather than
accept that with a caveat, `instance_id != 0` is rejected outright.

Positions are plate millimetres, the same frame `get_object_info` reports its `bounding_box`
in — but for the numbers, use *this* response's own `bounding_box` (or `get_object_paint`'s),
not `get_object_info`'s: that one spans every instance of the object, and matches this tool's
only when the object has one instance. This one is always instance 0's, the same instance brim
ears themselves resolve through.

**Response includes:** `object_id`, `coordinate_frame` (`"plate"`), `instance_id` (always
`0`), `bounding_box` (the object's model-part footprint through instance 0, same field name
and shape `paint_object` and `get_object_paint` use), `brim_ear_count`, `brim_ears`
(`{x, y, z, radius}` per ear, in plate mm), `info_messages`, `active_warnings`.

---

### get_object_components
Find loose parts, stray shells and mesh fragments: the connected shells of each **model part**'s mesh — the pieces `paint_object
{selection: "component"}` can paint individually. A generated or assembled model often has a
feature (a bag, a wheel) as its own shell.

Parts only. Modifiers, negative volumes and support blockers are not listed, so this is not a census
of the object's volumes and its `volume_id`s are not contiguous when the object has any of those.
`get_object_info`'s `volumes` lists every volume with its type and filament.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `volume_id` | integer | No | Part index (0-based); omit or `-1` for every part |
| `instance_id` | integer | No | Whose transform defines plate coordinates (default 0) |

**Response includes:** `coordinate_frame` (`"plate"`), `instance_id`, and `volumes` — one
entry per volume, each `{volume_id, name, original_facets, bounding_box, component_count,
components}`, where `bounding_box` is that volume's own plate-frame box and each component is
`{component, facet_count, area_mm2, bounding_box}`, most facets first. Component ids are stable
for a given mesh (discovery order by lowest facet index), so
`paint_object {selection: "component", component: <id>, volume_id}` reliably paints exactly
that shell. On a mesh of millions of facets this takes seconds; it runs off the GUI thread,
so other tools keep answering meanwhile.

---

### pick_facet
Turn a point, a ray, or a pixel of a render into the facet it lands on.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `volume_id` | integer | No | Restrict to one part; omit or `-1` to search every part |
| `instance_id` | integer | No | Whose transform defines plate coordinates (default 0) |
| `point` | array | one of three | `[x, y, z]` plate mm; the nearest surface point is picked |
| `ray` | object | one of three | `{origin: [x,y,z], direction: [x,y,z]}` plate mm; first hit along the ray |
| `pixel` | array | one of three | `[u, v]` in a render's pixels, `(0,0)` top-left; needs `camera` |
| `camera` | object | with `pixel` | The `camera` object a `render_plate_view` view returned, unchanged |
| `include_component` | boolean | No | Also report the shell id (default `false`; costs a pass over the mesh) |

Give exactly one of `point`, `ray`, or `pixel` (+ `camera`).

**Response includes:** `volume_id`, `volume_name`, `facet`, `point` (plate mm, on the
surface), `normal` (unit vector, plate frame), `distance_mm` (from the query point or ray
origin), `query` (which of `point`/`ray`/`pixel` was used), `coordinate_frame` (`"plate"`),
`instance_id`; with `include_component`: `component` (the shell id) and `component_count`.

The loop this closes: `render_plate_view` → read the image → `pick_facet {pixel, camera}` →
`paint_object {selection: "connected", seed: {point}}`.

---

## Printer Tools

### get_printers
List available printers.

**Parameters:** None

**Returns:**
```json
{
  "current_print_host": {"name": "C5P", "type": "printer_preset_host", "print_host": "10.0.0.100",
                         "host_type": "flashforge", "is_current": true, "last_status_age_s": 4},
  "local_printers": [...],
  "cloud_printers": [...],
  "physical_printers": [...],
  "total_count": 3
}
```
`local_printers[].is_online` is the device list's own flag, set to true when a device is added; it is
**not** a live check. For a Flashforge print host, `current_print_host.last_status_age_s` is how many
seconds ago the printer last answered a status read from anywhere in the app (the Device tab's poll
included), `null` when it has not answered since the app started. Use `get_printer_status` to read it
live.

---

### select_printer
Select a Bambu device by device ID, or a printer preset with a print host by name.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `dev_id` | string | One of the two | Bambu device ID from `get_printers` |
| `physical_printer` | string | One of the two | Printer preset with a print host, as listed by `get_printers` |

A switch of printer preset also returns `colors_source` and `filaments`, as `select_preset
{type: printer}` does.

---

### add_physical_printer
Create or update a physical printer preset with its print host settings.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | Yes | Preset name to save under, e.g. `"C5P"` |
| `host` | string | Yes | Printer IP or hostname |
| `host_type` | string | Yes | Print host type (`flashforge`, `octoprint`, …) |
| `serial_number` | string | No | Flashforge serial number; omit to keep the stored one |
| `api_key` | string | No | API key, or the Flashforge LAN check code; omit to keep the stored one |
| `obico_url` | string | No | Flashforge only: self-hosted Obico server base URL, e.g. `http://10.0.0.2:3334`. Give with `obico_token`; `""` for both clears the link |
| `obico_token` | string | No | Flashforge only: the printer's Obico auth token. Never returned by any tool |
| `printer_preset` | string | No | Printer preset to base it on (default: the edited one) |

---

### get_printer_status
Live status from the configured print host. Full detail for Flashforge hosts.

**Parameters:** None

**Returns (Flashforge):**
```json
{
  "status": "success",
  "host_type": "flashforge",
  "print_host": "10.0.0.10",
  "online": true,
  "obico": {"configured": true, "url": "http://10.0.0.2:3334"},
  "printer": {"state": "ready", "camera_stream_url": "http://10.0.0.10:8080/?action=stream", "...": "..."}
}
```
`obico.configured` says whether the preset names an Obico server; the token is never included.

**When the printer cannot be reached:** a connection that was never made is tried once more after
500 ms (not on the GUI thread, which must not sleep). If that fails too, the error names the host
and port and the next step, and `cached` carries the material station from the printer's last
answer, if it answered since the app started. `cached` appears only when the printer could not be
talked to at all (no connection, a timeout, an unresolvable name, a connection dropped before any
answer); a refusal such as a wrong check code, an HTTP error or an unreadable answer is returned as
that error alone:
```json
{
  "status": "error",
  "message": "Could not connect to the printer at 10.0.0.100:8898: the connection failed after 2 ms, before reaching the printer; tried 2 times. That usually means the printer is not on the network right now: ...",
  "cached": {"source": "cached", "age_s": 312, "material_station": {"present": true, "slots": [...]}}
}
```
A failure that happens at once, before reaching the printer, usually means the printer is off the
network. On macOS, a newly built or installed app can also be blocked by System Settings > Privacy &
Security > Local Network. Every failure is logged at warning level with the URL, curl code, HTTP
status and elapsed time (the first of a streak, every 100th, and the recovery), never the request
body.

---

### printer_control
Pause, resume or cancel the Flashforge printer's current job, turn its light on or off, or set its
target temperatures. It acts on real hardware.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | Yes | `pause`, `resume`, `cancel`, `light_on`, `light_off` or `set_temperature` |
| `bed`, `chamber` | number | No | `set_temperature` only: that heater's target, degrees C |
| `nozzles` | array | No | `set_temperature` only: `[{tool, temp}]`, tool 0-3. Tools not listed are left unchanged |

`set_temperature` needs something to set: with none of `bed`, `chamber` or a nozzle (or with an
empty `nozzles`) it is refused and nothing is sent to the printer. It used to send "no change" for
every heater and report success. A `nozzles` that is not a list is refused the same way.

### match_project_to_printer
Make the project's filament slots say what the Flashforge material station holds: for each loaded
slot, pick a filament preset of the reported material and set the slot's colour to the reported
colour. Empty slots are left alone.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `slots` | integer[] | No | 1-based material-station slots to match; omit for every loaded slot |
| `dry_run` | boolean | No | Report the plan without changing anything (default false) |
| `allow_cached` | boolean | No | Apply a plan made from the printer's last known status when it cannot be read live (default false) |

**Returns:** `{"status": "success"|"partial"|"not_applied", "dry_run": ..., "changed_count": N,
"slots": [...], "filaments": [...], "source": "live"|"cached"}`.

When the printer cannot be reached but answered earlier in this session, the plan is made from
that last status and the response says so: `"source": "cached"`, `age_s`, `live_error` and a `note`.
It changes the project only with `allow_cached: true`. Without it, a call that asked to apply comes
back with `"status": "not_applied"`, `"applied": false` and the plan as a dry run, and the note says
how to opt in. An error from the plan itself (no material station, an unknown slot) keeps its
`status: error` and message, labelled `source: cached`, with no plan note. With no earlier answer,
the call returns the live error. So does a printer that answered with a refusal (wrong check code,
HTTP error, unreadable answer): only an unreachable printer falls back to its last status.

---

### send_to_printer
Upload the selected plate's sliced G-code to the configured print host **and start printing it**:
`start_print` defaults to true, so a bare call begins a print on real hardware. Never call it to
look at a dialog or to test a refusal.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `direct` | boolean | No | `true` (default): upload with no dialog. `false`: open OrcaSlicer's send dialog (Bambu's `SelectMachineDialog`, or the print-host dialog) and leave the send to the user |
| `start_print` | boolean | No | Start printing once the upload finishes (default `true`). Direct sends only |
| `leveling_before_print`, `use_material_station`, `material_mappings` | | No | Flashforge hosts with local-API credentials only; see Material mapping below |
| `file_name` | string | No | Name to store the upload under. Direct sends only |
| `all_plates` | boolean | No | Dialog sends only (`direct: false`): send every plate |

Refused, as the GUI's Print and Send buttons are off, when a plate it would send failed the check
its slice ran on its G-code (`get_slicing_status`'s `plates[].gcode_check`): "The send did not
start: plate_index 0 failed the app's check of its sliced G-code, ...: a toolpath is above the
printer's printable height. ...". A direct send also needs the plate sliced ("Plate is not sliced;
run slice_all first"), and judges the check again just before it uploads.

---

### Material mapping (Flashforge material station)

`send_to_printer` and `print_printer_file` both return a `material_mappings` array saying which
material-station slot feeds which project tool. Pass `material_mappings` explicitly (a list of
`{tool_id, slot_id}`) to choose the slots yourself; leave it out and the slots are chosen
automatically.

Auto-mapping picks, among the loaded slots whose material family matches the project filament's, the
slot whose colour is closest to it (CIE76 / delta E, the same metric `suggest_color_mix` uses). Ties
break on the lower slot id, and a slot is never assigned to two tools. When neither colour can be
read the first free slot of the family is used, as before.

**Response:**
```json
{
  "status": "queued",
  "material_mappings": [
    {"tool_id": 0, "slot_id": 4, "color_delta_e": 0.0},
    {"tool_id": 1, "slot_id": 2, "color_delta_e": 18.7}
  ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `tool_id` | integer | 0-based project filament/tool |
| `slot_id` | integer | 1-based material-station slot the printer will feed it from |
| `color_delta_e` | number \| null | Perceptual distance between the project filament's colour and the slot's. `0` is an exact match, ~2.3 is a just-noticeable difference, and anything above ~10 is a visibly different colour — worth warning the user about before printing. `null` when either colour is missing or is not a `#RRGGBB` value. |

`color_delta_e` is reported for explicitly requested mappings too, so a hand-picked slot can be
checked the same way. A reported list can be passed back as `material_mappings` as it is, for
example to `print_printer_file` after an upload with `start_print: false`: `color_delta_e` is
accepted there and ignored.

---

## History Tools

### undo
Undo the last operation: each scene-changing tool takes an undo snapshot before it changes anything,
the per-object settings tools included (`set_object_config`, `reset_object_config`,
`set_object_layer_range`, `delete_object_layer_range`, `apply_adaptive_layer_height`,
`clear_adaptive_layer_height`, `rename_object`), under the names the GUI's own edits use. A call is
one undo step however many objects it changes, and a call that changes nothing takes no snapshot, so
it leaves the redo stack as it was. It rebuilds the plate list, so a slice in progress is cancelled first, and a
Slice All run with it: the response then carries `slice_cancelled: true` and an `info_messages` line
saying so; call `slice_all` again. If the undo fails, the error response says so too when it had
already stopped a slice. A snapshot load that fails partway (out of memory, a missing history entry)
may leave the project inconsistent: the error says "Undo/redo failed partway; the project may be
inconsistent. Save a copy (save_project with a new output_path) and restart OrcaMCP.", no preview is
made, and every later tool except `save_project`, `export_3mf`, `quit_app` and `get_server_info`
returns that same error until the app is restarted.

**Parameters:** None

---

### redo
Redo the last undone operation. A slice in progress is cancelled as for `undo`, and reported the same
way.

**Parameters:** None

---

## Active Warnings

Many tools return an `active_warnings` section in their response, providing visibility into OrcaSlicer's notification system. This helps AI agents understand when issues exist that need attention.

**Endpoints with active_warnings:**
- Scene: `get_scene_info`
- Slicing: `slice_all`, `get_slicing_status`, `get_print_estimate`
- Model ops: `load_model`, `arrange_objects`, `auto_orient`
- Transforms: `move_object`, `rotate_object`, `scale_object`, `transform_objects`, `mirror_object`, `clone_object`, `flatten_object`, `cut_object`, `delete_object`
- History: `undo`, `redo`

**Response format:**
```json
{
  "status": "success",
  "active_warnings": {
    "count": 1,
    "warnings": [
      {
        "level": "serious_warning",
        "message": "Conflicts of G-code paths have been found at layer 231, Z = 18.60mm. Please separate the conflicted objects farther (cute-rumistl.stl <-> cute-rumistl.stl).",
        "type": "GcodeOverlap"
      }
    ]
  }
}
```

**Warning levels:**
| Level | Description |
|-------|-------------|
| `warning` | General warning, non-critical |
| `serious_warning` | Important issue that may affect print quality |
| `error` | Critical error that prevents printing |

**Common warning types:**
| Type | Description |
|------|-------------|
| `GcodeOverlap` | Objects' toolpaths conflict - separate them |
| `NeedSupportOn` | Object may need supports enabled |
| `BedFilamentIncompatible` | Filament incompatible with bed type |
| `SlicingError` | Slicing failed |
| `SlicingSeriousWarning` | Serious slicing issue |
| `ValidateError` | Validation failed |
| `PlaterWarning` | General plater warning |
| `MeshErrors` | The object list shows its warning icon for an object: open edges or recorded repairs. Only `get_scene_info` (every flagged object) and `load_model` (the flagged objects it added) report it, because it stays until the mesh is repaired. `message` gives the list's reason and what an agent can do (`repair_mesh {object_id: N}` for open edges; slicing closes each layer's outline across gaps up to 2 mm); the entry also carries `object_id` and `object_name`. `get_mesh_health` has the numbers |

**Note:** The `count` field is always present (even when 0) to help agents confirm issues have been resolved.

---

## Next Steps

A response whose result implies a follow-up says which tool to call next, and why, in `next_steps`:

```json
"next_steps": [
  {"tool": "get_object_components", "arguments": {"object_id": 0},
   "why": "part \"bracket\" of object 0 (\"bracket\") has 2 shells: a loose part or stray fragment may be one of them"}
]
```

`tool` is always a real tool and `arguments`, left out when the tool needs none, is a call that tool
accepts as it stands. There is at most one step per tool: `arguments` names the first object it
applies to, and `why` names them all ("objects 0, 3 and 7 ... call it for each"). A response with
nothing to suggest has no `next_steps`.

| Response | Step | When |
|----------|------|------|
| `load_model` (the objects it added), `get_scene_info` (every object) | `get_mesh_health` | an object shows the object list's mesh warning icon (open edges, or repairs a 3MF recorded) |
| `get_mesh_health` | `repair_mesh` for that object | the object has open edges. Not for a closed mesh whose icon shows repairs recorded at load: a repair leaves it as it is |
| | `get_object_components` | a model part of the object is more than one shell: a loose part or a stray fragment, which leaves no warning icon when it is closed |
| `slice_all` | `wait_for_slice` | `slicing_started`, or `not_started` with `busy_slicing` (wait, then `slice_all` again) |
| | `get_slicing_status` | `busy_job`: an arrange or orient holds the app, which `wait_for_slice` does not wait for; `slice_all` again once `ui_job` is null |
| | `get_print_estimate` with the `plate_index` of a sliced plate (the selected one when it has a result) | `already_sliced` |
| `export_gcode` | `wait_for_slice` | `export_started`: the file is still being written |
| `render_plate_view`, on each view whose `uniform_image` is true (beside its `hint`) | `get_scene_info` | nothing printable on that plate was drawn |
| | `render_plate_view` with `{plate_index, save_to_file: true}` | the plate's objects were drawn but the camera looked elsewhere: no views gives a contact sheet fitted to the plate |
| `paint_object` with `mode: support` | `set_object_config` for that object: `enable_support` `"1"` and `support_type` `normal(manual)` (or `tree(manual)` when its type is a tree one), for support only where painted | the object has painted enforcers and `enable_support` is off for it, so they do nothing (`info_messages` says so too). Not for blockers alone or erased paint: turning support on is the opposite of what a blocker asks; and not with an `(auto)` type, which would also support every other overhang |

---

## Error Handling

A tool that ran and could not do what it was asked answers normally, with `"status": "error"` and a
`message` in its result. A call that never reached the tool is a JSON-RPC error instead.

**Arguments the tool's schema does not allow** are refused before the tool runs, with -32602. The
message names the tool, what is wrong, and every argument (or, inside a nested object, every key)
it takes, required ones first:

```json
{"jsonrpc": "2.0", "id": 7, "error": {"code": -32602,
 "message": "scale_object has no argument \"scale\". Its arguments: object_id, include_preview, preview_resolution, preview_views, uniform, x, y, z."}}
```

| The call | The message |
|----------|-------------|
| An argument the tool does not take | `scale_object has no argument "scale". Its arguments: ...` |
| A required argument left out | `get_object_info is missing its required argument "object_id". Its arguments: object_id.` |
| A misspelled required argument (both at once) | `scale_object has no argument "objectid" and is missing its required argument "object_id". Its arguments: ...` |
| A tool that takes no arguments | `get_slicing_status has no argument "plate". It takes no arguments.` |
| A key a nested object does not take | `set_object_config: settings[0] has no key "unit". Its keys: key, value.` |
| A nested object's required key left out | `apply_config: settings[0] is missing its required key "type". Its keys: type, key, value.` |
| `arguments` that is not an object | `get_server_info's arguments must be a JSON object of named arguments; got array.` |

Absent and `null` `arguments` both mean no arguments. Nested objects are checked where their schema
says `additionalProperties: false`: `set_object_config`'s items, `set_object_layer_range`'s
`settings` items, `transform_objects`' entries and their `position`/`rotation`/`scale`,
`apply_config`'s `settings` items, `set_brim_ears`' `points`, `printer_control`'s `nozzles`,
`send_to_printer`'s and `print_printer_file`'s `material_mappings`, `paint_object`'s `box` and
`sphere`, and `pick_facet`'s `ray`. What a tool returns in the shape one of these takes is accepted
back: `brim_ears` as `set_brim_ears`' `points` (their `z` is ignored), a send's reported
`material_mappings` (their `color_delta_e` is ignored), and an object's `position`,
`rotation_degrees` and `scale` as `transform_objects`' `position`, `rotation` and `scale`. Other
nested objects take any key: a render's `views` and a paint call's `bands` may be passed back with
the extra fields the response carried.
Types, ranges and enum values are not checked here; the tool reports those itself. The bridge's own
tools (`start_orca`, `wait_for_slice`) are held to their schemas the same way.

JSON-RPC error codes:
| Code | Meaning |
|------|---------|
| -32700 | The request is not valid JSON |
| -32600 | Not a JSON-RPC 2.0 request, or not a POST |
| -32601 | Unknown JSON-RPC method (not a tool: an unknown tool is -32602) |
| -32602 | Invalid params: an unknown or bridge-only tool, or arguments the tool's schema refuses (above) |
| -32603 | Internal error: the tool failed unexpectedly; the message names it |
| -32001 | OrcaMCP is still starting up; retry in a few seconds |
| -32002 | OrcaMCP is quitting, so the call was not run; `start_orca` starts it again |
| -32003 | Refused: the request came from a web page (`Origin`) or named another host (`Host`) |

---

## Bridge Tools

These tools are handled by the MCP bridge script (`orcamcp-bridge.py`), not the OrcaSlicer server. `start_orca` works even when OrcaSlicer is not running; `wait_for_slice` needs it running, but waits in the bridge so the app stays free to answer other calls.

### start_orca
Start the OrcaMCP application. Use this when OrcaMCP is not running.

**Parameters:** None

**Example:**
```json
{"name": "start_orca", "arguments": {}}
```

**Returns (success):**
```json
{
  "content": [{"type": "text", "text": "OrcaMCP started successfully. Ready for commands."}],
  "isError": false
}
```

**Returns (already running):**
```json
{
  "content": [{"type": "text", "text": "OrcaMCP is already running"}],
  "isError": false
}
```

**Behavior:**
- Checks if OrcaMCP is already running (returns immediately if so)
- Launches OrcaMCP in detached mode
- Waits up to 30 seconds for the MCP server to become available
- Returns success once the server responds to ping

**Platform-specific launch:**
| Platform | Method |
|----------|--------|
| macOS | Uses `open` command for .app bundles |
| Windows | Uses `subprocess.Popen` with detached flags |
| Linux | Uses `subprocess.Popen` with new session |

**Executable search paths:**

*macOS:*
- `/Applications/OrcaMCP.app`
- `~/Applications/OrcaMCP.app`

*Windows:*
- `%ProgramFiles%\OrcaMCP\orca-mcp.exe`
- `%ProgramFiles(x86)%\OrcaMCP\orca-mcp.exe`
- `%LOCALAPPDATA%\Programs\OrcaMCP\orca-mcp.exe`

*Override:* Set `ORCAMCP_APP_PATH` environment variable to specify a custom path.

### wait_for_slice
Wait until the running slice is over, instead of polling `get_slicing_status`. Call it right after
`slice_all`: the bridge polls `get_slicing_status` every 1.5 s and returns when nothing is slicing
any more, or at the timeout.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `timeout_s` | number | No | Longest wait in seconds, at least 1. Default and ceiling: the cap |

**The cap** is 15 s below `ORCAMCP_TIMEOUT` (105 s at the default 120 s), or a quarter below it
when that is less (30 s at 40 s, 7.5 s at 10 s). While it waits the bridge answers nothing else (its
stdio loop is single-threaded: no ping, no cancel), so the wait always stays under the longest time a
user has said one call may take. A longer `timeout_s` is cut to the cap and the response says
`timeout_capped: true`. Below an `ORCAMCP_TIMEOUT` of 2 s there is no room to wait, and the call is
refused. A slice that outlasts the cap needs another call. A poll gets at least 1 s to be answered, and
the last one starts 1 s before the deadline, so a slice that ends just before it is still seen and the
wait still ends by it.

**Example:**
```json
{"name": "wait_for_slice", "arguments": {"timeout_s": 60}}
```

**Returns:**
```json
{
  "status": "success",
  "outcome": "done",
  "timed_out": false,
  "waited_s": 12.4,
  "polls": 9,
  "timeout_s": 60,
  "timeout_cap_s": 105,
  "slicing_status": {"is_slicing": false, "state": "done", "plates": [{"plate_index": 0, "index": 0, "slice_result_valid": true, "percent": 100}], "slice_run": {"outcome": "done", "...": "..."}}
}
```

| `outcome` | Meaning |
|-----------|---------|
| `done` | Every plate the last `slice_all` asked for has a slice result, empty plates aside: those have nothing to slice and are skipped (`slice_run.skipped`). Without a `slice_all` this session: the selected plate has one |
| `ended_early` | Slice All stopped before its last plate; `message` is the app's reason |
| `incomplete` | The run is over and some of its plates still there have no result (a plate-list change during the run cancels Slice All), or none of them had anything to slice; `message` names them. A plate deleted after the run does not count |
| `not_slicing` | Nothing was slicing and the selected plate has no result: `slice_all` was never called, could not start, or its plates are gone (a new project) |
| `timed_out` | Still slicing at the timeout (`timed_out: true`); call it again |
| `app_gone` | The app quit or crashed during the wait: it answered that it is quitting, or refused every connection for a second or more after it had been there. The slice did not finish; `start_orca`, then `slice_all` again |

`slicing_status` is `get_slicing_status`'s last answer (`null` if the app answered no poll in
time). A poll the app is too busy to answer, whose connection closes under the reply, whose reply is
cut short (once the app has answered cleanly), or a refusal shorter than a second, does not end the
wait. When the last polls before the deadline were refused, one more poll a second after the first
refusal (inside the headroom below `ORCAMCP_TIMEOUT`) decides: still refused is `app_gone`, an answer
is judged as usual; if it cannot fit, `timed_out` says the app stopped answering, never that it is
still slicing. An app that is not running when the wait starts, or that fails the status call, ends
it with an error.

### Tool list freshness

The bridge answers the first `tools/list` from `scripts/orcamcp_tools.json` when OrcaSlicer is not
yet running, which is normal, since the MCP client starts first. The file is generated from the
app's tool registry, and a unit test fails whenever the two disagree, so a build and the file it
ships with always list the same names, descriptions and schemas. The bridge's own tools
(`start_orca`) come from the same file whether the app runs or not, so an agent sees the same list
before and after the app starts.

The server instructions an MCP client shows before any tool is loaded -- `get_server_info` for
every tool, one line per job naming its key tools, `next_steps`, and the calls that overwrite,
discard or start something -- come from the same file (`instructions`). The bridge always answers
`initialize` itself, so they are the same with the app running or not. Claude Code shows the first
2048 characters of them; unit tests keep them within that and under 1,600 (room for later tools), in
ASCII, naming only real tools.

A running app of a different build than the bridge's file can still list other tools. For that
case the bridge advertises `tools.listChanged` and sends `notifications/tools/list_changed` the
first time a live OrcaSlicer answers after the file's list was served. A client that honours it
re-fetches without a restart.

After adding or changing a tool, regenerate the file (no running app needed):

```bash
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests
ORCAMCP_UPDATE_TOOLS_GOLDEN=1 build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][tools]"
```
