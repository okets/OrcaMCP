# OrcaMCP roadmap

Work agreed to be worth doing but deliberately kept out of the current release (v2.5.0.6-dev,
planned in `docs/superpowers/plans/2026-09-26-release-2506/`). The user picks items up from here
once the current release ships. Each item records what was decided and what is already known, so
nobody has to rediscover it.

---

## Next release: pointing and the window screenshot, together

**The goal.** The user can show the agent what they mean instead of describing it:

- they click the thing in OrcaSlicer and say "this one";
- the agent can look at the OrcaSlicer window, with the cursor drawn on it.

It came from the user struggling to explain in words what a click would have shown.

**Decided on 2026-09-26:**

- Pointing and the screenshot ship together, in the release after v2.5.0.6-dev.
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
