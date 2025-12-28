# OrcaMCP Tools Reference

Complete reference for all 48 MCP tools available in OrcaMCP.

## Quick Reference Table

| Category | Tools |
|----------|-------|
| **Information** | `get_server_info`, `get_scene_info`, `get_slicing_status` |
| **Project** | `new_project`, `load_project`, `save_project`, `export_3mf` |
| **Models** | `load_model`, `auto_orient`, `arrange_objects` |
| **Transforms** | `move_object`, `rotate_object`, `scale_object`, `mirror_object`, `flatten_object` |
| **Object Ops** | `clone_object`, `cut_object`, `delete_object`, `rename_object`, `transform_objects` |
| **Plates** | `add_plate`, `select_plate`, `delete_plate` |
| **Presets** | `get_presets`, `get_edited_presets`, `select_preset`, `apply_config`, `clone_preset`, `save_preset`, `delete_preset`, `reset_preset`, `get_valid_config_keys` |
| **Per-Object** | `get_object_info`, `get_object_config`, `set_object_config`, `reset_object_config` |
| **Layer Ranges** | `get_object_layer_ranges`, `set_object_layer_range`, `delete_object_layer_range` |
| **Slicing** | `slice_all`, `export_gcode`, `get_print_estimate` |
| **Visualization** | `render_plate_view` |
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
Check if slicing is in progress.

**Parameters:** None

**Returns:**
```json
{"is_slicing": false}
```

**Usage:** Poll every 2-3 seconds after calling `slice_all`.

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
Load a project file (.3mf).

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | Yes | Path to .3mf file |

**Example:**
```json
{"name": "load_project", "arguments": {"file_path": "/path/to/project.3mf"}}
```

---

### save_project
Save current project to file.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | No | Output path (opens dialog if omitted) |

---

### export_3mf
Export project as 3MF file.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `output_path` | string | No | Output path (opens dialog if omitted) |
| `all_plates` | boolean | No | Export all plates (default: current only) |

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
List all available presets.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type` | string | No | "printer", "filament", "print", or "all" |

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
Get print time and material estimates (after slicing).

**Parameters:** None

**Returns:**
```json
{
  "print_time": "2h 30m",
  "print_time_seconds": 9000,
  "filament_used_g": 45.2,
  "filament_used_m": 15.3
}
```

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
