# Painted modifiers for agents

Date: 2026-11-19. Requested by the user after the 2026-11-19 upstream sync ("write the spec doc. I will run it after
your deploy"). Status: approved direction (full agentic parity), not started. Prerequisite: the 2026-11-19 sync is
merged into `mcp` (it brings upstream's feature and OrcaMCP's basic compatibility, listed under "Already done").

## What upstream added

Upstream commit db9968b9ee ("Painted modifiers", #16213) adds a volume type, `ModelVolumeType::PAINTED_MODIFIER`. A
user brushes an area on a model part's surface; the area, to a chosen depth, becomes a modifier: its settings (walls,
infill, speeds, any region setting a mesh modifier takes) apply to the part behind the paint. It is a modifier whose
shape follows the surface exactly, where a box or cylinder modifier does not fit.

Facts the implementation relies on (read them again before building; line numbers drift):

- Data (`src/libslic3r/Model.hpp`, `ModelVolume`): `painted_modifier_facets` (a `FacetsAnnotation` over the **host's**
  mesh; painted facets carry `EnforcerBlockerType::ENFORCER`), `painted_modifier_depth` (mm from the painted surface;
  0 = across the whole part up to the nearest unpainted surface), `painted_modifier_host` (the host part's `ObjectID`).
  `is_painted_modifier()`, `is_region_modifier()` (= mesh modifier or painted modifier).
- The volume shares its host's mesh and transformation (`ModelObject::add_painted_modifier(host)`); `ModelObject::
  sync_painted_modifiers()` brings it back in line with its host (mesh, hull, transform, painting remapped) and runs
  before every slice (`Plater::priv::update_background_process`) and every 3MF save. `painted_modifier_host(volume)`
  finds the host, or null when the host was removed.
- Painted modifiers have **no GLVolume** in the 3D scene (`GLCanvas3D`, `Selection::scene_volumes_count`). Code that
  walks GLVolumes must look parts up by `composite_id.volume_id`, never by position (OrcaMCP's render code does).
- GUI entry points: the object and part menus' "Add Painted Modifier" (`MenuFactory::append_menu_item_add_painted_modifier`
  -> `ObjectList::add_painted_modifier()`, which picks the selected part or the object's first part, takes the "Add painted
  modifier" snapshot and calls `ObjectList::add_painted_modifier(obj_idx, host, paint)`), and the paint tool
  `GLGizmoPaintedModifier` (strokes; its first stroke on a part creates the modifier; a Depth field writes
  `painted_modifier_depth`; "Remove all" resets the facets). Its place in the object list sets its priority among
  modifiers, as a mesh modifier's does.
- Settings: a painted modifier takes the same region settings as a mesh modifier, through its `config`.

## Already done (in the 2026-11-19 sync)

- Mesh repair skips painted modifiers (upstream's rule, in `capture_cgal_repair`); `repair_mesh` refuses a painted
  modifier's `volume_id`, naming its host.
- `volume_type_name` reports `painted_modifier`; the filament model counts a painted modifier's filament as a mesh
  modifier's (`prints_filament`, `is_region_modifier`); `set_object_config` / `get_object_config` / `reset_object_config`
  take its `volume_id` (`volume_settings_refusal`); `set_object_filament` takes it.
- Moving, rotating, scaling or mirroring one, and `set_volume_type` on one, are refused, naming the host
  (`painted_modifier_refusal`, `OrcaMCPPartEdits.cpp`).

## Use cases (what an agent should be able to do from one sentence)

- "Make the screw bosses / mounting tabs stronger": find those faces, paint them, give the modifier more walls and
  solid infill; the rest of the part stays light.
- "Slow down / clean up the front face": paint the visible face, slow its outer wall or change its pattern there.
- Functional zones: a grip with other walls, a flexible strip with lighter infill, a delicate overhang printed slower.
- Show the result: `render_plate_view` tints the painted area; a `layer_view` shows the changed walls after a slice.

## Goals

1. An agent can add a painted modifier to a part, paint and erase its area with every selection form `paint_object`
   already takes, set its depth, give it settings, read it back, see it, and remove it -- everything the GUI does.
2. Each change is one undo step, named as the GUI names it; an open toolbar tool is closed first (`closed_toolbar_tool`),
   as every paint tool does.
3. Nothing new to learn: the existing tools gain a mode or a type; no new tool unless one is truly needed.

Non-goals: emulating brush strokes or the GUI's brush shapes beyond the selection forms `paint_object` has; ordering
modifiers (the object list's drag) unless it falls out of an existing tool; painted modifiers on negative volumes or
modifiers (upstream allows model parts only).

Anti-goals: a painted modifier that silently does nothing (paint on a host that is gone, depth on an empty area) must
be refused or warned in the answer, never reported as a success.

## Design

1. **`add_volume`** gains `type: "painted_modifier"`, with optional `host_volume_id` (a model part; default: the
   object's first model part, as the GUI's menu does) and optional `depth_mm` (>= 0, default 0). Runs
   `ObjectList::add_painted_modifier(obj_idx, host, nullptr)` under the GUI's "Add painted modifier" snapshot, then sets
   the depth inside the same step. `shape`, `size` and `file_path` are refused with this type (it has no shape of its
   own). Answer: the new `volume_id`, `host_volume_id`, `depth_mm`, `painted_facets: 0`, and `next_steps` to
   `paint_object` (mode `painted_modifier`, this `volume_id`).
2. **`paint_object`** gains `mode: "painted_modifier"`, which requires `volume_id` naming a painted modifier (refused
   otherwise, with the object's painted modifiers listed, or `add_volume` as the next step when it has none). Every
   selection form works as for the other modes, over the **host's** mesh in the host's frame, writing
   `painted_modifier_facets`. States: `painted` (and `enforcer`, the same) and `none`. `match_state` / `selection:
   state` work on that modifier's facets. Optional `depth_mm` sets the depth in the same undo step, with or without a
   selection (depth alone is a valid call). Refused when the host is gone (the GUI's "no_host"). Answer: as the other
   modes (`facets_before` / `facets_after`, coverage), plus `depth_mm`; `next_steps` to `set_object_config` with that
   `volume_id` when the modifier has no settings yet.
3. **`get_object_paint`** and **`clear_object_paint`** gain the same mode: per painted modifier its `volume_id`,
   `host_volume_id`, `depth_mm`, painted facets and `coverage_percent` of the host; clear takes `volume_id` (one
   modifier) or none (every painted modifier's area of the object), facets only, the volumes kept.
4. **`render_plate_view`**'s `paint` gains `painted_modifier`: the host drawn with each painted modifier's area tinted
   (a legend entry per modifier, `volume_id` and its settings' keys), from the facets, since the modifier has no
   GLVolume. `texture_preview` / `section` combinations follow what the other paint tints allow.
5. **`get_object_info`** volume rows: `type: "painted_modifier"`, `host_volume_id`, `depth_mm`, `painted_facets`,
   `coverage_percent`, and its settings' keys; `get_scene_info` needs nothing beyond the type name.
6. **`delete_object`** with a painted modifier's `volume_id` removes it as the object list does (one step). Deleting a
   host part: report the painted modifiers left without a host (`orphaned_painted_modifiers`), as the GUI keeps them.
7. Pure decisions (mode parsing, state labels, refusals, the answer's shape) live apart from the app, as the other paint
   modes' do (`OrcaMCPPaintModel`, `OrcaMCPPartEdits`), unit-tested without it.

## Tests

- Unit (`tests/slic3rutils`, tags `[McpPaint]`, `[PartEdits]`): the mode and its states parse and read back; every
  refusal above, with its words; the answer's fields; an `add_volume` request with `shape` and this type refused.
- Model level (`tests/slic3rutils` or `tests/fff_print`): a cube with a painted modifier painted on one face and
  `wall_loops` overridden slices with more walls on that face only (upstream's `tests/fff_print/test_painted_modifier.cpp`
  shows how to build one); the tool path that writes the facets produces the same `painted_modifier_facets` the gizmo's
  `TriangleSelector` would for the same facets.
- Next steps: a builder case in `test_mcp_next_steps.cpp` for each new step.
- Golden tool file regenerated; `[orcamcp][tools]` green.

## Live check (a dev build on its own data folder, `ORCAMCP_APP_PATH`, never the user's app)

Add a cube, `add_volume {type: painted_modifier}`, `paint_object {mode: painted_modifier, box: one face}`,
`set_object_config` with `wall_loops: 6` on that `volume_id`, `render_plate_view {paint: painted_modifier}`, `slice_all`,
`wait_for_slice`, `render_plate_view {layer_view: {layer: 5}}`: the painted face shows six walls, the others the
default. Then `undo` four times returns the plain cube, and `export_3mf` / `load_project` keeps the modifier, its area,
depth and settings.

## Docs

CLAUDE.md's tool table rows (Models: `add_volume`; Painting: `paint_object`, `get_object_paint`, `clear_object_paint`;
Visualization: `render_plate_view`), `docs/tools/reference.md`, `docs/tools/workflows.md` (a "strengthen part of a
part" workflow), and each tool's registration text. The server instructions need no line (the A/B result: tools are
found by name).

## Effort

About one to one and a half days of agent work including tests, docs and the live check; `paint_object`'s mode is the
bulk, the rest is plumbing over existing machinery.
