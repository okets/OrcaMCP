# OrcaMCP Tools Reference

Reference for the MCP tools available in OrcaMCP. The authoritative tool count and the
command that regenerates it live in `CLAUDE.md`, so it is not repeated here.

The table below is every tool, grouped by the category each one declares in the registry; the
same grouping, with a one-line summary per tool, is what `get_server_info` returns. Several tools
it lists -- `get_filaments`, `set_mixed_filament`, `get_flush_volumes` among them -- have no
dedicated section below yet.

## Quick Reference Table

| Category | Tools |
|----------|-------|
| **Scene** | `get_scene_info`, `new_project`, `load_project`, `save_project`, `export_3mf` |
| **Models** | `load_model`, `auto_orient`, `arrange_objects`, `get_object_info`, `rename_object`, `set_object_printable` |
| **Transforms** | `move_object`, `rotate_object`, `scale_object`, `mirror_object`, `flatten_object`, `clone_object`, `cut_object`, `delete_object`, `transform_objects` |
| **Plates** | `add_plate`, `select_plate`, `delete_plate`, `set_prime_tower_position` |
| **Config** | `get_presets`, `get_edited_presets`, `select_preset`, `apply_config`, `clone_preset`, `save_preset`, `delete_preset`, `reset_preset`, `get_valid_config_keys` |
| **Per-Object** | `get_object_config`, `set_object_config`, `reset_object_config` |
| **Layer Ranges** | `get_object_layer_ranges`, `set_object_layer_range`, `delete_object_layer_range` |
| **Filaments & colour** | `get_filaments`, `set_object_filament`, `set_mixed_filament`, `delete_mixed_filament`, `set_filament_color`, `get_flush_volumes`, `set_flush_volumes`, `auto_calc_flush_volumes`, `get_toolchanger_config`, `suggest_color_mix`, `get_color_palette` |
| **Painting** | `paint_object`, `get_object_paint`, `clear_object_paint`, `set_brim_ears`, `get_object_components`, `pick_facet` |
| **Slicing** | `slice_all`, `get_slicing_status`, `export_gcode`, `get_print_estimate` |
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

**Returns:** `{"status": "success", "slot": 4, "color": "#FFFFFF", "previous_color": "#BEBEBE"}`.
Pair it with `select_preset` (`type: filament`, `slot`) to put a material on the spool, then
`paint_object` or `set_object_filament` to use it.

---

### quit_app
Quit OrcaMCP cleanly with no dialog. An agent has to be able to close the app; a signal skips the
shutdown path and AppleScript raises the "save changes?" prompt, which MCP dialog suppression does
not cover because the close did not come through MCP.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `discard_changes` | boolean | No | Default `true`: unsaved project changes are discarded. `false` refuses while the project is dirty, so call `save_project` first. |

**Returns:** `{"status": "quitting"}`; the app exits within a few seconds.

---

### get_scene_info
Get current project state including plates, objects, and positions.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `with_model_object_features` | boolean | No | Reserved: adds an empty `features` object to each model object. No mesh analysis is computed yet. |
| `include_preview` | boolean | No | Include turntable preview path |

**Example:**
```json
{"name": "get_scene_info", "arguments": {"with_model_object_features": false}}
```

**Returns:**
```json
{
  "bed": {"origin": "corner", "min_x": 0, "max_x": 310, "min_y": 0, "max_y": 310, "max_z": 350},
  "plates": [{
    "plate_index": 0,
    "is_current": true,
    "model_objects": [{
      "object_index": 0,
      "id": "abc123",
      "name": "benchy.stl",
      "position": [155, 155, 0],
      "rotation_degrees": [0, 0, 0],
      "scale": [1, 1, 1],
      "bounding_box": {"min": [...], "max": [...], "size": [...]},
      "brim": {"type": "auto_brim", "extent_mm": 0.0, "extent_upper_bound_mm": 18.0, "extent_is_exact": false},
      "printed_footprint": {"min_x": 145, "min_y": 145, "max_x": 165, "max_y": 165, "size_x": 20, "size_y": 20},
      "printed_footprint_includes_brim": false,
      "instance_count": 1
    }],
    "prime_tower": {
      "printed": true,
      "reason": "printed",
      "reason_detail": "A prime tower is printed on this plate and occupies the reported footprint.",
      "frame": "plate_mm",
      "stored_position": {"x": 165.0, "y": 250.0, "frame": "plate_local_mm"},
      "position": {"x": 165.0, "y": 250.0},
      "position_is": "front_left_corner_of_tower_body",
      "size": {"x": 60.0, "y": 42.5, "z": 31.2},
      "brim_width": 3.0,
      "body": {"min_x": 165, "min_y": 250, "max_x": 225, "max_y": 292.5, "size_x": 60, "size_y": 42.5},
      "footprint": {"min_x": 162, "min_y": 247, "max_x": 228, "max_y": 295.5, "size_x": 66, "size_y": 48.5},
      "footprint_includes_brim": true
    },
    "excluded_areas": [],
    "occupancy_frame": "plate_mm",
    "occupancy": [
      {"kind": "object", "name": "benchy.stl", "object_index": 0, "footprint": {...},
       "includes_brim": false, "footprint_is_exact": false, "height_mm": 48.0},
      {"kind": "prime_tower", "name": "Prime tower", "footprint": {...},
       "includes_brim": true, "footprint_is_exact": true, "height_mm": 31.2}
    ]
  }]
}
```

#### Which filament an object prints with

Each `model_objects` entry carries three filament fields. `extruder_id` is the object's own
setting. A volume's own slot beats it (`ModelVolume::extruder_id`), so `filaments_used` — every
slot the object's parts, modifiers, painted facets and layer ranges print with — is the field to
read, and `filament_override_count` says how many parts and modifiers carry their own slot. The two
agree only when that count is 0. `filaments_used` is the per-object half of the rule the plate
applies for `prime_tower`; an object whose modifiers are pinned to another slot keeps the plate
multi-filament however `extruder_id` reads.

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

---

### get_slicing_status
Check slicing progress, for the selected plate and for every plate.

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
    {"index": 0, "slice_result_valid": true},
    {"index": 1, "slice_result_valid": true}
  ],
  "plates_sliced": 2,
  "plates_total": 2,
  "active_warnings": {"count": 0, "warnings": []}
}
```

| Field | Meaning |
|-------|---------|
| `state` | `idle` (never sliced, or the result was invalidated by an edit), `slicing` (in progress), `done` (the current plate has a valid slice result) |
| `is_slicing` | Background process running right now. During a `slice_all` run over every plate it stays true from the first plate to the last |
| `status` | Legacy field, `slicing` or `idle` only - use `state` |
| `slice_result_valid` | The current plate's own slice-result flag, the same one the GUI's Print/Export buttons use |
| `plates` | Every plate's slice-result flag, so a multi-plate run can be followed plate by plate (and a plate that failed can be identified) |
| `plates_sliced` / `plates_total` | How many of the plates have a valid result |
| `restored_selected_plate` | Present only on the poll that ends a `slice_all` run over every plate: the plate that was selected when `slice_all` was called has been selected again |

**Usage:** Poll every 2-3 seconds after `slice_all` until `state` is `done`, then call
`get_print_estimate`. `is_slicing: false` on its own does **not** mean the slice finished - it is
also false before slicing ever started. `state` is about the *selected* plate; for a multi-plate
run read `plates_sliced` / `plates`.

---

## Project Tools

### new_project
Create a new empty project.

**Parameters:** None

**Example:**
```json
{"name": "new_project", "arguments": {}}
```

---

### load_project
Load a project file (.3mf), replacing the current project.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | Yes | Path to .3mf file |
| `include_preview` | boolean | No | Return a turntable preview path |

**Example:**
```json
{"name": "load_project", "arguments": {"file_path": "/path/to/project.3mf"}}
```

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
Save the current project.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `output_path` | string | No | Path of the `.3mf` to save to. **Required while the project has no file name.** Also acts as Save As. |
| `save_as` | boolean | No | Legacy, ignored - use `output_path` |

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
Import a 3D model file, adding its objects to the scene.

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
| `loaded_objects` | One entry per object the load added, in the same shape as `get_scene_info`'s `model_objects`: `id`, `name`, `object_index` (the index other tools take as `object_id`), `instance_count`, `volume_count`, `position`, `rotation_degrees`, `scale` `{x,y,z}` of its first instance and `bounding_box` `{size_x, size_y, size_z, min, max}`. A merged multi-part file shows as one object with several volumes; a model scaled to fit the bed shows its scale. Empty for a G-code preview. |
| `filaments_added` | Filament slots the import added, because the model uses more filaments than the scene had (0 when none) |
| `project_renamed_to` | Present only if the project's name changed: never for a model file, and always for a G-code preview (named after the file, so a later `save_project {}` writes there) |
| `info_messages` | What happened, then what the slicer would have shown. A 3MF import says whether the file carried presets that were not applied. A prompt that offered a choice ends with the answer given, e.g. `"Object too large: ... scale it down to fit the print bed automatically? (auto-answered Yes)"`; the multi-part question also names the other `multipart` value |
| `active_warnings` | As for every scene tool |

A 20 mm cube exported 1000 times too large, on a 256 mm bed:

```json
{
  "status": "success",
  "file": "/tmp/cube_20m.stl",
  "loaded_objects": [
    {"id": "65", "name": "cube_20m.stl", "object_index": 0, "instance_count": 1, "volume_count": 1,
     "position": {"x": 128.0, "y": 128.0, "z": 127.0},
     "rotation_degrees": {"x": 0.0, "y": 0.0, "z": 0.0},
     "scale": {"x": 0.0127, "y": 0.0127, "z": 0.0127},
     "bounding_box": {"size_x": 254.0, "size_y": 254.0, "size_z": 254.0,
                      "min": {"x": 1.0, "y": 1.0, "z": 0.0}, "max": {"x": 255.0, "y": 255.0, "z": 254.0}}}
  ],
  "filaments_added": 0,
  "info_messages": ["Object too large: Your object appears to be too large, do you want to scale it down to fit the print bed automatically? (auto-answered Yes)"],
  "active_warnings": {"count": 0, "warnings": []}
}
```

---

### auto_orient
Automatically orient object for optimal printing.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | No | Object index (omit for all objects) |
| `include_preview` | boolean | No | Include preview after operation |

**Note:** This is an async operation. Poll `get_slicing_status` or check completion.

---

### arrange_objects
Automatically arrange objects on the plate.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `include_preview` | boolean | No | Include preview after operation |

**Note:** Async operation.

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

**Placement in the response.** Every transform re-homes the object onto the plate whose area now
contains it, then answers about *that* plate:

| Field | Meaning |
|-------|---------|
| `plate_index` | The plate the object is on after the transform, or `null` when it is on no plate |
| `on_bed` | Whether the object fits inside that plate's printable area |
| `placement_warning` | Present only when `on_bed` is false, and it names the plate |

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
| `relative` | boolean | No | Relative rotation (default: true) |
| `include_preview` | boolean | No | Include preview |

**Example:**
```json
{"name": "rotate_object", "arguments": {"object_id": 0, "z": 45}}
```

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

**Placement in the response.** Every transform re-homes the object onto the plate whose area now
contains it, then answers about *that* plate:

| Field | Meaning |
|-------|---------|
| `plate_index` | The plate the object is on after the transform, or `null` when it is on no plate |
| `on_bed` | Whether the object fits inside that plate's printable area |
| `placement_warning` | Present only when `on_bed` is false, and it names the plate |

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
| `uniform` | boolean | No | Apply X scale to all axes |
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

**Placement in the response.** Every transform re-homes the object onto the plate whose area now
contains it, then answers about *that* plate:

| Field | Meaning |
|-------|---------|
| `plate_index` | The plate the object is on after the transform, or `null` when it is on no plate |
| `on_bed` | Whether the object fits inside that plate's printable area |
| `placement_warning` | Present only when `on_bed` is false, and it names the plate |

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

**Placement in the response.** Every transform re-homes the object onto the plate whose area now
contains it, then answers about *that* plate:

| Field | Meaning |
|-------|---------|
| `plate_index` | The plate the object is on after the transform, or `null` when it is on no plate |
| `on_bed` | Whether the object fits inside that plate's printable area |
| `placement_warning` | Present only when `on_bed` is false, and it names the plate |

Before v2.3.2 `on_bed` was measured against whichever plate happened to be *selected*, so a correct
move into another plate's area was reported as "outside printable area"; `move_object` also left the
object registered on its old plate, which sliced it onto the wrong plate with no error.

---

### flatten_object
Flatten object to the bed (place flat side down).

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `include_preview` | boolean | No | Include preview |

---

## Object Operations

### clone_object
Create copies of an object.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `count` | integer | No | Number of clones (default: 1) |
| `duplicate` | boolean | No | Independent copies (true) vs linked instances (false) |
| `destination_plate` | integer | No | Target plate (default: current plate) |
| `include_preview` | boolean | No | Include preview |

**Example:**
```json
{"name": "clone_object", "arguments": {"object_id": 0, "count": 3, "duplicate": true}}
```

---

### cut_object
Cut an object at a specified Z height.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `z_height` | number | Yes | Cut height in plate mm, measured from the bed |
| `keep` | string | No | "below", "above", or "both" (default: "both") |

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
Rename an object.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `name` | string | Yes | New name |

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
`position.z`: an explicit Z is kept as given, as `move_object` keeps it. An entry whose scale factors are not
all positive is reported as an error against its own `object_id` and nothing in that entry is
applied; the rest of the batch still runs.

Each entry of `results` carries the same `plate_index` / `on_bed` / `placement_warning` fields the
single-object transforms return, measured against the plate that object landed on.

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
Remove a plate from the project.

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
Get currently active presets with dirty options.

**Parameters:** None

**Returns:** Current presets with `dirty_options` arrays showing modified settings.

---

### select_preset
Switch to a different preset.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type` | string | Yes | "printer", "filament", or "print" |
| `name` | string | Yes | Preset name |

---

### apply_config
Modify configuration settings.

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
List valid configuration keys for a category.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `category` | string | Yes | "per_object", "print", "filament", or "printer" |

---

## Per-Object Config Tools

### get_object_info
Get detailed information about an object.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |

**Placement fields:** alongside `position`, `bounding_box`, `rotation_degrees` and `scale`, the
response carries the same three placement fields the transform tools return:

| Field | Meaning |
|-------|---------|
| `plate_index` | The plate this object is on, or `null` if it is on none |
| `on_bed` | Whether the object fits inside **that** plate's printable area |
| `placement_warning` | Present only when `on_bed` is false, and it names the plate |

Before v2.3.2 `on_bed` here was measured against whichever plate happened to be *selected*, and
`plate_index` was not reported at all. Plates do not share a coordinate range, so an object sitting
correctly on plate 4 read as off the bed whenever another plate was selected.

`on_bed` still only means "inside the plate in XY, and not sunk below Z". It does not check for
collisions with other objects or the prime tower, and a part floating above the bed passes it.

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
Set per-object configuration overrides.

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
of both, per object.

---

### reset_object_config
Clear per-object configuration overrides.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `keys` | array | No | Specific keys to reset (omit for all) |

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
and `error` when none did.

---

### delete_object_layer_range
Remove a layer range configuration. Range Z is measured from the object's own base, not from the bed.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `z_min` | number | Yes | Range start to delete |

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
  "active_warnings": {"count": 0, "warnings": []}
}
```

**Note:** Async operation. Poll `get_slicing_status` until `state` is `done` (or until
`plates_sliced` equals `plates_total` for a multi-plate run).

**Plate selection.** Slicing every plate is driven by the slicer's own per-plate chaining, which
selects each plate in turn, so the selection moves while the run is in progress. The first
`get_slicing_status` poll after the run ends selects the plate that was current when `slice_all`
was called again and reports it as `restored_selected_plate`. This matters because
`export_gcode` and `get_preview_base64` all answer about the *selected* plate.
`get_print_estimate` takes an optional `plate_index` and answers about the selected plate only when
that is omitted.

Until v2.3.2, `slice_all` sliced only the current plate despite its name: a four-plate project was
left with three unsliced plates and no error.

---

### export_gcode
Export sliced G-code to file.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `output_path` | string | No | Output path (opens dialog if omitted) |

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
  "active_warnings": {"count": 0, "warnings": []}
}
```

The numbers are read from the plate's own slice result, so they match the G-code's
`; estimated printing time (normal mode)` and `; total filament used [g]` comments. `filament`
entries are `null`, never `0`, when the slicer did not record the property they need (a filament
with no configured density has an unknown weight). `filament` is 1-based, as in every other
filament tool.

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
| `plate_index` | integer | Yes | Plate to render (0-based). Only its volumes are drawn. |
| `views` | array | No | Views to render. **Omit for a contact sheet** of `iso`, `top` and `front` fitted to the plate, composed side by side into one image. |
| `save_to_file` | boolean | No | Write a PNG to `/tmp` and return its path (recommended) instead of inline base64. |
| `resolution` | integer | No | Pixels per view side (default 512, 32–2048). Prefer `fit` to an object over more pixels. |
| `image_format` | `"png"` / `"jpeg"` | No | Default `png` for files, `jpeg` for inline base64. |
| `overlays` | boolean / object | No | `true` (default) draws all; `false` none; or `{outline, grid, origin, labels, excluded}` booleans. |
| `layer_view` | `"first_layer"` | No | A top-down **first-layer plan** instead of a 3D render — see below. Ignores `views`. |

**A view** is one of:
```json
{"preset": "iso" | "top" | "front" | "back" | "left" | "right" | "low", "fit": "plate" | {"object_index": 8}}
{"camera_position": [x, y, z], "target": [x, y, z], "frame": "bed_mm" | "plate_local"}
```
`fit` defaults to the plate (its footprint at the height of what is on it). Fitting an object
frames it and zooms to it: a closer camera, not more pixels. `low` looks at the first layers from
the front, slightly downward — brims, support feet, bottom edges. `frame: "plate_local"` lets an
explicit camera be given relative to the plate's front-left corner.

**Examples:**
```json
{"name": "render_plate_view", "arguments": {"plate_index": 1, "save_to_file": true}}
{"name": "render_plate_view", "arguments": {"plate_index": 1, "save_to_file": true,
  "views": [{"preset": "low", "fit": {"object_index": 8}}, {"preset": "top"}]}}
{"name": "render_plate_view", "arguments": {"plate_index": 1, "save_to_file": true, "layer_view": "first_layer"}}
```

**Returns** an array with one entry per view (or one contact-sheet entry):
```json
[{
  "file_path": "/tmp/orcamcp_render_1789763152_1_0.png",
  "frame": "bed_mm",
  "plate_origin": [307.2, 0.0],
  "objects_in_frame": [
    {"object_index": 8, "name": "Top Frame-SOLID-2", "screen_bbox": [206, 184, 437, 315], "clipped": false}
  ],
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
- `uniform_image: true` means the picture is a single flat colour, and `hint` says why: which
  plate the camera should be aimed at, or that the plate has nothing printable. Check it before
  reading the image.
- `camera` is what `pick_facet` needs to turn a pixel back into a ray; pass it unchanged. On a
  contact sheet each entry under `views` carries its own `camera`, `column` and `x_offset` to add
  to a pixel's x first.

**First-layer plan** (`layer_view: "first_layer"`): a top-down, orthographic plan drawn from the
plate's sliced first layer — each object's footprint in its colour, its brim loops as darker
lines, the support first layer hatched grey, the wipe tower in grey — plus the overlays. On an
unsliced plate it falls back to model footprints with the configured brim width as a ring and
reports `source: "footprints"` instead of `"sliced"`. The entry adds `has_brim` per object,
`support_present`, `wipe_tower_present` and `camera.mm_per_pixel`. This is the view for
"is the brim wide enough" and "where do the support feet land".

---

### get_preview_base64
Convert a preview image file to base64 data URI for remote/containerized clients.

**When to use:** Only use this tool if you do NOT have direct filesystem access to read the `preview_path`. Agents with local filesystem access (like Claude Code CLI) should use the Read tool instead.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | Yes | Path to the preview image file (from `preview_path` in other tool responses) |

**Example:**
```json
{"name": "get_preview_base64", "arguments": {
  "path": "/tmp/orcamcp_preview_1234567890.jpg"
}}
```

**Returns:**
```json
{
  "status": "success",
  "preview_base64": "data:image/jpeg;base64,/9j/4AAQSkZJRg...",
  "source_path": "/tmp/orcamcp_preview_1234567890.jpg"
}
```

**Security:** Only `orcamcp_preview_*` and `orcamcp_render_*` files can be converted. Other paths are rejected.

---

## Adaptive Layer Height Tools

### apply_adaptive_layer_height
Enable adaptive layer height for better surface quality.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_ids` | array | No | Objects to apply to (omit for all) |
| `include_preview` | boolean | No | Include preview |

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
Assign a filament slot to a whole object, or to one volume of it.

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
their responses, not `get_object_info`'s: that one is a looser box (the untransformed AABB's
corners, transformed, then unioned over every instance) and matches these tools' snug,
single-instance box only for one unrotated instance — under rotation it is strictly larger,
and with more than one instance it spans all of them. Band from the box these tools report,
not from `get_object_info`. A facet belongs to the band or region containing its
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
Write per-triangle paint — the same data the GUI paint gizmos write.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index (0-based) |
| `selection` | string | Yes | `bands`, `box`, `sphere`, `all`, `connected` or `component` |
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
| `filament` | integer | `box`/`sphere`/`all`/`connected`/`component` + `color` | 1-based slot; `0` = unpainted |
| `state` | string | `box`/`sphere`/`all`/`connected`/`component`, non-color | `none`, `enforcer`, `blocker`; in `fuzzy_skin` mode, `fuzzy_skin` is also accepted as a synonym for `enforcer` (there is no `blocker`) |
| `replace` | boolean | No | `true` (default) discards this mode's existing paint first |

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
| `points` | array | Yes | `[{x, y, radius?}]` in plate mm. An empty array removes every ear, when `append` is left `false` |
| `radius` | number | No | Default ear radius for points without one (default 5.0 mm, range 0.1-100) |
| `append` | boolean | No | `false` (default) replaces the object's ears; `true` adds to them |
| `instance_id` | integer | No | Must be `0` (the default) — `set_brim_ears` rejects any other value |

Only `x` and `y` matter: an ear always sits on the underside of the object. Ears produce
brim only when `brim_type` is `painted`; the response says so in `info_messages` when it
is not.

Brim ears are object-level data, not per-instance: `Brim.cpp` resolves stored ears through
instance 0 only when slicing, so writing through any other instance's frame would store a
point that prints somewhere else than this call's own response would suggest. Rather than
accept that with a caveat, `instance_id != 0` is rejected outright.

Positions are plate millimetres, the same frame `get_object_info` reports its `bounding_box`
in — but for the numbers, use *this* response's own `bounding_box` (or `get_object_paint`'s),
not `get_object_info`'s: that one is a looser box (untransformed-AABB corners, unioned over
every instance) and only matches this tool's for a single unrotated instance. This one is
always instance 0's, the same instance brim ears themselves resolve through.

**Response includes:** `object_id`, `coordinate_frame` (`"plate"`), `instance_id` (always
`0`), `bounding_box` (the object's model-part footprint through instance 0, same field name
and shape `paint_object` and `get_object_paint` use), `brim_ear_count`, `brim_ears`
(`{x, y, z, radius}` per ear, in plate mm), `info_messages`, `active_warnings`.

---

### get_object_components
List the connected shells of each **model part**'s mesh — the pieces `paint_object
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
`{component, facet_count, area_mm2, bounding_box}`, largest first. Component ids are stable
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
  "current_print_host": "http://10.10.10.20",
  "local_printers": [...],
  "cloud_printers": [...],
  "physical_printers": [...],
  "total_count": 3
}
```

---

### select_printer
Select a printer by device ID.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `dev_id` | string | Yes | Device ID from `get_printers` |

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

---

### send_to_printer
Send sliced G-code to printer.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `all_plates` | boolean | No | Send all plates |

**Note:** Opens appropriate upload dialog (OctoPrint or Bambu).

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
checked the same way.

---

## History Tools

### undo
Undo the last operation.

**Parameters:** None

---

### redo
Redo the last undone operation.

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

**Note:** The `count` field is always present (even when 0) to help agents confirm issues have been resolved.

---

## Error Handling

All tools return errors in JSON-RPC format:
```json
{
  "error": {
    "code": -32602,
    "message": "Invalid params: object_id is required"
  }
}
```

Common error codes:
| Code | Meaning |
|------|---------|
| -32602 | Invalid parameters |
| -32603 | Internal error |
| -32601 | Unknown tool |

---

## Bridge Tools

These tools are handled by the MCP bridge script (`orcamcp-bridge.py`), not the OrcaSlicer server. They work even when OrcaSlicer is not running.

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

### Tool list freshness

The bridge answers the first `tools/list` from `scripts/orcamcp_tools.json` when OrcaSlicer is not
yet running, which is normal, since the MCP client starts first. The file is generated from the
app's tool registry, and a unit test fails whenever the two disagree, so a build and the file it
ships with always list the same names, descriptions and schemas. The bridge's own tools
(`start_orca`) come from the same file whether the app runs or not, so an agent sees the same list
before and after the app starts.

A running app of a different build than the bridge's file can still list other tools. For that
case the bridge advertises `tools.listChanged` and sends `notifications/tools/list_changed` the
first time a live OrcaSlicer answers after the file's list was served. A client that honours it
re-fetches without a restart.

After adding or changing a tool, regenerate the file (no running app needed):

```bash
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests
ORCAMCP_UPDATE_TOOLS_GOLDEN=1 build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][tools]"
```
