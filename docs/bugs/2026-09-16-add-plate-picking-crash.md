# Crash in the canvas picking pass after MCP `add_plate`, and floating objects after `delete_plate`

Observed 2026-09-16 on macOS 27.0 (arm64), OrcaMCP 2.5.0.1-dev, branch `sync-upstream-2.5`
at f5ff97bfa1, running the built app from `build/arm64`. Crash report:
`~/Library/Logs/DiagnosticReports/OrcaSlicer-2026-09-16-234607.ips`. App log:
`~/Library/Application Support/OrcaMCP/log/debug_Wed_Sep_16_21_55_05_91986.log.0`.

Three findings, in order of severity. The first is a use-after-free that kills the app.

---

## 1. SIGSEGV in `GLCanvas3D::_picking_pass` after MCP `add_plate`

### Symptom

`EXC_BAD_ACCESS / KERN_INVALID_ADDRESS at 0x18` on the main thread, `x0 == 0`, in

```
0  Slic3r::AABBMesh::AABBImpl::intersect_ray(...)            +56
1  Slic3r::AABBMesh::query_ray_hits(...)                     +124
2  Slic3r::GUI::MeshRaycaster::closest_hit(...)              +196
3  Slic3r::GUI::SceneRaycaster::hit(...)::$_0::operator()    +656   (the test_raycasters lambda)
4  Slic3r::GUI::SceneRaycaster::hit(...)                     +320
5  Slic3r::GUI::GLCanvas3D::_picking_pass()                  +440
6  Slic3r::GUI::GLCanvas3D::render(bool)                     +1736
7  Slic3r::GUI::GLCanvas3D::_refresh_if_shown_on_screen()    +144
8  Slic3r::GUI::GLCanvas3D::on_idle(wxIdleEvent&)            +860
```

`this` for `AABBImpl` is null: `AABBMesh::m_aabb` read as 0 through a `MeshRaycaster` that
has already been freed. The picking pass runs on every idle frame while the mouse is over the
3D canvas, so the crash lands whenever the user's pointer happens to be on the canvas after the
offending call, not necessarily at the call itself.

### Exact sequence that crashed

All calls through the MCP server (the Python bridge, tool by tool), against a project with four
plates and 17 objects, mouse over the canvas:

| Time | Call | Result |
|------|------|--------|
| 23:36:34 | `add_plate` (4 plates → 5; grid re-flowed from 2 to 3 columns) | ok |
| … | `select_plate 4`, `load_model`, `paint_object`, `render_plate_view`, `slice_all` on plate 4 | ok |
| 23:42:00 | `delete_plate 4` (5 → 4; grid re-flowed back to 2 columns) | ok, but see finding 2 |
| 23:45:0x | `delete_object 19`, `delete_object 18`, `delete_object 17` (the three objects left floating by finding 2) | ok |
| 23:45:33.06 | `get_object_info 17` | error "Invalid object_id: 17" (expected) |
| 23:45:33.16 | `add_plate` (4 → 5, re-flow to 3 columns) | log: three `calc_exclude_triangles: Unable to create exclude triangles` lines |
| 23:45:33.26 | idle frame, `_picking_pass` | **crash** |

The 100 ms gap between `add_plate` executing and the crash is one idle frame.

### Mechanism

1. `SceneRaycasterItem` stores a **raw pointer** to the `MeshRaycaster` it was registered with
   (`src/slic3r/GUI/SceneRaycaster.hpp`, `const MeshRaycaster* m_raycaster`). It does not own or
   share ownership of it.

2. Each `PartPlate` owns its picking meshes as `PickingModel { GLModel model;
   std::unique_ptr<MeshRaycaster> mesh_raycaster; }` and registers them with the canvas as
   `EType::Bed` raycasters via `register_model_for_picking` →
   `canvas.add_raycaster_for_picking(Bed, id, *model.mesh_raycaster, ...)`
   (`src/slic3r/GUI/PartPlate.cpp` ~1493).

3. `PartPlate::set_shape` rebuilds the plate geometry on every re-flow: `calc_triangles(poly)`
   and then `init_raycaster_from_model(m_triangles)` (`PartPlate.cpp` ~3400), which makes a
   **new** `MeshRaycaster` and frees the previous one. The `SceneRaycaster` still holds the raw
   pointer to the freed one.

4. In the GUI this is harmless because every plate operation goes through `Plater::update()` →
   `GLCanvas3D::reload_scene`, which does
   `m_scene_raycaster.remove_raycasters(SceneRaycaster::EType::Bed)` (`GLCanvas3D.cpp:2950`)
   and re-registers every plate with
   `partplate_list.register_raycasters_for_picking(*this)` (`GLCanvas3D.cpp:3061`). Compare the
   GUI's add-plate handler, `Plater::priv::on_action_add_plate` (`Plater.cpp:12655`):

   ```cpp
   take_snapshot("add partplate");
   this->partplate_list.create_plate();
   int new_plate = this->partplate_list.get_plate_count() - 1;
   this->partplate_list.select_plate(new_plate);
   update();
   q->get_camera().requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;
   ```

5. The MCP `add_plate` handler (`src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:3199`) calls
   `plate_list.create_plate(true)` and returns. **No `take_snapshot`, no `update()`, no
   `reload_scene`.** When the plate count crosses a column boundary (4 → 5 does: 2 → 3 columns),
   `create_plate(true)` calls `update_all_plates_pos_and_size` → `set_shapes` → every existing
   plate's `set_shape` → every plate's `m_triangles.mesh_raycaster` is replaced. The scene
   raycaster now holds dangling `Bed` entries for every plate. The next picking pass
   dereferences them.

Why the 23:36 `add_plate` did not crash: same dangling pointers, but the freed blocks had not
been reused yet, and the `load_model` that followed a few seconds later triggered
`reload_scene`, which discarded the stale entries. At 23:45 the picking pass ran on the very
next idle frame and the memory had already been reallocated (the new plate's own allocations),
so the read returned zeros.

### How to confirm

- Build with `-fsanitize=address`, open any project with 4 plates, call `add_plate` over MCP,
  then move the mouse over the canvas. ASan should report a heap-use-after-free with the
  allocation stack in `init_raycaster_from_model` and the free stack in the same function
  (unique_ptr reassignment) under `PartPlate::set_shape`.
- Without ASan: after the MCP `add_plate`, call `render_plate_view` (which does not run
  the picking pass) and confirm the app is fine, then hover the canvas and watch it die.

### Suggested fix

Make the MCP `add_plate` handler do what the GUI does after `create_plate`:

```cpp
plater->take_snapshot("add partplate");           // also fixes finding 3
int new_index = plate_list.create_plate(true);
plater->update();                                 // → reload_scene → Bed raycasters rebuilt
```

(`Plater::update()` is public; `Plater::priv::on_action_add_plate` is the reference.)

Then audit every other MCP handler that touches `PartPlateList` or plate geometry without going
through a `Plater` method that ends in `update()`:

```bash
grep -n 'get_partplate_list()' src/slic3r/GUI/OrcaMCP/*.cpp
```

`select_plate` and `set_prime_tower_position` are the ones to look at first. `delete_plate` is
fine on this axis because it calls `Plater::delete_plate`, which calls `update()`.

A defensive follow-up, separate from the MCP fix: `PartPlate::set_shape` could unregister the
plate's `Bed` raycasters before it replaces the meshes (there is precedent in
`PartPlate::invalidate_plate_name_texture`, which does exactly that for the name-edit icon).
That would make the crash impossible regardless of who calls `set_shape`. Upstream code, so weigh
it against the stay-close-to-upstream rule.

---

## 2. `delete_plate` leaves the plate's objects floating outside every plate

### Symptom

After `delete_plate 4`, the three objects that had been on plate 4 were still in the model, at
positions shifted by one plate width (x went from ~435 to ~742), on no plate. `get_scene_info`
does not list them (it walks plates), so an agent cannot see them. `get_object_info` on their
indexes reported `"on_bed": false, "plate_index": null, "placement_warning": "Object is not on
any plate"`. The user saw them in the GUI as parts floating beside the bed.

### Mechanism

`PartPlateList::delete_plate` (`PartPlate.cpp`) does not delete objects. It calls
`plate->move_instances_to(*(m_plate_list[m_plate_list.size()-1]), unprintable_plate)` and moves
them to the "unprintable" area, then `delete plate`. This is what the GUI's toolbar delete does
too, so the behaviour is inherited. The MCP tool description, however, says
**"Objects moved to another plate."**, which is not what happens: they are moved to the
unprintable area, off every plate.

### Suggested fix

- Correct the tool description and `docs/tools/reference.md`: objects on a deleted plate are
  moved to the unprintable area outside all plates and are no longer listed by `get_scene_info`.
- Add `"unplaced_objects": [...]` (index, id, name, position) to `get_scene_info` so floating
  objects are visible to agents. This is the part that turned a surprise into a hidden state.
- Consider an optional `delete_objects: true` on `delete_plate` that removes the plate's objects
  with it, since that is what an agent that just created a scratch plate almost always wants.

Minor: the handler validates `actual_plate_to_delete` but then passes the raw `plate_index`
(possibly `-1`) to `Plater::delete_plate`. Harmless today because `Plater::delete_plate` resolves
`-1` to the current plate itself, but the validated value is the one to pass.

---

## 3. MCP `add_plate` takes no undo snapshot

The GUI's add-plate takes `take_snapshot("add partplate")`; the MCP handler does not, so a plate
added over MCP cannot be undone with the `undo` tool. Fixed by the same change as finding 1.

---

## Reproduction script (MCP tool calls, any 4-plate project)

```
add_plate                 # 4 → 5 plates, grid 2 → 3 columns
# move the mouse over the 3D canvas → crash (finding 1)
```

For finding 2:

```
add_plate ; select_plate 4 ; load_model <any file> ; delete_plate 4
get_scene_info            # object is gone from every plate
get_object_info <n>       # ... but still exists, plate_index null
```
