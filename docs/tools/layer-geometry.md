# Native sliced-layer geometry

`get_layer_geometry` reads one completed native FFF slice without slicing, rendering,
exporting or changing the project. `plate_index` is required and zero-based. Choose
exactly one `layer` (the one-based Preview slider number) or finite `z` in millimetres.
The nearest printed height wins, with ties going lower and requests outside the print
clamped to its lowest or highest printed height. A Z query echoes `requested_z` and
`also_at` for sequential-print layers at the same height.

For an app listening on its configured MCP port, this JSON-RPC request reads support
and interface centerlines:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_layer_geometry","arguments":{"plate_index":0,"layer":1,"features":["support","support_interface"],"max_points":512}}}
```

A successful tool response contains JSON text with `status:"success"`, schema version
1, source `gcode_processor`, application/server versions and the existing build commit
label (or null when unavailable). `paths` contains ordered fragments with `start_move`,
`feature`, `filament`, `width_mm`, `height_mm`, `object_label_id`, `continues` and XYZ
`points`. `empty_layer` means no supported feature extrusions; `nothing_matched` means
none matches the filters. Empty results complete successfully with an explanatory hint.

## Paging

The complete success JSON is at most 64 KiB. `max_points` defaults to 512 and accepts
2 through 8192, counting repeated boundary points. While `has_more` is true, copy the
entire `next_cursor` unchanged into the next call's `cursor`; keep the same resolved
layer and canonical feature/filament filters. The next call may use the resolved layer
number instead of Z, and may change `max_points`. Stop when `has_more` is false and
`next_cursor` is null. Do not restart silently on an error.

A fragment starts at the previous move endpoint and includes each native extrusion
endpoint. `start_move` identifies its first segment in the processor moves. A long run
split across pages repeats its boundary endpoint; `continues:true` identifies this
continuation. Metadata changes and intervening travel or filtered moves create a new
run even when coordinates coincide. Concatenation must preserve these boundaries.

`result_id` combines the process session, plate identity/index and native processor
result identity. It is shared across layers of that completed result and is not an
exported-file fingerprint. A new native result invalidates cursors even if geometry is
identical. `layer_digest` (domain `orcamcp-layer-geometry-v3`) separately hashes the actual physical
predecessor raw index and explicit encodings of every record from that predecessor
through the selected layer, including all `internal_only` flags, the captured
`frame_digest` and selected-layer XYZ,
move type/role, filament, width, height, raw label, range/layer metadata and the
predecessor endpoint. In-place geometry changes therefore invalidate cursors too.
Each request scans/hashes the selected layer; paging bounds response memory, not
execution time. No layer cache or lifecycle generation store is involved.

## Coordinates and meaning

See [captured coordinate frames](coordinate-frame.md) for the additive result-bound
`coordinate_frame` object, normalization availability and strict oracle limitations.

Coordinates are native processor Preview XYZ in bed millimetres, including processor
offsets. They are not necessarily the original machine G-code coordinates. There is
no rounding, simplification, synthetic closure, origin shift or instance transform.
Interpolated internal-only Preview vertices are skipped without breaking physical runs.
Endpoints use the last noninternal predecessor, possibly before the selected raw layer span.
Native raw layer numbering/ranges are retained; representative-Z/fallback ignores internal copies.
Raw index gaps are expected; do not infer source indices from point counts.
The first predecessor may belong to the previous layer. Vase paths retain changing Z;
`layer.z` describes the last supported extrusion height, not a coplanarity guarantee.

Features are `perimeters` (including gap fill), `infill` (including bridges), `support`
(including support transition), `support_interface`, `brim`, `skirt`, and `prime_tower`
(wipe tower). Travel, custom purge and other roles are excluded. Filaments are the
one-based native processor filament slots; they are never remapped through printer
filament maps. Optional nonempty feature/filament arrays have set semantics and are
canonicalized. Native slot/filter metadata is bounded to the processor's 256 slots.

Width, height and raw `object_label_id` are native values. A label of -1 means unknown
or outside a label; a label is not a model-object index or proof of support ownership.
Support/interface paths describe extrusion ribbons, not support topology, connected
bodies, contact polygons or mechanical contact. This endpoint does not report volume,
flush events or original arc commands. Existing rendered layer plans retain their
width-tolerant grouping; geometry fragments split on exact width, height and label
changes and ignore internal split copies. The legacy renderer includes interpolated split
widths, so its extruded area may differ. Compare renderer to its legacy policy and geometry
to the shared physical-record policy; classify native Seam midpoint/offset discrepancies
against original G-code rather than requiring false cross-policy area equality.

## Admission and errors

All result access occurs within one non-yielding GUI callback after a synchronized
background-worker readiness snapshot. INITIAL and IDLE permit a read; readiness
contention or a nonquiescent worker state returns retryable `slice_busy`. This refusal
is global across plates, including automatic/export work whose explicit slicing flag
is false. After readiness passes, pending settings or explicit slice state returns
`no_valid_slice`. The endpoint never applies pending settings or waits for a worker.
A readiness observation is valid only inside that callback, because new work can start on the next GUI callback.

Framework schema rejections remain JSON-RPC -32602; unexpected faults remain -32603.
Endpoint domain errors are normal tool JSON text:

```json
{"status":"error","code":"slice_busy","message":"Background slice work is active or awaiting completion; retry later"}
```

Other codes are `invalid_arguments`, `invalid_plate`, `no_valid_slice`, `stale_slice`,
`unsupported_source`, and `output_limit`. Retrying busy does not promise a fixed
completion time. A mismatched cursor after a valid new slice yields `stale_slice`.
Imported/preview-only sources are unsupported. There is no model fallback.

## Native validation

Build the Release app with tests enabled using the platform build instructions. Run
`slic3rutils_tests "[LayerGeometry]" --order rand` and the existing `[LayerPlan]` tests,
then regenerate the bridge tool catalog through the native registry golden-file test
(`ORCAMCP_UPDATE_TOOLS_GOLDEN=1`). Run the bridge suite with
`python3 -m unittest discover -s scripts/tests -t scripts`.

Synthetic native tests exercise selection, validation, exact metadata, all feature
names, paging limits/reconstruction and independent identity fences. Runtime
acceptance additionally requires a source-built app: discovery, first post-completion
read, automatic/export/cancel busy admission, thumbnail polling, queued GUI mutations,
and independent exported-G-code/render comparisons on support, vase and multifilament
fixtures. Per-page native logs report returned bytes/points and digest, serialization
and total GUI time. Peak allocation and runtime fixture measurements require their
own profiler/oracle evidence; a unit-test pass does not establish them.
