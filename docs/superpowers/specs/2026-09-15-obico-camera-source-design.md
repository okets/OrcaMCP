# Obico as the camera and watch source for the Flashforge console — design

> **Amended 2026-09-16, before release:** the console shows **only Obico's primary camera**. The
> thumbnail strip for additional cameras described below was built and then removed on the user's
> request; `cameraSources()` in `index.html` keeps the primary (or first streamable) Obico webcam
> and drops the rest. The fallback order — Obico stream → Obico snapshot → the printer's own stream
> — is unchanged. Re-enabling multi-camera is a one-line change at that filter.


**Date:** 2026-09-15
**Status:** draft for review
**Companion:** the `flashforge-obico` agent (separate repo, `~/Projects/flashforge-obico`), whose
spec is `docs/superpowers/specs/2026-09-15-flashforge-obico-agent-design.md` there.

## 1. Problem

The Creator 5 / 5 Pro camera is an MJPG-Streamer that serves **one client at a time** and has no
snapshot action (verified 2026-09-15: a second concurrent client is reset within ~10 ms). Once an
Obico agent owns that stream for failure detection, the Flashforge console, which today takes
`cameraStreamUrl` from every `detail` reply and puts it straight into an `<img>`, shows
"Camera stream unavailable" for as long as the agent runs.

The agent fixes the sharing problem by re-serving every camera at full frame rate on the Unraid
host, and it publishes those addresses through Obico in the printer's webcam list. What is missing
is a way for the console to know about Obico at all.

## 2. Goal

A physical Flashforge printer can be given an **Obico server** (URL plus the printer's Obico
token). When it has one, the console:

- shows every camera Obico lists for the printer, played from each camera's advertised local
  stream at full frame rate, with the primary camera large and the others selectable;
- shows Obico's watch state for the running print (detection confidence, and a failure alert when
  Obico has raised one);
- falls back gracefully: advertised stream → Obico's latest snapshot → the printer's own stream →
  "unavailable".

When it has none, nothing changes: the console draws the camera straight from the machine as it
does today.

Out of scope: pausing or resuming through Obico from the console (the console already pauses the
printer directly), Obico's WebRTC video, Obico login or printer creation from inside the slicer.

## 3. Design

### 3.1 Preset: two new physical-printer keys

| Key | Type | Shown | Meaning |
|---|---|---|---|
| `flashforge_obico_url` | string | Flashforge hosts only | Obico server base URL, e.g. `http://10.0.0.2:3334`. |
| `flashforge_obico_token` | string | Flashforge hosts only | The printer's Obico auth token (the same one the agent uses). A credential: stored like `printhost_apikey`, never logged, never returned by MCP tools. |

Both are defined in `PrintConfig.cpp` next to `flashforge_serial_number`, registered in the two
physical-printer key lists in `Preset.cpp` (lines ~1439 and ~4056 today), appended to the
`PhysicalPrinterDialog` option group and shown/hidden together with `flashforge_serial_number`,
and added to the dialog's saved-keys list (`PhysicalPrinterDialog.cpp:804`). Both empty means
"no Obico"; one empty is a validation error in the dialog ("Obico needs both a server URL and a
printer token").

### 3.2 Handler: pass the Obico settings to the page

`FlashforgeConsoleHandler::start_polling` already builds the session `identity` from the preset
(`preset`, `host`). It gains, when both keys are set,

```json
"obico": {"url": "http://10.0.0.2:3334", "token": "…"}
```

so every snapshot pushed to the page carries it. Nothing else in the handler changes: the printer
poll loop, cadence, and commands are untouched, and the handler does no Obico networking.

Why the page and not C++: the page already owns the camera `<img>` and every render decision, a
`WebSocket` is native in the web view (WebKit on macOS, WebView2 on Windows, WebKitGTK on Linux),
and the only Boost.Beast client in the tree (`WebSocketClient.hpp`) is a blocking demo-grade
class. The token does go into the page, which is our own `file://` document with no third-party
script, on the same footing as the API key `PrinterWebView` passes to other vendors' device pages.

### 3.3 Page: an Obico client module

A new, self-contained block in `resources/web/flashforge/index.html`, `obicoLink`, with one
purpose: keep a websocket to Obico open while the page has Obico settings, and expose the latest
Obico printer document.

- **Connect** to `ws(s)://<host>/ws/token/web/<token>/` (scheme follows the URL: `https` → `wss`).
  This Obico route authenticates with the printer token alone and pushes the full printer
  document (`status`, `pic.img_url`, `settings.webcams`, `current_print`, `normalized_p`) on
  connect and on every change. Being connected also makes Obico tell the agent someone is
  watching, which raises the snapshot rate; no polling is needed.
- **Reconnect** with backoff 2 s → 60 s. Close and forget the document when the page is suspended
  (printer deselected) or the Obico settings disappear from a snapshot; open when they appear.
- **Expose** `obicoLink.doc()` (latest document or `null`), `obicoLink.state()` (`off` |
  `connecting` | `connected` | `unreachable`), and call the page's `render` with the last printer
  snapshot whenever the document changes, so Obico updates repaint through the same path as
  printer updates.

### 3.4 Page: camera rendering

`renderCamera` becomes a chooser over ordered sources, tried in this order, each falling through
on `<img>` error:

1. **Obico webcams with `stream_url`** — one entry per item of `settings.webcams`; the
   `is_primary_camera` entry is shown large, the rest as a row of named thumbnails that swap into
   the large slot on click (Obico's own dropdown pattern, but visible). Each thumbnail is its own
   `<img>` on the camera's `stream_url`, so switching is instant.
2. **Obico snapshot** — `pic.img_url`, re-set on every document push. Labelled "Snapshot via Obico"
   so a ~1 fps picture is not mistaken for a broken stream. Primary camera only; Obico stores no
   other camera's pictures.
3. **The printer's own stream** — today's behaviour, `p.camera_stream_url`. Used when no Obico is
   configured, or when Obico is unreachable and its cached webcam list is gone.
4. **Unavailable** — today's message, extended to say which sources were tried.

As today, the `<img>` is created once and only its `src` is touched, so re-renders never restart a
stream; the chooser only acts when the chosen URL changes.

### 3.5 Page: Obico watch state

A small line under the camera, present only when Obico is configured:

| Condition | Text |
|---|---|
| `state() === 'connecting'` | "Obico: connecting…" |
| `state() === 'unreachable'` | "Obico unreachable — showing the printer's camera" (or "— last snapshot", per the source in use) |
| connected, no `current_print` | "Obico: watching, idle" |
| connected, printing | "Obico: watching · confidence low / medium / high" from `normalized_p` (Obico's own gauge thresholds: < 0.33, < 0.66, ≥ 0.66) |
| `current_print.alerted_at` set and not `alert_muted` | The existing banner slot shows **"Obico has flagged a possible failure"** with the alert time; it clears when Obico clears it. |

Nothing on this line is actionable; acting on an alert (pause) happens through Obico's own
notification and app, or the console's existing pause button.

### 3.6 MCP: read-only visibility

`get_printer_status` (`OrcaMCPPrinterUtils.cpp`, `printer_json`) adds

```json
"obico": {"configured": true, "url": "http://10.0.0.2:3334"}
```

with the token deliberately absent. `add_physical_printer` gains optional `obico_url` and
`obico_token` parameters that set the two preset keys, so an agent can configure the link the same
way a human does. No tool talks to Obico.

### 3.7 Documentation

- `docs/printers/flashforge-creator-5.md`: a new "Failure detection with Obico" section under
  "The device console": what to fill in, what the console then shows, and a pointer to the
  `flashforge-obico` repo. The "Known limitations" camera bullet gets the single-client fact.
- `docs/printers/flashforge-lan-api.md` §5 gets the verified single-client result and the missing
  snapshot action as facts, replacing "test this before you design around it".
- `CLAUDE.md` tool table: `add_physical_printer` description mentions the Obico parameters.

## 4. Failure handling

| Situation | Behaviour |
|---|---|
| Obico URL unreachable | `unreachable` after the first failed connect; camera falls back per §3.4; reconnect with backoff; the printer poll is unaffected. |
| Token rejected | Obico closes the socket immediately; shown as "Obico: token rejected" (distinguished by a close before any document arrives, twice in a row) and no further reconnects until the settings change. |
| Webcam entry without `stream_url` (a stock Obico agent, not ours) | That camera is not offered as a stream; the primary falls to the Obico snapshot. |
| Advertised `stream_url` unreachable (e.g. Unraid re-server down) | `<img>` error → next source. Re-tried on the next document push. |
| Page suspended | Socket closed; nothing is retried until the page is resumed. |

## 5. Testing

- **C++ (Catch2, `test_flashforge_console.cpp`):** the identity carries `obico` only when both
  keys are set; `printer_json` reports `obico.configured`/`url` and never the token;
  `add_physical_printer` round-trips the two new keys.
- **Page:** the page has no JS test harness today; the chooser and the watch-state text are written
  as pure functions of `(snapshot, obicoDoc, obicoState)` returning a plain description, and a
  Catch2 test evaluates them through the same `SAMPLE`-style fixtures the page already carries
  (the fixture JSON is the contract; the functions are small enough to reason about by reading).
  If that proves awkward in practice the fallback is a hand-run checklist in the PR.
- **Manual acceptance (macOS first, then Windows):** with the agent running on Unraid: console
  shows the primary camera at full frame rate from `10.0.0.2:8081`; a second configured camera
  appears as a thumbnail and swaps in on click; stopping the agent container makes the console fall
  to "Snapshot via Obico" and then, after stopping Obico too, to the printer's own stream;
  clearing the Obico fields restores today's behaviour exactly; `get_printer_status` never shows
  the token.
- **First thing to verify during implementation:** that a `WebSocket` to `ws://` and an `<img>`
  to `http://` both work from the `file://` page inside the wx web view on macOS and Windows. The
  `<img>` case already works today (the printer stream is `http://`); the websocket case is
  expected to (neither WebKit nor Chromium applies mixed-content blocking to a non-`https` page)
  but has not been tried. If it is blocked, the socket moves to C++ behind the same `obicoLink`
  interface, and the page design does not change.

## 6. Decisions taken

- **Obico is the single thing configured**; camera addresses are discovered through it. The
  alternative, a camera-URL override in the preset, would need one field per camera and would
  drift from what Obico knows.
- **Full-rate streams come from the agent, not Obico.** Obico's picture path is one snapshot per
  second for one camera; its multi-camera path is WebRTC. Advertising the agent's local streams
  through Obico's webcam list gives multi-camera at full rate with no new protocol.
- **The websocket lives in the page.** Native, non-blocking, and next to the rendering it drives.
- **Read-only integration.** The console never sends commands to Obico; the printer stays the
  authority and the existing direct pause/resume path is unchanged.
