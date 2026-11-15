# Native Bambu camera capture

`get_bambu_camera_frame` returns one JPEG MCP image and JSON text metadata for
an explicit `printer_id` matching the selected, connected native LAN Bambu
printer. It uses `MediaPlayCtrl` and `wxMediaCtrl3`; connection credentials and
camera URLs stay inside the existing native player. It neither selects a
printer nor sends print commands.

The native Device page is constructed on demand without selecting its tab.
Existing playback is borrowed. Idle playback is started with a capture lease,
which suppresses hidden-page teardown during the request. Cleanup stops only
playback that lease started. A target/mode change, playback action, or showing
the Device page invalidates the lease; cleanup cannot stop replacement playback.
The normal player remains responsible for teardown at application shutdown.

The HTTP request has one absolute deadline of at most eight seconds, including
initial construction/queueing, polling, and cleanup. Its value-owned gate calls
can release the HTTP caller even when GUI work has already started; ordinary
reference-capturing gate calls retain their existing wait-until-finished rule.
Queued work that has not started is abandoned at the deadline. Running work
holds shared request state, checks cancellation before starting playback, and
releases a lease when it completes late. It never writes into a returned caller's
stack or returns a late image.

Cleanup gets only the remaining request budget. If the GUI is stalled, an error
with `cleanup_pending:true` and no image returns at the deadline; an idempotent,
value-owned cleanup runs when the GUI resumes. It stops only capture-owned
playback. If shutdown closes the gate, native player destruction owns teardown.
There is no unbounded cleanup wait in the HTTP request. A frame
must have decoded after the request began and be at most two seconds old when
encoded. Metadata binds the image to `printer_id`, `received_at_unix_ms`, `age_ms`,
`timestamp_basis`, `width`, and `height`. Time means native decode/receive time,
not camera exposure time, which the native decoder does not provide. Output is
at most 1280 pixels per side and two MiB of JPEG bytes; no camera file is written.

Refusals return only the ordinary JSON text envelope: `camera_unavailable`
(connection, LAN capability, startup/backoff or encoding unavailable),
`camera_identity_changed` (selection or playback ownership changed),
`camera_stale` (only old frames observed), or `camera_timeout` (no frame). These
are observations, not evidence that the camera or physical print is healthy.

Use `get_printers` and `select_printer` to inspect/select an already provisioned
native device before calling. The capture tool does not provision credentials.
Qualify an installed build through the actual MCP bridge with a real camera
image, identity and freshness metadata; mocked/unit tests do not establish live
camera availability. No print or printer-control operation is part of this flow.
