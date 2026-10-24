# OrcaMCP Common Workflows

This guide shows how to combine MCP tools for common 3D printing tasks.

## Basic Print Workflow

**Goal:** Load a model, slice it, and export G-code.

```
1. load_model          Load STL/OBJ file
        ↓
2. arrange_objects     Auto-position on bed
        ↓
3. slice_all           Start slicing
        ↓
4. wait_for_slice      Returns once the slice is over
        ↓
5. export_gcode        Save G-code file (answers once it is written)
```

**Example sequence:**
```json
// Step 1: Load model
{"name": "load_model", "arguments": {"file_path": "/path/to/model.stl"}}

// Step 2: Arrange
{"name": "arrange_objects", "arguments": {}}

// Step 3: Slice
{"name": "slice_all", "arguments": {}}

// Step 4: Wait for the slice (it returns once the run is over)
{"name": "wait_for_slice", "arguments": {}}

// Step 5: Export
{"name": "export_gcode", "arguments": {"output_path": "/path/to/output.gcode"}}
```

---

## Visual Inspection Workflow

**Goal:** View the model before making changes.

```
1. render_plate_view   Capture images (with save_to_file=true)
        ↓
2. Read tool           Use Claude's Read tool to view the image files
```

**Camera positions for common views:**

| View | Camera Position | Target |
|------|-----------------|--------|
| Front-right isometric | `[300, -200, 150]` | `[155, 155, 30]` |
| Front view | `[155, -200, 50]` | `[155, 155, 30]` |
| Side view | `[10, 155, 80]` | `[155, 155, 30]` |
| Top view | `[155, 155, 300]` | `[155, 155, 0]` |

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

**Important:** Always use `save_to_file: true` to get file paths instead of large base64 strings.

---

## See What You Did Workflow

**Goal:** Check a change by looking at it, close up and from the right side, before and after.

```
1. render_plate_view  close-up: fit {point, radius_mm}, light {from: left, elevation_deg: 12}
2. render_plate_view  paint: ["support"] (or "seam", "fuzzy_skin", "color") to see what is painted
3. render_plate_view  section {plane: z, at_mm} to see inside; section_area_mm2 says how much
4. render_plate_view  texture_preview: true, before bake_texture_displacement
5. render_plate_view  save_to_file, change something, then a view with compare_with: that file
6. bake_texture_displacement, then render_plate_view color_by relief / triangle_size
```

```json
{"name": "render_plate_view", "arguments": {"plate_index": 0, "save_to_file": true,
  "light": {"from": "left", "elevation_deg": 12},
  "views": [{"preset": "iso", "fit": {"point": [128, 128, 15], "radius_mm": 6}}]}}
{"name": "render_plate_view", "arguments": {"plate_index": 0, "save_to_file": true, "texture_preview": true,
  "light": {"from": "left", "elevation_deg": 15}, "views": [{"preset": "iso", "fit": {"object_id": 0}}]}}
{"name": "render_plate_view", "arguments": {"plate_index": 0, "save_to_file": true,
  "views": [{"compare_with": "<file_path of the earlier render>"}]}}
{"name": "render_plate_view", "arguments": {"plate_index": 0, "save_to_file": true, "color_by": "relief",
  "light": {"from": "left", "elevation_deg": 15}, "views": [{"preset": "iso", "fit": {"object_id": 0}}]}}
```

**Notes:**
- A close-up is a small `fit`, not more pixels; `camera.mm_per_pixel` gives the scale. A returned `camera` can be sent back as a view to draw the same picture again (same plate only).
- `light.from` is relative to each view's camera, so `left` is the left of the image. A low elevation grazes the surface and shows relief.
- `texture_preview` changes nothing in the project; it is cached, so a second call is quick until the layers, paint or mesh change.
- `compare_with` draws with the earlier picture's camera and size: send the same overlays, light, section and paint in both. Only pictures this session saved are read.
- `color_by: relief` after a bake measures from the surface the bake displaced; `detail` and `triangle_size` need no reference and work on any mesh (a band of large flat triangles shows in `triangle_size` and its `slabs`).

---

## Settings Modification Workflow

**Goal:** Adjust print quality settings.

```
1. get_presets         See available presets
        ↓
2. select_preset       Switch preset (optional)
        ↓
3. apply_config        Modify specific settings
        ↓
4. get_edited_presets  Verify changes (see dirty_options)
```

**Common settings to adjust:**

| Setting | Type | Example Values |
|---------|------|----------------|
| `layer_height` | print | "0.1", "0.2", "0.3" |
| `wall_loops` | print | "2", "3", "4" |
| `sparse_infill_density` | print | "10%", "20%", "50%" |
| `enable_support` | print | "0", "1" |
| `nozzle_temperature` | filament (with `filament_slot`) | ["210"], ["220"] |
| `hot_plate_temp`, `textured_plate_temp` | filament (with `filament_slot`) | ["60"], ["70"] |

A filament setting changes one slot's preset: `filament_slot` names the slot, and it is required when
the project has more than one physical slot. Every slot using that preset gets the change, so one
another slot shares is refused unless `include_sharing_slots: true` (the refusal names how to give the
slot a preset of its own).

**Example:**
```json
{"name": "apply_config", "arguments": {
  "settings": [
    {"type": "print", "key": "layer_height", "value": "0.15"},
    {"type": "print", "key": "wall_loops", "value": "3"},
    {"type": "print", "key": "sparse_infill_density", "value": "20%"},
    {"type": "print", "key": "enable_support", "value": "1"}
  ]
}}

// Slot 2's filament
{"name": "apply_config", "arguments": {
  "filament_slot": 2,
  "settings": [{"type": "filament", "key": "nozzle_temperature", "value": ["230"]}]
}}
```

---

## Object Manipulation Workflow

**Goal:** Position, rotate, and scale objects.

```
1. get_scene_info      Get object IDs
        ↓
2. Transform tools     Apply transformations
        ↓
3. render_plate_view   Verify result
```

**Transform examples:**

```json
// Move 10mm to the right
{"name": "move_object", "arguments": {"object_id": 0, "x": 10}}

// Center on bed (absolute position)
{"name": "move_object", "arguments": {"object_id": 0, "x": 155, "y": 155, "relative": false}}

// Rotate 45 degrees around Z
{"name": "rotate_object", "arguments": {"object_id": 0, "z": 45}}

// Scale to 150%
{"name": "scale_object", "arguments": {"object_id": 0, "x": 1.5, "uniform": true}}

// Mirror along X axis
{"name": "mirror_object", "arguments": {"object_id": 0, "axis": "x"}}
```

---

## Per-Object Settings Workflow

**Goal:** Apply different settings to specific objects.

```
1. get_scene_info          Get object IDs
        ↓
2. set_object_config       Apply per-object settings
        ↓
3. get_object_config       Verify settings
```

**Example:** Make one object solid (100% infill) while others use default:

```json
// Get objects
{"name": "get_scene_info", "arguments": {}}

// Set 100% infill for object 0
{"name": "set_object_config", "arguments": {
  "object_id": 0,
  "settings": [
    {"key": "sparse_infill_density", "value": "100%"}
  ]
}}

// Verify
{"name": "get_object_config", "arguments": {"object_id": 0}}
```

---

## Layer Range Workflow

**Goal:** Different settings at different heights within one object.

**Use cases:**
- Fine detail layer at specific heights
- Variable infill density
- Different speeds for overhangs

```
1. get_scene_info              Get object info
        ↓
2. set_object_layer_range      Define height-specific settings
        ↓
3. get_object_layer_ranges     Verify ranges
```

**Example:** Fine layers (0.1mm) for detailed section between 10-20mm:

```json
{"name": "set_object_layer_range", "arguments": {
  "object_id": 0,
  "z_min": 10,
  "z_max": 20,
  "settings": [
    {"key": "layer_height", "value": "0.1"}
  ]
}}
```

---

## Cut Object Workflow

**Goal:** Split a model at a specific height.

```
1. get_scene_info      Get object info (bounding box shows height)
        ↓
2. cut_object          Cut at desired Z height
        ↓
3. arrange_objects     Re-arrange resulting parts
```

**Cut options:**
- `"keep": "below"` - Keep only bottom part
- `"keep": "above"` - Keep only top part
- `"keep": "both"` - Keep both parts as separate objects

**Example:**
```json
// Cut at 25mm, keep bottom half
{"name": "cut_object", "arguments": {
  "object_id": 0,
  "z_height": 25,
  "keep": "below"
}}
```

---

## Parts Workflow: Remove a Stray Fragment

**Goal:** Delete a small loose piece from a mesh and join the rest into one part again, as the object
list's Split, Delete and Mesh boolean do.

```
1. get_object_components   The part's shells: facets and box of each
        ↓
2. split_object            to: "parts" -- one part per shell (pieces: their volume ids)
        ↓
3. get_object_components   Each part's size: which one is the fragment
        ↓
4. delete_object           object_id and the fragment's volume_id
        ↓
5. merge_parts             The remaining parts unioned into one (optional)
```

Each step is one undo step. `split_object` `to: "objects"` makes one object per piece instead.

---

## Parts Workflow: A Modifier

**Goal:** Different settings inside one region of an object (denser infill in a boss, more walls in a
tab).

```
1. add_volume          type: "modifier", shape: "cube" -- stands beside the object
        ↓
2. move_object         with volume_id: the region's centre, relative: false
3. scale_object        with volume_id: the region's size
        ↓
4. set_object_config   with volume_id: the settings inside it (region settings only)
```

**Example:**
```json
{"name": "add_volume", "arguments": {"object_id": 0, "type": "modifier", "shape": "cube"}}
{"name": "move_object", "arguments": {"object_id": 0, "volume_id": 1, "x": 128, "y": 128, "z": 10, "relative": false}}
{"name": "set_object_config", "arguments": {"object_id": 0, "volume_id": 1,
  "settings": [{"key": "sparse_infill_density", "value": "80%"}]}}
```

`get_object_info` lists every volume with its box and position; `set_volume_type` turns a part into a
modifier, negative volume or support volume, and `assemble_objects` puts several objects into one as
its parts.

---

## Texture Workflow: Texture a Part

**Goal:** Put a surface texture on part of an object and print it. A texture layer is a plan until it is
baked into the mesh: nothing prints before step 6.

```
1. get_texture_library        Pick a texture by id (colour textures: "color": true)
        ↓
2. set_texture_displacement   action "add": the texture, depth, projection (one layer per call)
        ↓
3. paint_object               mode "texture", that texture_layer: where the layer applies
        ↓
4. set_texture_unwrap         Only for projection "unwrap": seams and islands (get_texture_unwrap first)
        ↓
5. get_texture_displacement   The layers, their paint, needs_bake (optional check)
        ↓
6. bake_texture_displacement  Turns the painted layers into geometry (one undo step)
        ↓
7. slice_all, wait_for_slice  The relief is in the G-code
        ↓
8. render_plate_view          layer_view shows the result
```

```json
// 1: find a texture (or import a file in step 2 with file_path)
{"name": "get_texture_library", "arguments": {}}

// 2: add a layer on object 0
{"name": "set_texture_displacement", "arguments": {"object_id": 0, "action": "add", "texture": "builtin/Knurl.png", "depth_mm": 0.4, "projection": "view", "view_direction": "front"}}

// 3: paint where it goes (a box on the grip; or selection "visible" paints what a view sees)
{"name": "paint_object", "arguments": {"object_id": 0, "mode": "texture", "texture_layer": 0, "selection": "box", "state": "painted", "box": {"min": [0, 0, 0], "max": [40, 30, 20]}}}

// 6: bake
{"name": "bake_texture_displacement", "arguments": {"object_id": 0}}

// 7: slice
{"name": "slice_all", "arguments": {}}
{"name": "wait_for_slice", "arguments": {}}
```

`next_steps` leads through it: a layer painted nowhere points at `paint_object`, painted-but-unbaked paint at
`bake_texture_displacement`, and a bake at `slice_all`. A coarse mesh gives a coarse relief: `prepare_texture_mesh`
(`remesh`, `subdivide`) before painting refines it, though the default bake refines by itself. A colour layer
(`color: true` in the library) is baked as colour paint and may add mixed filament slots (`get_filaments`). One
`undo` reverts the bake, but not slots it added. Editing the mesh afterwards (`repair_mesh`, `split_object`, ...)
drops texture paint; the answer's `texture_changes` says what was lost.

---

## Multi-Plate Workflow

**Goal:** Organize objects across multiple build plates.

```
1. get_scene_info      See current plates
        ↓
2. add_plate           Create new plate
        ↓
3. clone_object        Copy objects to new plate
        ↓
4. select_plate        Switch between plates
```

**Example:**
```json
// Add new plate
{"name": "add_plate", "arguments": {}}

// Clone object 0 to plate 1 (newly created)
{"name": "clone_object", "arguments": {
  "object_id": 0,
  "count": 1,
  "duplicate": true,
  "destination_plate": 1
}}

// Switch to plate 1
{"name": "select_plate", "arguments": {"plate_index": 1}}
```

---

## Send to Printer Workflow

**Goal:** Slice and send directly to printer.

```
1. slice_all              Start slicing
        ↓
2. wait_for_slice         Returns once the slice is over
        ↓
3. get_printers           Check available printers
        ↓
4. send_to_printer        Upload AND START the print (start_print: false only uploads)
```

**For OctoPrint/Klipper:**
```json
// Check printer is configured
{"name": "get_printers", "arguments": {}}
// Look for current_print_host field

// After slicing complete
{"name": "send_to_printer", "arguments": {}}
// Opens OctoPrint upload dialog
```

**For Bambu printers:**
```json
// List available printers
{"name": "get_printers", "arguments": {}}
// Look for local_printers array

// Select specific printer (if needed)
{"name": "select_printer", "arguments": {"dev_id": "00M00A2B0123456"}}

// Send
{"name": "send_to_printer", "arguments": {}}
```

---

## Recovery Workflow

**Goal:** Recover from mistakes.

```
1. undo                    Undo last operation
        ↓
2. undo (repeat)          Continue undoing if needed
        ↓
3. redo                    Redo if you went too far
```

**Safety tip:** Before major changes, save your project:
```json
{"name": "export_3mf", "arguments": {"output_path": "/tmp/backup.3mf"}}
```

---

## Tips for Efficient Workflows

### Minimize Token Usage
- Use `save_to_file: true` with `render_plate_view`
- Use `with_model_object_features: false` in `get_scene_info`
- Cache `get_presets` results (they rarely change)

### Batch Operations
- Use `transform_objects` for multiple transforms
- Use `apply_config` with multiple settings at once
- Use `set_object_config` with multiple keys

### Async Operations
`slice_all` returns at once while the slice goes on in the background: call `wait_for_slice`, which returns
once it is over. `export_gcode` answers once its file is written (a `.gcode` is written in the background, and
the call waits for it); past the bridge's cap it answers `export_started` with `finished: false`, and
`wait_for_slice` returns once the file is complete. `arrange_objects`, `auto_orient`, `flatten_object` and
`clone_object` answer once their job has been applied; past the bridge's cap they answer
`finished: false`, and `get_slicing_status`'s `ui_job` says when the job has ended.

### Object ID Management
- Object IDs can shift when objects are added/deleted
- Always re-query `get_scene_info` before transforms if you've made changes
- Pass an object's `object_id` (the same number as `object_index`); `internal_id` finds the same
  object again after the indices shifted, but no tool takes it
