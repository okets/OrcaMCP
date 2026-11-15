# Captured processor coordinate frames

Geometry and both analysis sections always include additive `coordinate_frame` metadata.
Existing requests, schema version 1, result IDs and detail cursor keys remain unchanged.
The legacy geometry `frame:"bed"` label means native processor-preview coordinates.
Frame inputs are captured with the moves, never reconstructed from current Print settings,
a padded bed box, plate-zero assumptions or the legacy result Z field.

```json
{
  "version": 1,
  "frame": "gcode_processor_preview",
  "binding": "captured_with_moves",
  "config_provenance": {"kind": "typed_print_config", "explicit_frame_keys": []},
  "result_id": "same-as-response",
  "frame_digest": "40-lowercase-hex-sha1",
  "normalization": {"availability": "available", "kind": "per_filament_translation", "reason": null},
  "processor_offsets": {"xy_mm": [256.125, -33.0625], "z_offset_mm": 0.125, "extruder_offsets_mm": [[0, 0, 0]]},
  "start_custom_z": {"policy": "first_layer_height_plus_extruder_z", "first_layer_height_mm": 0.2},
  "source_file_state": "not_verified"
}
```

The JSON above illustrates shape, not a measured slice. XY offsets are exact captured
doubles. Z, first-layer height and each resolved filament's XYZ offset are serialized as
`double(float)` so consumers can reproduce native float32 storage. The vector contains
exactly the configured slot count, at most 64, without another physical-nozzle remap.
Extra processor-vector entries are excluded; a shorter vector is unavailable.

Capture begins at the first real stored vertex, excluding the mandatory dummy. Later
XY/config/context changes invalidate the run, even if an input is restored. Reset clears
capture; copy/move assignments preserve it. Reload transfers frame and moves under the
same result mutex. A failed reload retains old numeric diagnostics but marks
`source_file_state:"reload_failed"` and refuses normalization. Successful reload records
`reload_succeeded`; ordinary processing begins `not_verified`. None is a file hash.

Normalization is available only for positively classified ordinary non-belt translation.
Typed configuration is `typed_print_config`. File reprocessing is
`explicit_gcode_config_block` only with successfully parsed literal keys captured before
legacy composite/default conversion. The bounded key list is `belt_printer`, `z_offset`,
`extruder_offset`, `initial_layer_print_height`. A literal unsubstituted `belt_printer`
is required for positive file eligibility. Missing/default-only/unsupported-producer
configuration is `none_or_defaulted` or unknown; seeded false is not non-belt proof.
A substituted belt key remains unknown even if a later duplicate restores a valid value.

Missing/unknown capture uses `binding:"unavailable"`, null `processor_offsets` and null
`start_custom_z`, never invented zero offsets. Captured belt/general-transform or failed
reload inputs can remain diagnostic values with unavailable normalization. Unavailable
normalization has null `kind` and one bounded reason: `not_captured`,
`configuration_unknown`, `unsupported_transform`, `invalid_slot_count`,
`unresolved_offsets`, `nonfinite_inputs`, `inputs_changed_during_processing`,
`invalid_filament_slot`, or `reload_failed_source_mismatch`. Belt true refuses even at zero
tilt; observed belt headers/general transforms also refuse ordinary normalization.

`frame_digest` hashes canonical explicit encodings of frame state, provenance, inputs and
source-file state. It is not a portable slice identity, geometry hash or exported-byte hash.
Geometry's `orcamcp-layer-geometry-v3` digest includes it, so frame-only changes invalidate
geometry cursors. Detail retains its existing result/shape fence and immutable-publication
contract; consumers must reject differing frame digests across joined pages. It does not
claim to detect arbitrary same-ID postpublication mutation server-side.

All metadata counts toward each complete 64 KiB success JSON limit. No fields/vectors are
omitted or truncated to fit; insufficient space returns `output_limit`. Frame hashing is
bounded by 64 slots and does not inspect moves or consume the detail move budget.

## Independent normalization

After reproducing G-code modal, G92 and unit semantics, normalize each ordinary machine
endpoint with its own resolved filament slot. Apply XY and Z offset in the native order:
cast the base `(machineX+offsetX, machineY+offsetY, machineZ-offsetZ)` to float32, then add
the filament's float32 vector in float32. Predecessor and current endpoints may use
different slots. Do not translate the native dummy as if it were a commanded motion.

Start-custom-code non-belt Z can instead be pinned to first-layer height plus filament Z.
Metadata declares the policy, not per-vertex pin windows. Controlled fixtures can reproduce
a known boundary; arbitrary unknown windows remain an evidence gap. Noninternal Seam
vertices remain synthetic native predecessors under the physical policy. Their loop-end
eligibility is less than 0.25 mm; either reproduce and label that native policy or report
its affected segment discrepancy. Do not silently drop/reconnect Seam segments or claim
those classes independently verified.

Retain exact export/parser hashes, source build/version, request/response hashes and result,
layer and frame identities. Export may reprocess and change result ID: capture again after
export rather than bind an older response by equal coordinates. Frame provenance alone does
not prove the exported bytes are the exact source of the result. Strict physical-coordinate
comparisons retain the 0.002 mm tolerance. At far origins where float32 ULP exceeds it,
report the strict row unavailable/failing with measured ULP; a separately labelled exact
float32-storage reconstruction may supplement it but never silently widens that tolerance.
