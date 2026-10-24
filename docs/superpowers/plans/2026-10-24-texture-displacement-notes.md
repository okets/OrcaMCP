# Notes for later texture tasks (from implementers and audits)

From T0 (d3f22b1aa0, review fix c5d0296320):
- palette_inputs takes the extruder colours; color_settings is the uncached composition for MCP (color_settings_for keeps the panel's cache).
- remove_texture_layer and swap_texture_layer_slots return bool (true when anything changed, a mask cleared included).
- texture_layer_move_swaps and an on_swap callback on move_texture_layer exist.
- resting_drops(const ModelObject&, parts) overload; smoothed_baked_mesh takes an optional TextureSmoothSkip*.
- queue_texture_displacement_bake takes a captured input; TexturePrepareScope lives in the prepare job header.
- TextureBakeOutcome::committed holds the volumes' new ids (the bake changes them); apply_texture_prepare returns TextureDisplacementPrepareApplied with each part's new id.
- Both plans carry a canceled flag; both apply functions refuse a stopped plan.
- texture_bake_applies_to re-runs the capture and compares part ids, drops, preview_adjust; ignores active_slot and panel_mode.
- An empty snapshot_name to capture_texture_prepare means no undo step.
- The apply_* functions need a Plater: their new-ids, stopped-plan refusal and mixed-slot count have no headless test. T15's live check must cover them.

From T1/T2 (593332f32b, f6e94b3d36, plus a review fix):
- parse_paint_mode does not parse "texture" yet: paint tools refuse it via mode_is_texture / TEXTURE_MODE_REFUSAL in OrcaMCPPaintTools.cpp. T3 and T10 replace those (parse "texture" in any case, remove both). Do not release between T2 and T10: that refusal names tools that do not exist yet.
- TextureState::Part has volume_id (position among the object's volumes).
- stack_options_json reports smooth.strength in percent (panel units).
- apply_layer_settings ignores view_direction, view_axes, projection_frame and adjust_image: T5's handler applies them.
- png_has_color returns nullopt for palette, grey+alpha and 16-bit PNGs (decoder rejects them).

For T5 (audit):
- `changed` must also count the handler's own view, frame and image-adjust changes.
- Refuse projection_frame together with a projection other than view, or with view_direction.
- The sent-back test (layer_json -> read_layer_request) must stay green.

For T7 (audit):
- unwrap_json islands carry centroid and group, which read_unwrap_request refuses; its uv_edits is a count where the request takes a list. Widen the request or rename the field. The plan's area_mm2, bounding_box, placement and neighbours are not there yet.

For T8 (audit):
- stack_options_json gives null for automatic resolution_mm / budget_k: accept null as auto in the request, or report 0 / -1.

For T11 (audit):
- Use needs_bake_fast (fixed to agree with needs_bake on stale masks).

After the T1/T2 review fix (2b1e908561):
- layer_edit_refusal takes `before` (null for add) and refuses colour / model_uv / color_mix_count only for a change. StackFacts has color_mix_count.
- layer_json takes the volume-to-world transform; reports view_axes {right, up} in plate axes; the frame flag is projection_frame_active (read_layer_request ignores it); the response's texture object is named `image` (the request's `texture` is a string). T6 uses `image`.
- A texture PaintChannel cannot be built without a valid slot (throws).
- needs_bake_fast agrees with needs_bake on stale masks.
- texture_state_of includes any volume holding a stack or masks (a first part turned modifier reads stacks_unused).
- Reuse identity includes has_color().
- stack_options_json has no reader yet (T8 adds the bake options reader: accept its output back, null = auto).

From T3:
- paint_object: parse_paint_channel (mode + texture_layer), texture_paint_refusal (job idle + layer exists), add_texture_answer (texture_layer, needs_bake via needs_bake_fast, texture_paint_next_steps), refresh_after_texture_paint, capture_visible_view, all in OrcaMCPPaintTools.cpp's anonymous namespace. PaintRemapRequest now holds a PaintChannel; run_paint_remap (so remap_paint and selection state) also closes an open toolbar tool and reports closed_toolbar_tool.
- assign_visible(targets, occluders, VisibleView, state) in OrcaMCPPaintSelect: takes all model parts as occluders and builds their AABBMesh once (plan deviates: spec had per-volume index signature).
- resolve_paint_modes / mode_is_texture / TEXTURE_MODE_REFUSAL still exist for get_object_paint and clear_object_paint: T10 replaces them (parse_paint_mode still does not parse "texture"; PaintChannel(PaintMode::Texture) throws, so never pass a Texture PaintMode where a PaintChannel is taken).
- TODOs left: T5 (name set_texture_displacement add in texture_paint_refusal's no-layer message), T8 (bake step in add_texture_answer / texture_paint_next_steps), T6/T8 (docs comment in paint_object Notes), TEXTURE_MODE_REFUSAL still names get_texture_displacement (T10).
- Named views for selection visible are orthographic along Camera::select_view's forward (iso too); only direction current keeps the camera's type and eye.

From the T3 audit (applies to EVERY changing tool, T5-T12):
- Close an open panel only after all refusals pass and only when something will change, right before the snapshot. Closing a painter takes its own "Leaving ..." undo step (GLGizmosManager.cpp ~1478). Error answers after a close carry closed_toolbar_tool.
- A projection frame counts only under ViewProjected AND view_project_projective. Frames use the camera's full projection (perspective too).
- "painted anywhere" uses needs_bake_fast's test (state 1 present, mask not stale), never !annotation.empty().
- Long pure work on the HTTP thread checks this_thread_cancelled() between parts/facet batches.

From T4/T5 (02108b3063, then the T5 commit):
- T4 added pure library_list_json, user_texture_delete_refusal; T5 added layer_position, move_insertion_index; read_layer_request now refuses projection_frame with view_direction/view_axes or a non-view projection.
- OrcaMCPTextureTools.cpp holds (anonymous namespace): LibraryCopy/library_copy, read_image, find_identical_texture (reuses ANY library source, cached identity by size+mtime), import_or_reuse, save_or_reuse, EditPlan/plan_edit (no side effects), write_edit, capture_layer (PendingLayer: copies for layer_json), view_axes_of, bounded_property/set_texture_displacement_schema. T6 can reuse capture_layer + the mask_reports-off-main-thread pattern in set_texture_displacement().
- Order in edit_stack: refusals -> library import/save (side effect, before any close) -> close_toolbar_tool_for_change -> take_snapshot -> write. No close for changed:false or any refusal (coordinator's correction).
- Answer: layer_json is under `layer` (not duplicated to top level inactive_settings/warnings, deviation from plan). remove answers removed_layer {layer,name,had_paint}.
- A projection frame defaults center to the object's instance-0 bounding box centre; frame is a planar rectangle (orthographic_frame_matrix), not the camera's perspective frame.
- read_layer_request errors still say "get_texture_displacement lists them" (tool arrives in T6).
- Edits with the same image (same_texture of raw decode) report image unchanged; adjust_image that leaves pixels equal is no change.
- Live check (T15) must cover: import reuse, adjust_image save, frame, move slot swap incl. active_slot, close-with-panel-open, wxLogNull on a bad image file.
- The unit suite cannot run edit_stack (needs Plater); only the pure parts are tested.

From T6/T7 (d1bec40792, then the T7 commit):
- T6: pure texture_stack_json(layers, reports, only_volume, ...) -> StackAnswer {json, first_unpainted_layer}; the tool adds object_id, stack_volume_id, parts, ignored_stacks and the next step. capture_stack/PendingStack/capture_parts in OrcaMCPTextureTools.cpp are reusable by T8 (bake needs the same stack copy). mask_has_paint(PaintData, facet_count) is the one "painted" test now (slot_painted_fast uses it).
- The earlier texts naming get_texture_displacement (read_layer_request, TEXTURE_MODE_REFUSAL, texture_paint_refusal) are left as they are: the tool exists now. Still TODO(T8): OrcaMCPNextSteps.hpp texture_paint_next_steps, add_texture_answer in OrcaMCPPaintTools.cpp, reference.md's paint_object note. The reference.md "bake" mentions use no tool name.
- T7: island_padding_mm is removed from the unwrap request, unwrap_json and layer_json: no code reads it (the panel and the bake always unwrap with padding 0), so it was a setting that did nothing.
- T7: compute_patch_unwrap drops the seam angle to 180 once ANY manual seam exists (library rule): with a seam, only seams cut. Documented in both tools; cutting a chart of a cube's 6 faces gives 2 charts, not 7.
- T7: unwrap_json is flat (offset/rotation_deg/scale beside area_mm2, centroid, bounding_box, group, neighbours); islands accepts all of those back; uv_edits is a [{vertex, uv}] list (vertex = unwrapped vertex, key -(v+1)); legacy mesh-vertex edits are mesh_vertex_uv_edits. The sent-back test is in test_mcp_tool_arguments.cpp and test_mcp_texture_displacement.cpp.
- T7: island_group_of / join_island_groups moved into TextureDisplacementObject.{hpp,cpp} (gizmo statics delegate); BX in CLAUDE.md lists them.
- T7 apply path (set_texture_unwrap): capture on main -> edit_unwrap on the HTTP thread (copies only, cancelled = this_thread_cancelled || tool_wait_cap) -> apply on main: re-capture, compare (same_inputs), close the tool only now; if closing changed the paint/mesh, re-plan once on the new input, then apply with no second close. Unit suite cannot run apply_unwrap_edit (needs Plater): T15 live check must cover set_texture_unwrap with a panel open, undo name, and the replan.
- T7: a call that changes seams/angle AND carries join/unjoin/islands/uv_edits is refused (checked after the seam steps, so reading a get_texture_unwrap answer back whole, with unchanged seams, is accepted).

From T8/T9 (bake_texture_displacement, prepare_texture_mesh; one file OrcaMCPTextureBakeTools.cpp, `register_texture_bake_tools`):
- Both tools: capture (main) -> plan (HTTP, stop = this_thread_cancelled || tool_wait_cap) -> finish (main). An open panel is closed only in round 0 and only when something will change; if closing changed what the plan read the tool plans ONCE more (second round never closes). The unit suite cannot run any of capture/finish (need Plater): T15 must cover them.
- Pure pieces in OrcaMCPTextureDisplacement.{hpp,cpp}: read_bake_request / bake_options (Standard pins: displace_border true + smooth off unless given) / bake_options_differ, read_prepare_request, remesh_/subdivide_prepare_params, default_remesh/subdivide_edge_mm, smooth_options, uniform_subdivision_refusal (cap 4M facets, added: uniform subdivide has no progress hook), message builders, bake_part_json etc. Bounds tables bake_setting_bounds / prepare_setting_bounds.
- stack_options_json is accepted back whole by read_bake_request (null/0 resolution_mm, null/-1 budget_k = auto; color_mix, color_mix_count, color_denoise taken as given and applied to the bake's options).
- Added to upstream-ish files: GLGizmoTextureDisplacement::standard_prepare_params(budget_k) (the STD_* recipe; panel's bake_standard and run_stage_debug call it), same_texture_layers (BakeJob, was a local), texture_prepare_applies_to (PrepareJob). CLAUDE.md BX probe unaffected.
- Deviations: smooth runs entirely on the main thread (smoothed_baked_mesh reads the live ModelVolume; linear work, as the panel); uniform subdivide runs subdivide_mesh_uniform on the HTTP thread and applies like the panel's subdivide_model (save/restore painting, texture paint dropped, reported); classic bake: bake input rebuilt after the planned prepare with resting_drops recomputed (geometry of unpainted parts is captured), apply = options store + apply_texture_prepare (no snapshot) + apply_texture_bake under ONE Plater::TakeSnapshot.
- texture_paint_next_steps now takes needs_bake (4 args); bake_next_steps and baked_texture_next_steps (slice_all, get_filaments after new mixed slots, bake again while paint is left) are the builders. paint_object, set_texture_displacement (all actions), get_texture_displacement use them.
- T15 live check must cover: default bake of a painted cube (counts, undo = one step, slice_all after); classic bake; a colour bake adding mixed slots (undo_warning, created flags) ; bake with the panel open (closed_toolbar_tool, replan when strokes were unflushed); bake past the wait (timed_out, nothing changed); bake of an unmoved part (depth too small: error, paint kept); multibody object (unpainted part, resting drop); smooth/uniform/remesh/adaptive prepare incl. PaintLost, Unchanged, panel open, undo names.

From T10/T11 (a484e1b263, then the T11 commit):
- T10: parse_paint_mode parses "texture" (any case); mode_is_texture / TEXTURE_MODE_REFUSAL / resolve_paint_modes are gone. New in OrcaMCPPaintTools.cpp: read_texture_slot, PaintSelection + resolve_paint_selection (get: mode texture without texture_layer = the owner's layer slots; clear: all eight slots), texture_layers_painting (pick_facet).
- get_object_paint: per-volume `texture: {layers: [{layer, painted_facets, coverage_percent, stale?}]}` (present with mode texture or no mode; `modes` is {} for texture alone); the masks are copied on main and reported off the main thread (mask_input_of, mask_reports, texture_paint_layers_json, layer_slots_of in OrcaMCPTextureDisplacement).
- clear_object_paint: mode texture + texture_layer (else all 8; layers stay). Refused while the UI worker runs; panel closed only if a texture mask will change (before the snapshot); cleared entries carry texture_layer; volumes[].texture read after the clear; closed_toolbar_tool in the answer; texture changes use refresh_after_texture_paint.
- pick_facet: `texture_layers` (slots with a non-empty get_triangle_as_string) read in a third main-thread hop that rechecks plan_meshes_unchanged.
- remap_paint: DEVIATION from the plan (which said colour-only): the coordinator listed it, so mode texture + texture_layer renumbers one mask (states 0/1; "from" above 1 refused); run_paint_remap already supported the channel. Description points layer reordering at set_texture_displacement move.
- T11: texture_object_fields (texture_layers, needs_bake via needs_bake_fast, model_uv) in model_object_summary_json (get_scene_info, load_model) and also get_object_info's top level; texture_volume_fields (model_uv, texture_layers owner only else 0, texture_paint_layers) in volume_rows_json, so every part tool's `volumes` rows carry them too. Reference example updated; [McpSceneDescription] green.
- Live check (T15) must cover: get_object_paint texture on a painted/stale layer; clear_object_paint texture with the panel open (closed_toolbar_tool, one undo step, none when empty); remap_paint texture {"1":0}; pick_facet texture_layers on a painted facet; get_scene_info needs_bake before/after a bake; load_model of an OBJ+MTL gives model_uv true. The app-side hops (clear/remap/pick) have no unit test (need Plater).

From T12:
- texture_state_of now counts "painted" with mask_has_paint (stale/empty masks are not paint); TextureState gained volume_count and Part::stack_owner (unused_before uses it, not parts.front()).
- New: rekey_texture_state (new ids, same positions: repair, reload, replace), texture_changes_messages, add_texture_changes (single object: `texture_changes` object), append_object_texture_changes (several objects: `texture_changes` list with object_id), texture_changes_next_steps, append_next_steps (add_next_steps REPLACES next_steps; append keeps them, one per tool).
- Added after guard.report (it replaces info_messages) in repair_mesh (both scopes), split_object, merge_parts, cut_object, set_volume_type, assemble_objects, delete_object volume_id, reload_from_disk (list), replace_volume_with_file. No tool text changed: no golden. Docs: one paragraph per tool in reference.md.
- ObjectList::merge (assemble) copies volumes with the same ids, so ids match; repair gives new ids (set_new_unique_id), reload/replace new volumes (matched by position when the volume count is unchanged).
- Live check: repair a painted object (single and plate_index), split to objects and parts, merge_parts, cut, set_volume_type first part -> modifier, assemble two textured objects, delete_object volume_id of the first part, reload_from_disk, replace_volume_with_file; confirm text and that no texture_changes appears when nothing was lost.

From T13/T13b (04e3ebb5ee, 27cdcc8af4 = upstream-ready code commit, 80f4709887, 5d5029489b):
- Fixture tests/data/textured_cube (cube.obj/.mtl/.png: 8 vertices, 6 quads = 12 triangles, a vt per corner). test_mcp_texture_files.cpp [McpTexture][3mf]: OBJ gives texture_uv with V flipped (24 vt, each corner one of them), new_layer picks ModelUV; same OBJ without its MTL gives no UV.
- Plan deviation: no shipped "Colour Bricks" in the test (resources path not available to tests); it uses the fixture's colour PNG as the layer image.
- T13b format: per-triangle attribute texture_uv="u1 v1 u2 v2 u3 v3" (%.9g) in bbs_3mf.cpp, NOT m:texture2dgroup (needs a texture2d resource; importer ignores pid/p1..p3). Reader: all triangles of a part must carry a valid one, else dropped; flip swaps corners 1,2; shared-mesh volume takes the source volume's channel; exporter shares a mesh only if texture_uv equal.
- Tests for upstream are in tests/libslic3r/test_3mf.cpp (7 cases, [3mf]). Cherry-picks cleanly onto upstream/main (49667537cf in a throwaway worktree, removed). T16 still has to build and run it there.
- T14 docs: load_model / load_project notes say a saved project keeps the UVs (CLAUDE.md item BZ already written; its probe greps upstream bbs_3mf.cpp for texture_uv).

T12 follow-up: TextureState parts carry the ModelVolume* (identity, never dereferenced), so repair/split-to-parts/re-sorts find parts by pointer or id; rekey_texture_state is for reload/replace only. New keys uv_lost, unwrap_edits_stale (the bake ignores out-of-range seam edges but does not detect edits made on an old mesh), paint_unused, paint_reassigned. No paint_object next step (get_object_paint). Multi-object answers: add_objects_texture_changes (list + one summary line). Before-state is read after the panel closes. [McpNextSteps] now checks top-level enum values.
