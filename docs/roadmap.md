# OrcaMCP roadmap

Work agreed to be worth doing but deliberately kept out of the releases so far (v2.5.0.6-dev,
planned in `docs/superpowers/plans/2026-09-26-release-2506/`; v2.5.0.7-dev, the upstream catch-up).
The user picks items up from here once the current release ships. Each item records what was
decided and what is already known, so nobody has to rediscover it.

---

## Next release: pointing and the window screenshot, together

**The goal.** The user can show the agent what they mean instead of describing it:

- they click the thing in OrcaSlicer and say "this one";
- the agent can look at the OrcaSlicer window, with the cursor drawn on it.

It came from the user struggling to explain in words what a click would have shown.

**Decided on 2026-09-26:**

- Pointing and the screenshot ship together (planned for the release after v2.5.0.6-dev; the upstream
  catch-up, v2.5.0.7-dev, went first).
- Both must work on macOS, Windows and Linux. If one platform can't be done, the feature waits;
  it doesn't ship for some platforms only.
- Pointing is by **click**, not hover.
- The work gets self-contained prompts, like the v2.5.0.6-dev folder, written when it's picked up.

### Pointing, by click

**The flow the agent follows.**

- **Read on cue only.** The agent reads clicks when it asked the user to click, or when the user's
  words point ("this one", "here", "where I clicked"). Never speculatively, and never every turn.
  Reading unasked would spam results with clicks made for other reasons.
- **Teach it once per session, when it helps.** The user may not know they can point. The first time
  a description is ambiguous, or the agent is about to ask "which one?", it tells them they can
  click the spot in OrcaSlicer and say "this one".
  - This goes in the tool description and in the server instructions (the instructions come from
    v2.5.0.6-dev prompt 08). An agent never sees a tool's description until it loads the tool, so
    the instructions are what make it offer pointing at all.
- **Echo before acting.** The agent restates the click in plain words: "you clicked the left pompom,
  white, filament 3". For inspection it then carries on; before anything that changes the scene,
  the echo is a confirmation question.
- **Freshness.** Every click carries a sequence number and an age.
  - When the agent asks for a click, it notes the current number first and accepts only later clicks.
  - A user-initiated "this one" with a click older than about a minute gets checked with the user
    before it is used.

**What a click reports.** The tool keeps the last few clicks, which gives "these three" and "from
here to here" for free. Each click records:

- where it landed: the 3D view, the bed, or the object list;
- **3D view:** the object, part (volume), facet and instance; the point in bed millimetres and the
  surface normal; the paint state at that facet (filament, support, seam); and the component id,
  when the part has several shells;
- **bed with no object under the click:** the bed point, for "put it here";
- **object list:** the row type (object, part, modifier, layer range or settings group) and its ids.

**It works on single-shell STLs.** A click gives a surface point and a facet, not a part. The facet
goes straight into `paint_object`'s `connected` seed: `seed: {volume_id, facet}` is already accepted
(`OrcaMCPPaintTools.cpp:~534-541`). Connected fill stops at sharp creases, and the agent can fall
back to a sphere around the point. Clicks also drive:

- support and seam painting at a spot;
- cutting at the clicked height;
- placing on a clicked bed point;
- measuring between two clicks, or a height band between them;
- "what's this?": height above the bed, the overhang angle from the normal, and the painted filament.

**Also in scope: a `paint_object` dry run.** It reports the region's facet count, area and
bounding box, plus a render with the region highlighted, before anything is painted. This makes the
echo step concrete where a region's edges are soft.

**Implementation notes and traps.**

- **Capturing clicks:** one app-wide wx event filter (`wxEvtHandler::AddFilter`), with no edits to
  upstream widgets. Verify that it sees GL-canvas mouse events on all three platforms.
- **Click vs drag:** a click is a press and release with almost no movement. Left-drag rotates the
  camera.
- **Gizmos:** don't record clicks while a paint, cut or other gizmo is active (brush strokes are
  clicks), or tag each click with the active gizmo.
- **Picking:** map the pixel to a pick ray with the live camera and the canvas scale factor (Retina
  on macOS, DPI scaling on Windows). Reuse the code behind `pick_facet`. Pick at click time, not at
  read time, so a later camera move can't change the answer.
- **Selection:** clicking selects the object in Orca. That is harmless, and it gives the user a
  visible highlight.
- **Tests:** unit tests for the click/drag classifier and the pixel-to-ray mapping, plus a manual
  check on each platform. Windows and Linux need a person at a machine, so plan that with the user.

### The window screenshot, with the cursor

**What.** A tool that shows the agent the OrcaSlicer window, either whole or a region (object
list, 3D view, notifications), with the cursor position drawn on it.

**Known so far:**

- **Don't use system screen capture on macOS.** `CGWindowListCreateImage` is obsolete, and
  ScreenCaptureKit needs a screen-recording permission. macOS prompts for that again for every
  freshly built binary, the same pain as the keychain prompt that `ORCAMCP_SKIP_CLOUD_LOGIN` works
  around.
- **Draw the window in-process instead:**
  - AppKit views: `cacheDisplayInRect:toBitmapImageRep:`.
  - The OpenGL canvas comes out blank that way. Capture it from its own framebuffer right after it
    draws (a capture-next-frame flag on `GLCanvas3D`, then `glReadPixels` before the swap), and
    composite it at the canvas's frame.
  - Web views (Home, Device) need `WKWebView` snapshots, which are asynchronous.
- **Windows:** `PrintWindow` with `PW_RENDERFULLCONTENT` covers GL and composited content.
- **Linux:** X11 can read the window; Wayland can't, so fall back to in-process drawing.
- **Shared code with pointing:** the cursor and click positions use the same window coordinates as
  the click capture, so build the two on one coordinate helper.
- **Bonus:** capturing the live 3D canvas also gives pictures of the sliced preview exactly as the
  user sees it.
- **Size:** medium on macOS alone, large across three platforms. Start with a one-hour spike on
  macOS to confirm the GL compositing.

---

## Later

### The Design (CAD) tab for agents: not now

**The goal.** An agent models in the slicer: sketches, constrains, extrudes, fillets, and commits the
part to the plate, then slices it with the tools it already has. The user, 2026-09-29: "I ABSOLUTELY
LOVE IT and can't wait to experiment with it."

**What upstream shipped (merged in v2.5.0.7-dev, PR #15238, 2026-09-18).** A sketch-first parametric
CAD tab on OCCT and SolveSpace's libslvs, behind an experimental preference. The model is a recipe of
features, replayed on every change and saved in the 3MF. It comes with its own agent surface: about 60
JSON-RPC verbs (`sketch_begin` / `sketch_add` / `sketch_commit`, `extrude`, `revolve`, `fillet`,
`chamfer`, `shell`, `hole`, `pattern`, `boolean`, `measure`, `mass_properties`, `import_step`, ...) in
`src/slic3r/GUI/CAD/McpControl.cpp`, over a Unix socket that opens only with `ORCA_CAD_MCP` set, through
its own bridge (`tools/orca_cad_mcp_bridge.py`). No Windows transport.

**Its state on 2026-09-29.** Early. The PR's own notes: most tools never click-tested, the defect rate
"has not converged". PR comments report hard crashes on macOS drawing a rectangle or circle in a new
sketch, flicker, broken scaling; one regular contributor asked for a revert. Upstream's changes since the
merge are build and OCCT 8 fixes only. The geometry kernel has about 200 test cases in CI; the crashes are
in the mouse-driven sketch UI, which an agent path through the kernel skips.

**Decided on 2026-09-30.** The user tried it: "this tab IS NOT READY". No investment for now: no tools, no
probe of its socket, no fixes. After every upstream sync its state is checked and reported (CLAUDE.md, "The
Design (CAD) tab -- check its state after every sync"); the user decides when it is worth picking up. If it is,
the aim stays the verbs as OrcaMCP tools on the one server (every platform, instance routing, argument checks,
the golden tool list), starting with a hands-on probe of upstream's socket.

### Hover pointing

**What:** read what is under the cursor right now, without a click. This is for voice users who keep
the mouse over OrcaSlicer while they talk.

**Why later:** a typing user moves the mouse to the terminal before the agent can read it, so the
last hover is just wherever the mouse left the window. Click pointing is deliberate and survives
that trip. Build hover on top of the click capture and pick code.

### Pointing at settings fields

**What:** the user clicks a setting in the Process, Filament or Printer tab, and the agent gets its
config key and current value ("what does this do?", "change this one").

**Why later:** it needs a map from wx widgets back to option keys (`Field` / `OptionsGroup`), which
is separate from 3D and object-list pointing.

### Slicing and plate-lifecycle behaviours left as upstream has them

Found by prompt 04c on 2026-09-27. These are upstream behaviours, not crashes, so they were
deliberately left alone. Each is a candidate for a small fork fix (with a probe line), or for an
upstream issue if the user wants one filed.

- **Restarting Slice All.** `slice_all` (all plates) during a running Slice All restarts the run from
  plate 0.
- **Undo and the "sliced" flag.** Undo restores each plate's "sliced" flag from the snapshot, so a
  plate sliced after the undo point shows as not sliced.
- **The Slice All flag is never cleared.** Upstream never clears it at the end of a run; other GUI
  paths clear it before slicing. Only MCP's single-plate path was fixed.
- **A busy worker between plates.** Slice All doesn't wait for a busy UI worker between plates. The
  fork now ends the run and says so, but doesn't retry.
- **No cancel tool.** MCP has no tool to cancel a running slice or Slice All.
- **Tool calls run while a dialog is open.** Tool calls still run while a user-facing modal is open.
  `get_scene_info` reports it (prompt 04c), but tools don't refuse; that is a product decision. For
  example, answering the restore prompt later can load the backup over an agent's project.
- **Closing a dialog with its close box.** Many upstream Yes/No callers only check for No, so
  closing the dialog with the close box goes ahead (`Tab.cpp`, the sync-printer dialog,
  `PrintHostDialogs`). This is a possible upstream issue.
- **Windows logout with a dialog open.** wx's Windows end-session handler deletes every top-level
  window. With a stack-allocated dialog open at logout, that likely crashes, in upstream too. It is
  unverified; it needs a Windows machine.
- **Slow cancel in organic tree supports.** They check for cancel only between phases (47 s on the
  -O0 dev build). This is upstream algorithm code.

- **`TriangleMeshStats::merge` repair counts.** It keeps only the last mesh's `repaired_errors`
  (upstream). A correct fix must distinguish volumes from instances: `ModelObject::mesh()` merges
  `raw_mesh()` once per instance, so summing overcounts. Prompt 05 tried a fix and reverted it on
  2026-09-27.
- **Repair counts from STL files.** Upstream's STL loader repairs meshes but throws away the repair
  counts (`TriangleMesh.cpp`, `#if 0`), so the GUI never flags a repaired STL.
- **`object_id` validation.** About 20 MCP tools still validate `object_id` their own way; move them
  onto the shared `resolve_object_id` (prompt 05) in a follow-up.

- **Ghost "Loading..." windows on a locked screen.** While the Mac's screen is locked, AppKit's close
  animation never finishes, so each progress dialog stays listed as an on-screen ghost until the app
  quits. On 2026-09-27, with the screen unlocked, five loads on the release app left none (checked
  with the user present). A possible fix is to turn off the animation for `ProgressDialog` on macOS
  (prompt 04d's design, probe U). It only matters for unattended agent runs; it was not shipped,
  because it changes the dialog for every user to fix something users don't see.

- **Finding overhang layers.** In the 2026-09-27 acceptance pass, finding the layer where a cap starts
  took four `render_plate_view` layer calls of guessing. A summary (e.g. in `get_print_estimate`, or as a
  `layer_view` option) listing the layers where overhang area first appears, with each one's
  `support_below` gap, would find it in one call.

### Create printer and Create filament

**What:** the sidebar's **Create printer** (`CreatePrinterPresetDialog`: a new printer preset from a
vendor model's template or the current printer, with a custom name, nozzle, bed shape and height, and
copies of the chosen filament and process presets made for it) and **Create filament**
(`CreateFilamentPresetDialog`: a new filament preset from a vendor and type template, for chosen
printers), as MCP tools.

**Why later:** decided on 2026-09-28 (prompt 13). Both dialogs keep their logic inside their UI: the
Create button's handler (`CreatePresetsDialog.cpp`, about 5,000 upstream lines) reads its widgets,
validates, clones the filament and process presets (`clone_presets_for_printer`) and saves the printer
preset in one lambda. A tool that runs "what the GUI runs" needs that core pulled out of the dialog
first, a large change to an upstream file. What an agent needs most is already there: `install_presets`
installs any printer or filament the app ships (the Setup Wizard's install), and `clone_preset`,
`apply_config` and `save_preset` make a custom printer or filament from an installed one.

**Known so far:** upstream's `PresetBundle::load_system_models_from_json`, which lists the vendor models
for Create printer, reads `.json` profiles only, while a release build ships every vendor as an `.opc`
cache (65 caches and one `.json` in `/Applications/OrcaMCP.app` on 2026-09-28): Create printer's model
list may be empty in a release build. Not checked live; check before building on it.

### 3D toolpath render

**What:** render the sliced G-code from any camera, coloured by feature, speed or tool, with a layer
range and a feature or extruder filter.

**Why later:** it is large, and upstream has no G-code-to-image renderer. v2.5.0.6-dev ships the
cheaper top-down layer plan (prompt 07). The window screenshot's live-canvas capture also partly
covers this.

**Known so far:**

- **Recommended route:** a private `libvgcode::Viewer`, loaded from
  `PartPlate::get_slice_result()` through `libvgcode::convert` (`LibVGCodeWrapper.hpp:73`).
  - Filter the vertices by `extruder_id`, `role` and `layer_id` (`PathVertex.hpp:60-76`) before
    loading.
  - Render into the existing `OffscreenRenderTarget`, using the MCP camera matrices.
  - This leaves the user's preview untouched, at the cost of a second GPU copy of the toolpaths.
- **Why not reuse the preview's viewer:** `GCodeViewer::render_toolpaths()` is private and uses the
  plater's camera (`GCodeViewer.cpp:2348-2357`), so that route would need a fork patch.
