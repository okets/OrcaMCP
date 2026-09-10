# OrcaMCP Tools Reference

Complete reference for all 50 MCP tools available in OrcaMCP.

## Quick Reference Table

| Category | Tools |
|----------|-------|
| **Bridge** | `start_orca` |
| **Information** | `get_server_info`, `get_scene_info`, `get_slicing_status` |
| **Project** | `new_project`, `load_project`, `save_project`, `export_3mf` |
| **Models** | `load_model`, `auto_orient`, `arrange_objects` |
| **Transforms** | `move_object`, `rotate_object`, `scale_object`, `mirror_object`, `flatten_object` |
| **Object Ops** | `clone_object`, `cut_object`, `delete_object`, `rename_object`, `transform_objects` |
| **Plates** | `add_plate`, `select_plate`, `delete_plate` |
| **Presets** | `get_presets`, `get_edited_presets`, `select_preset`, `apply_config`, `clone_preset`, `save_preset`, `delete_preset`, `reset_preset`, `get_valid_config_keys` |
| **Filament & Colour** | `get_filaments`, `set_mixed_filament`, `delete_mixed_filament`, `set_object_filament`, `get_flush_volumes`, `set_flush_volumes`, `auto_calc_flush_volumes`, `get_toolchanger_config`, `suggest_color_mix`, `get_color_palette` |
| **Per-Object** | `get_object_info`, `get_object_config`, `set_object_config`, `reset_object_config` |
| **Layer Ranges** | `get_object_layer_ranges`, `set_object_layer_range`, `delete_object_layer_range` |
| **Slicing** | `slice_all`, `export_gcode`, `get_print_estimate` |
| **Visualization** | `render_plate_view`, `get_preview_base64` |
| **Adaptive** | `apply_adaptive_layer_height`, `clear_adaptive_layer_height` |
| **Printers** | `get_printers`, `select_printer`, `send_to_printer` |
| **History** | `undo`, `redo` |

---

## Information Tools

### get_server_info
Get comprehensive documentation about the server, tools, and workflows.

**Parameters:** None

**Returns:** Complete documentation including quick start, workflow examples, and tool usage.

**Example:**
```json
{"name": "get_server_info", "arguments": {}}
```

---

### get_scene_info
Get current project state including plates, objects, and positions.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `with_model_object_features` | boolean | No | Include detailed mesh features (increases response size) |
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
      "instance_count": 1
    }]
  }]
}
```

---

### get_slicing_status
Check whether the current plate has been sliced.

**Parameters:** None

**Returns:**
```json
{
  "is_slicing": false,
  "state": "done",
  "status": "idle",
  "plate_index": 0,
  "slice_result_valid": true,
  "active_warnings": {"count": 0, "warnings": []}
}
```

| Field | Meaning |
|-------|---------|
| `state` | `idle` (never sliced, or the result was invalidated by an edit), `slicing` (in progress), `done` (the current plate has a valid slice result) |
| `is_slicing` | Background process running right now |
| `status` | Legacy field, `slicing` or `idle` only - use `state` |
| `slice_result_valid` | The current plate's own slice-result flag, the same one the GUI's Print/Export buttons use |

**Usage:** Poll every 2-3 seconds after `slice_all` until `state` is `done`, then call
`get_print_estimate`. `is_slicing: false` on its own does **not** mean the slice finished - it is
also false before slicing ever started.

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
Import a 3D model file.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | Yes | Path to STL, OBJ, 3MF, or STEP file |

**Example:**
```json
{"name": "load_model", "arguments": {"file_path": "/path/to/model.stl"}}
```

**Returns:** Object info including `object_index`, `name`, `bounding_box`

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
| `x` | number | No | X position/offset |
| `y` | number | No | Y position/offset |
| `z` | number | No | Z position/offset |
| `relative` | boolean | No | Relative move (default: true) |
| `include_preview` | boolean | No | Include preview |

**Examples:**
```json
// Relative move: shift 10mm in X
{"name": "move_object", "arguments": {"object_id": 0, "x": 10}}

// Absolute position: center of bed
{"name": "move_object", "arguments": {"object_id": 0, "x": 155, "y": 155, "relative": false}}
```

---

### rotate_object
Rotate an object.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `x` | number | No | X rotation (degrees) |
| `y` | number | No | Y rotation (degrees) |
| `z` | number | No | Z rotation (degrees) |
| `relative` | boolean | No | Relative rotation (default: true) |
| `include_preview` | boolean | No | Include preview |

**Example:**
```json
{"name": "rotate_object", "arguments": {"object_id": 0, "z": 45}}
```

---

### scale_object
Scale an object.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `x` | number | No | X scale factor |
| `y` | number | No | Y scale factor |
| `z` | number | No | Z scale factor |
| `uniform` | boolean | No | Apply X scale to all axes |
| `include_preview` | boolean | No | Include preview |

**Examples:**
```json
// Uniform scale: 150%
{"name": "scale_object", "arguments": {"object_id": 0, "x": 1.5, "uniform": true}}

// Non-uniform: double height only
{"name": "scale_object", "arguments": {"object_id": 0, "z": 2.0}}
```

---

### mirror_object
Mirror an object along an axis.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `axis` | string | Yes | "x", "y", or "z" |
| `include_preview` | boolean | No | Include preview |

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
| `z_height` | number | Yes | Cut height in mm |
| `keep` | string | No | "below", "above", or "both" (default: "both") |

**Example:**
```json
{"name": "cut_object", "arguments": {"object_id": 0, "z_height": 25, "keep": "below"}}
```

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

**Example:**
```json
{
  "name": "transform_objects",
  "arguments": {
    "transforms": [
      {"object_id": 0, "move": {"x": 10, "y": 0}},
      {"object_id": 1, "rotate": {"z": 90}},
      {"object_id": 2, "scale": {"x": 1.5, "uniform": true}}
    ]
  }
}
```

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
{"type": "print|filament|printer", "key": "setting_name", "value": "new_value"}
```

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
Get layer-specific settings for an object.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |

---

### set_object_layer_range
Set settings for a specific Z height range.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `z_min` | number | Yes | Range start (mm) |
| `z_max` | number | Yes | Range end (mm) |
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

---

### delete_object_layer_range
Remove a layer range configuration.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `object_id` | integer | Yes | Object index |
| `z_min` | number | Yes | Range start to delete |

---

## Slicing Tools

### slice_all
Start slicing the current plate.

**Parameters:** None

**Note:** Async operation. Poll `get_slicing_status` until `is_slicing` is false.

---

### export_gcode
Export sliced G-code to file.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `output_path` | string | No | Output path (opens dialog if omitted) |

---

### get_print_estimate
Get print time and material estimates for the current plate. Requires a valid slice result
(`get_slicing_status` reporting `state: "done"`).

**Parameters:** None

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
  "total_toolchanges": 0,
  "active_warnings": {"count": 0, "warnings": []}
}
```

The numbers are read from the plate's own slice result, so they match the G-code's
`; estimated printing time (normal mode)` and `; total filament used [g]` comments. `filament`
entries are `null`, never `0`, when the slicer did not record the property they need (a filament
with no configured density has an unknown weight). `filament` is 1-based, as in every other
filament tool.

**Other statuses:**

| Status | State | Meaning |
|--------|-------|---------|
| `in_progress` | `slicing` | The background slicer is still running |
| `error` | `idle` | The current plate has no valid slice result - run `slice_all` first |

---

## Visualization Tools

### render_plate_view
Capture plate images from specified camera angles.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `plate_index` | integer | No | Plate to render (default: current) |
| `save_to_file` | boolean | No | Save to temp file (RECOMMENDED: true) |
| `resolution` | integer | No | Image size in pixels (default: 512) |
| `views` | array | No | Array of camera configurations |

**View object format:**
```json
{"camera_position": [x, y, z], "target": [x, y, z]}
```

**Example:**
```json
{"name": "render_plate_view", "arguments": {
  "plate_index": 0,
  "save_to_file": true,
  "resolution": 512,
  "views": [
    {"camera_position": [300, -200, 150], "target": [155, 155, 30]},
    {"camera_position": [155, -200, 50], "target": [155, 155, 30]}
  ]
}}
```

**Returns:**
```json
{
  "images": [
    {"file_path": "/tmp/orcamcp_render_abc123.jpg"},
    {"file_path": "/tmp/orcamcp_render_def456.jpg"}
  ]
}
```

**Important:** Always use `save_to_file: true` to avoid large base64-encoded responses.

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
    {"hue_degrees": 0, "name": "red"},
    {"hue_degrees": 30, "name": "orange"}
  ],
  "gamut_delta_e_threshold": 20.0
}
```

`measured: true` means the colour came from the standard recipe table rather than the blend model.
Entries within ΔE 5 of a loaded filament, or of an already-accepted entry, are dropped, and the
list is ordered by hue.

`unreachable_hues` lists the 30-degree hue sectors no palette entry reaches — the colours this
filament set cannot mix at all. With cyan, magenta and yellow loaded, red and orange come back
here, because alternating layers averages the components rather than mixing them like pigment.

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

The bridge answers the first `tools/list` from a checked-in snapshot (`scripts/tools_schema.py`)
when OrcaSlicer is not yet running — which is normal, since the MCP client starts first. That
snapshot is regenerated by hand and can lag a build.

To stop a client being stranded on it, the bridge advertises `tools.listChanged` and sends
`notifications/tools/list_changed` the first time a live OrcaSlicer answers after a snapshot was
served. A client that honours it re-fetches and picks up the current schemas without a restart.

After adding or changing a tool's parameters, regenerate the snapshot against a running build:

```bash
python3 scripts/regen_tools_schema.py
python3 -m pytest scripts/tests/test_tools_schema.py -v   # compares snapshot to the live server
```
