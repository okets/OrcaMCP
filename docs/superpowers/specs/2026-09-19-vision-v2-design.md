# Vision v2 — design

Written 2026-09-19, overnight, from the user's brief: *"I adopt all of your suggestions. This release
will be about vision-v2, not just making the old one work."* The suggestions came out of an evening in
which the agent produced fifteen black renders before understanding why (see the plan's Stage 3 notes
and the `render_plate_view` root-cause commit). Every item below removes one thing the agent had to
guess.

## Goal

An agent asking for a picture of a plate should get back an image it can *read* — located, labelled,
lit — together with the numbers that make the picture checkable without looking at it. It should
never have to compute a camera, and it should be told when a picture shows nothing.

## Non-goals

- No change to how the 3D scene is built or which volumes exist; this is presentation.
- No GL text rendering. Labels, grid and outline are drawn in 2D on the finished image with wx,
  using the render camera's own matrices to project 3D points. This keeps overlays testable and
  independent of GL state.
- The G-code preview (toolpaths, colours per feature) is not re-implemented. The sliced view is a
  first-layer *plan* drawn from the print's own first-layer polygons.
- Before/after diff renders: only if everything else lands.

## Components

### 1. Offscreen render target (done, prerequisite)

`RenderThumbnail` draws into its own framebuffer and restores the previous binding and viewport. Without
this every feature below inherits the "black when the Preview tab is in front" failure.

### 2. Response metadata — `RenderReport`

Every view in `render_plate_view` returns, alongside the image:

- `frame`: `"bed_mm"`, and `plate_origin`: `[x, y]` of the requested plate's bounding box minimum, so
  an agent can convert plate-local numbers itself.
- `camera`: the echo it has today (position, target, matrices, viewport, projection type), plus
  `preset` when one was used and `fit` (`"plate"` or `{ "object_index": n }`).
- `objects_in_frame`: for every volume drawn, `{ object_index, name, screen_bbox: [x0, y0, x1, y1],
  clipped: bool }` where the bbox is the projection of the volume's transformed bounding box, in image
  pixels with a top-left origin, and `clipped` says part of it fell outside the viewport.
- `uniform_image`: true when the rendered pixels (before overlays) are a single colour. When true the
  response also carries `hint`: which plate the camera is actually looking at, if any, or "no volumes on
  plate N are inside the view".
- `overlays`: which overlays were drawn.

The bbox projection is a pure function of (bbox, view, projection, viewport) and is unit-tested.

### 3. Overlays — self-locating images

Drawn in 2D after the GL pass, projected through the same camera:

- **Plate outline**: the plate's printable rectangle at z = 0, as a 1 px line.
- **Grid**: 10 mm lines inside the outline, faint; every 50 mm slightly stronger.
- **Origin marker** at the plate's front-left corner, with `X` and `Y` letters a short way along each
  axis, so orientation is unambiguous from any camera.
- **Object labels**: the `object_index` drawn at the projected centre of each visible object's bounding
  box, in the object's colour on a dark pill, so it stays legible on any background.
- **Excluded areas** of the plate, hatched, when the plate defines any.

Overlays default on; `overlays: false` disables all of them, and `overlays: { grid: false }` disables one.

### 4. Per-object colours

Each object (by `object_index`) gets a colour from a fixed 12-entry palette chosen for mutual contrast
on a light background; the same object keeps the same colour for the life of the session, and the
label uses it. The wipe tower keeps its light grey. Filament colours are not used: agents ask "which
object is that", not "which filament".

### 5. Light background and stronger emission

Clear colour becomes a light neutral grey; the `thumbnail` shader's `emission_factor` is raised so
that unlit faces are still readable. PNG is used when writing files (alpha preserved, no JPEG
smearing on 1 px overlay lines); JPEG remains for the inline base64 path where size matters.

### 6. Named cameras and auto-framing

`views[]` entries may carry `preset` instead of `camera_position` + `target`:

| preset | position relative to the fitted box |
|--------|-------------------------------------|
| `iso`  | +x, −y, +z corner, distance from the box diagonal |
| `top`  | straight above (with the stable-up fallback that already exists) |
| `front` / `back` | −y / +y, slightly above centre |
| `left` / `right` | −x / +x, slightly above centre |
| `low`  | front, at bed level looking slightly up — for first layers and feet |

`fit` chooses the box: `"plate"` (default) or `{ "object_index": n }`. Both are resolved to a
position/target pair by a pure function that is unit-tested, then rendered exactly like an explicit
camera. Explicit cameras remain bed mm; `frame: "plate_local"` on a view converts them.

When `views` is omitted, the tool renders `iso`, `top` and `front` fitted to the plate and composes them
side by side into one image (a contact sheet), returning one file and per-view metadata.

### 7. Sliced first-layer plan view

`layer_view: "first_layer"` renders a top-down 2D plan of the plate in bed mm → pixels, drawn with wx
on the CPU from the plate's `Print`:

- each object's first layer `lslices`, shifted per instance, filled in the object colour;
- the brim, from the print's brim extrusion entities, as outlines in a darker shade of the object
  colour (tree-support brims included: they are extrusion entities on the support first layer);
- the support first layer (`support_layers()[0]`) in a hatched grey;
- the wipe tower footprint, if printed, in the wipe-tower grey;
- the plate outline, grid and origin as in §3, and labels at object centroids.

If the plate is not sliced the tool returns the model footprints instead (what `GetObjectFootprint`
already computes, including brim extent) and says so in `source: "footprints"` vs `"sliced"`. This is
the view that answers "is the brim big enough" and "where do the tree feet land".

### 8. Zoom

`fit: { object_index: n }` replaces the request for higher resolution: a closer camera on one object.
`resolution` stays, default 512.

## Verification

- Pure functions (projection to screen bbox, preset camera resolution, palette assignment, grid line
  generation, first-layer polygon collection) get Catch2 tests in `tests/slic3rutils`.
- Every rendered feature is checked against the live app with `render_probe.sh`-style scripts: image
  not uniform, expected object count in `objects_in_frame`, and a saved PNG the agent reads back and
  describes. Screenshots go in the commit messages' evidence lines by path.
- Behaviour on an unsliced plate, an empty plate and a plate with excluded areas is exercised.

## Order of work

1 (done) → 2 → 3 → 4 + 5 → 6 → 7 → docs (`docs/tools/reference.md`, `get_server_info` examples,
CLAUDE.md tool table) → version bump to 2.5.0.3-dev → CI. The tag waits for the user.
