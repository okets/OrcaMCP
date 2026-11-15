# Native plate analysis

`get_plate_analysis` reads one completed native FFF result without changing settings,
slicing, exporting, rendering, or contacting a printer. `plate_index` is required and
zero-based. `section` is `summary` by default, or `detail`. Whole success JSON objects,
including timing metadata, are limited to 64 KiB. Integral JSON numbers such as `3.0`
are accepted for integer parameters; booleans, fractions and unknown keys are refused.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_plate_analysis","arguments":{"plate_index":0,"section":"summary"}}}
```

Both sections require a native project Print with objects and completed native slicing
and G-code export steps. Loaded sliced projects are unsupported before and after cached
G-code loading; an unchanged Slice can take the cached branch and remain unsupported.
Use an actual unsliced model project for comparisons. Worker readiness contention or
a nonquiescent worker state on any plate returns retryable `slice_busy`. After readiness passes, pending settings or explicit slice state returns
`no_valid_slice`; invalid results also return `no_valid_slice`. Every read and page is
re-admitted within one non-yielding GUI callback.

Every page also carries [captured coordinate-frame metadata](coordinate-frame.md).
Join only matching result and frame identities; unavailable normalization is not a zero offset.

## Summary: recorded material accounting

Summary is single-page. `native_estimate` retains numeric/null estimate fields for normal
and silent time, native filament/extruder counters and material usage. Used-filament rows
come from positive native total volumes. Silent time zero is unknown; normal time zero
is a recorded value. Missing properties needed for positive-volume length, weight or cost
produce null values, not zero. Zero category volumes have known zero conversions.

`profile_inputs.slots` lists every configured filament slot with native preset name,
diameter in mm, density in g/cm3 and price per kg. `price_status` is `ok`, `zero`, or
`invalid`; unavailable fields include reasons. Zero profile price preserves zero cost
arithmetic but does not prove the material is free or support a savings claim.

`material_categories` is slot-major, with model, support, prime_tower, flush and total.
`material_category_totals` contains the same plate-level categories. Every measurement
has `value`, `unit`, `availability`, `source`, and `reason` (null when known). Availability
is `recorded`, `recorded_zero`, `derived`, `unknown`, or `unavailable`. Units are mm3,
mm, g, or profile_money. `sources` maps short source keys to native fields.

Native model volume includes skirt, brim, custom start extrusion and flush into infill;
support combines support, interface and transition. Tower accounting uses native cache
attribution, which can differ from move slots during a switch. Flush is native tagged
E-only purge/VFLUSH/External_Purge accounting, with coverage not proven complete. Missing
FLUSH tags can yield recorded zero despite physical purge. Total volume already includes
flush: never add it again. Start-code purge and flush into infill are explicitly unavailable
as separately attributed categories. No per-change purge estimate or width-derived volume
is supplied. Postprocessing may replace moves while preserving original statistics;
summary/detail consistency is `not_verified`.

Costs include profile material cost only. `pricing.currency` is null and units remain
profile_money; hourly `time_cost` is excluded. `slice_inputs` captures native result settings
IDs plus immediately read live Print controls: flush multipliers, prime-volume mode, flush
matrix, nullable nozzle volumes, prime-tower enablement and time cost. Values are typed
scalars/arrays, with nil nozzle elements preserved as JSON null. These controls are labelled
`live_print_config_after_slice` / `not_proven_slice_time`, not immutable effective settings.
A missing control is unavailable, with no project-config fallback.

## Detail: bounded physical records

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_plate_analysis","arguments":{"plate_index":0,"section":"detail","max_moves":10000}}}
```

`max_moves` is detail-only, defaults to 10000 and accepts 5 through 10000. Every native
MoveVertex inspection counts: one tail read, one predecessor read and forward records,
including ignored internal vertices and discarded suffixes. Index zero is the native
dummy and never contributes. Only `internal_only:false` records are physical. Interpolated
Preview speed vertices do not add extrusion count, E, area, occupancy, events or endpoints.

A page commits only after a physical record. If a budget/byte stop falls inside internal
copies, that suffix is uncommitted and will be rescanned. If no physical record can commit,
`output_limit` requests a larger budget, or reports an unpageable source at the global cap.
Travel-only pages can validly advance with empty occupancy/events. No page silently loses
an event, layer row or occupancy contribution.

`range` is the committed raw half-open source interval; `inspected_moves` can exceed its
length plus two. `total_moves` and the tail's `final_layer` are shape metadata. Copy the
entire `next_cursor` unchanged while `has_more` is true; it contains result identity,
section, shape fence and the next raw index. Its predecessor must be physical. Terminal
pages have `has_more:false,next_cursor:null`; a supplied terminal index is invalid.
`max_moves` may change between pages. Result/shape mismatches return `stale_slice` before
cursor bounds are tested. Shape is not a content hash: the completed-publication lifecycle
remains part of the read contract. Never silently restart a failed continuation.

`layer_parts` describes consecutive physical-layer portions. Native IDs are one-based,
monotonically nondecreasing; heights need not increase or be planar. Empty native layers
have no row. Reduce the last endpoint/supported endpoint by greatest raw move index, then
choose supported Z when available and endpoint Z otherwise.

`occupancy_parts` is a stream of consecutive-key fragments keyed by layer, feature, explicit
untranslated native role symbol and one-based native filament slot. It counts physical
Extrude records and reports XYZ length, projected XY length, XY length times native width,
signed native filament delta E in mm, endpoint-inclusive Z bounds and raw first/last indices.
`other` retains custom/start-code extrusion; model-placement consumers usually exclude it.
Filament slots are not physical nozzle IDs and are never remapped a second time. Support
interface proves a recorded slot, not contact or ownership. Area counts overlaps repeatedly.
Unknown arithmetic results are null with reasons; invalid native geometric inputs are refused.

The predecessor is the last noninternal record across layers, including native Seam records.
A Seam midpoint can be synthetic, so geometric comparisons to original G-code may differ under
this declared predecessor policy. Preserve and classify that difference; do not widen a strict
fixture's tolerance or silently substitute another endpoint. The legacy layer renderer retains
its split-vertex width policy and can produce a different swept-area sum.

`events` preserves raw Tool_change/Color_change order, native source IDs, layer, slot, colour ID
and Z. A native initialization Tool_change is not a material swap. Color_change represents the
processor event for an active target, not every colour command or actual changed physical
material. Derive previous-tool slots from preceding Tool_change events, with first previous null;
there is no native previous_filament, physical-extruder mapping or per-event purge amount.

Reduce page-local parts by summing counts/E/length/area (null propagates), taking Z extrema,
unioning occupied layers and selecting last metadata by greatest source index. Deduplicate
retried pages by `result_id` plus `range`. Complete the stream before comparing results.

## Comparison and evidence

Use fresh sessions with identical copied unsliced source hashes. Capture model placement and
filament assignments before settings and after slicing; only requested settings changes should
separate A/B. Capture controls immediately after slicing as live evidence. These structural
snapshots do not detect concurrent human mesh-content edits. Missing capture or suspected edits
prevent causal purge-setting attribution.

Material cost delta requires identical slot, preset name and positive finite price across the
union of positively used slots. Zero/missing price or preset makes it unavailable; price changes
must be flagged. Label known deltas profile_money rather than dollar savings. Do not infer purge
savings from configured matrix values or equal layer numbers.

Native unit tests cover reducers, bounds, profile availability and typed nullable controls. Full
acceptance also requires the source-built app, native generation/load/cancel/worker cases and
independent exported-G-code evidence. Compare an arc-free fixture separately from Seam-policy
cases; report native postprocess/tower-attribution differences explicitly. Logs/timing fields
measure actual work; peak allocation and runtime latency distributions need retained profiler
measurements. A unit-test pass does not establish runtime or oracle acceptance.

Domain errors are normal tool JSON text with exactly `status:"error"`, `code`, `message`:
`invalid_arguments`, `invalid_plate`, `slice_busy`, `no_valid_slice`, `unsupported_source`,
`stale_slice`, or `output_limit`. Framework schema rejections remain JSON-RPC -32602;
unexpected faults remain -32603. No result silently truncates a field.
