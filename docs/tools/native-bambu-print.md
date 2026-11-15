# Native Bambu print submission

`start_bambu_print` reuses the current Orca plate's native packager and Bambu
`PrintJob`. The initial supported capability is a selected LAN-mode P1S with
both its installed and sliced nozzle reported as 0.4 mm. Existing
`send_to_printer` behavior and defaults are unchanged.

## Requests

Inspect with an explicit native printer ID from `get_printers` and a mapping
for every filament the selected plate actually uses:

```json
{"printer_id":"example-device","dry_run":true,"mappings":[{"project_filament":1,"ams_id":0,"slot_id":1}]}
```

`dry_run` defaults to `true`. It does not export a package, upload a file, select
or connect a printer, or send any printer command. A successful inspection
returns `slice_id`, `plate_index`, native mapping arrays and native telemetry
age. Filament numbers are one-based project positions. AMS and slot IDs are
zero-based: slot ID 1 means the second physical tray.

After the bed and plate are physically ready, call the same tool with
`dry_run:false`, `expected_slice_id` copied from inspection, and a unique
`request_id`. The optional native options are `bed_leveling` (default true)
and `timelapse` (default false). Flow/vibration calibration, layer inspection,
external-change assistance and nozzle-offset calibration are explicitly off;
PA manual mode is omitted using the native -1 sentinel.

```json
{"printer_id":"example-device","dry_run":false,"expected_slice_id":"inspection-result-id","request_id":"example-submission","bed_leveling":true,"timelapse":false,"mappings":[{"project_filament":1,"ams_id":0,"slot_id":1}]}
```

## Native admission and dispatch

Admission runs on the main thread. The selected printer's identity must equal
the explicit requested identity; the tool does not change selection. Orca must
have no modal dialog, active UI job, busy slicing/export/upload pipeline or
scheduled background update. The selected plate needs current valid slice data,
a printable instance, a valid native G-code file and the same passing G-code
check that enables the GUI's print action. Actual submission must still match
`expected_slice_id` at main-thread enqueue. Packaging is followed by a second
complete admission/mapping check, and the job pins its package and plate data.

The printer must report IDLE or FINISH without an error, calibration or upgrade.
It needs native access rights/configuration, normal SD storage and idle AMS.
Freshness uses a nonnegative age of at most 15 seconds from native
`MachineObject::last_push_time`, not the static device-list `is_online` row or
`last_update_time` (which selection and unrelated messages can refresh).
A complete actual LAN status/AMS snapshot must have been received after the
latest native offline/reset event (`msg:0`, or a legacy full LAN packet that
omits `msg`; `msg:1` never establishes readiness even when it carries all keys).
Reconstructed status deltas can keep that
same native snapshot current; they cannot establish it after reconnect.
Unknown readiness is refused.

Every mapped native tray must exist, contain filament, have ready information
and not be reading. The native material family and filament profile ID must
exactly match the project filament; there is no mismatch override. Mapping
arrays span the entire project filament list. Unused entries remain -1 in the
legacy array and {ams_id:255,slot_id:255} in the modern array. Metadata carries
native filament ID/type and source/target RGBA colors. P1S has no nozzle-rack
mapping.

The same `make_print_job_for_machine` factory configures both the normal send
dialog and the MCP job from native in-process credentials, connection flags and
storage state. `Plater::send_gcode` creates the package. The existing UI worker
runs `PrintJob` through `NetworkAgent`, Bambu's printer agent and its network
plugin. The admitted LAN branch invokes one native `start_local_print`; MCP
never enters the cloud local-to-cloud retry branch. The headless job suppresses
completion UI navigation, modal error handling and raw transport error strings.
No separate slicer or transport is implemented.

## Submission is separate from printing

Poll `get_bambu_print_submission` with `request_id`. `submission_state` is one
of preparing, queued, submitting, submitted, failed_before_submission or
uncertain. `submitted` means the native transport reported successful submission;
it is not proof of physical printing. A failure, cancellation or exception
after native submission began is conservatively `uncertain`. No automatic
resend occurs. `observed_printer` independently reports fresh native state,
with `correlated_to_submission:false`; it does not identify the job that
produced that state.

A repeated request ID returns its original record, even when new arguments
would differ. The dispatch tombstone is written before packaging/queueing and
retained after failures and exceptions. Both records and tombstones are scoped
to this Orca process. They are lost on restart; a timeout or lost response must
be reconciled with physical printer state before creating another request ID.
The API does not promise exactly-once delivery across restarts or inside the
native transport plugin.

## Capability discovery and verification

The full OrcaMCP connector discovers both tools through native `tools/list` and
the generated tools manifest. Its existing `notifications/tools/list_changed`
mechanism refreshes the selected instance's schema. Preparation-only clients
remain outside this print-start capability.

The source contract is covered by `tests/slic3rutils/test_mcp_bambu_print.cpp`
for refusal, material identity, unused filament positions and no-resend/dry-run
dispatch behavior. Registry tests hold public schemas to the native tool list.
These tests and compilation do not establish hardware operation. A runtime
consumer test must separately prove the installed build, dry-run response,
explicit physical readiness, one submission and observed printer progress.
