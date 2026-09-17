# FlashForge Creator 5 / 5 Pro — LAN API specification

**Audience:** someone writing a program that talks to a Creator 5 Pro directly over the local
network, with no access to this codebase. The motivating case is a bridge that lets an
[Obico](https://www.obico.io/) server pause the printer when its failure-detection model fires,
since Obico ships agents for OctoPrint and Klipper/Moonraker and none for FlashForge.

Everything here was verified against a physical Creator 5 Pro on firmware **1.9.9** in LAN-only
mode. Where something is inferred rather than observed, it says so.

---

## 1. The protocol in one page

Plain HTTP, no TLS, no session, no cookies. Every request is a `POST` of a JSON object, and
**every request carries the credentials in its body** — there is no login step and no token to
refresh.

```
POST http://<printer-ip>:8898/<endpoint>
Content-Type: application/json

{"serialNumber": "<serial>", "checkCode": "<check code>", ...}
```

| Endpoint | Purpose |
|---|---|
| `detail` | Full machine status. The only one a watchdog needs to read. |
| `control` | Every command: pause, resume, cancel, light, temperatures, fans. |
| `gcodeList` | Names of the G-code files stored on the printer. |
| `printGcode` | Start a stored file printing. |

A 15-second client timeout is what this implementation uses and it has been sufficient.

### Credentials

- **`serialNumber`** — the machine's serial. 13 characters on this unit.
- **`checkCode`** — the LAN access code. 8 characters on this unit.

`checkCode` is a **credential**: it is the only thing standing between anything on your LAN and
full control of the printer, including heaters. Never log it, never commit it, never put it in a
crash report. Treat the whole channel as trusted-LAN-only — it is unencrypted HTTP and an
attacker on the same network can read the code off the wire.

### Success and failure

The response is a JSON object. Success is **not** signalled by the HTTP status alone — check the
body:

- The result code is in `code`, or in `err` when `code` is absent.
- **`0` means success.** Any other value is a failure.
- The human-readable reason is in `message`, or `msg`.

```json
{"code": 0, "message": "Success", "detail": { ... }}
```

For `detail`, the payload may be nested under a `detail` key or returned at the top level
depending on firmware. Accept both: use `body["detail"]` if it is an object, otherwise `body`.

### The integer-typing trap

**This is the single thing most likely to break a naive implementation.** The firmware is
inconsistent about how it types integers. The same logical field arrives as:

- a JSON number on one firmware,
- a JSON boolean on another,
- a JSON **string**, sometimes space-padded, on a third.

Fields observed to do this include `code`, `err`, `hasMatlStation` and `slotCnt`. Write one
permissive integer reader and route every integer through it:

```python
def as_int(value):
    """Number, bool or (padded) decimal string -> int. None if it is none of those."""
    if isinstance(value, bool):
        return 1 if value else 0
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, str):
        try:
            return int(value.strip(), 10)
        except ValueError:
            return None
    return None
```

Two things that are easy to get wrong here:

- The `bool` check must come **before** the `int` check. In Python `bool` subclasses `int`, so
  `isinstance(True, int)` is `True` and a boolean would otherwise fall into the integer branch.
- Let `int()` do the string parsing inside a `try`. Hand-rolled validation like
  `text.lstrip("+-").isdigit()` accepts `"+-5"` and then raises on the conversion.

Reject anything else rather than coercing it. The C++ original requires the numeric text to be
fully consumed, so `"5.0"` is *not* a valid integer — match that.

Apply the same tolerance everywhere else: a firmware revision you have not seen should cost you
the one field you cannot read, never the whole poll.

---

## 2. Reading status — `detail`

```
POST http://<ip>:8898/detail
{"serialNumber": "...", "checkCode": "..."}
```

### Fields a watchdog cares about

| Field | Type | Meaning |
|---|---|---|
| `status` | string | Machine state. Lowercase it. `ready` observed on an idle machine. |
| `printFileName` | string | File currently printing; empty when idle. |
| `printProgress` | number | **0.0 to 1.0**, not a percentage. Multiply by 100 to display. |
| `printDuration` | number | Seconds elapsed in the current job. |
| `estimatedTime` | number | **Not remaining time.** On firmware 1.9.9 this tracks `printDuration` to the second (observed 2026-09-18: 1020 elapsed → 1020, 1680 → 1680, on a ~9 h job). The earlier "seconds remaining" reading was inferred from an idle printer where every time field is 0. Derive remaining time instead: `printDuration × (1 − printProgress) ÷ printProgress`, and treat it as unknown below 2 % progress. OrcaMCP exposes the raw value as `firmware_estimated_s` and the derivation as `remaining_s`. |
| `printLayer` | number | Current layer. |
| `targetPrintLayer` | number | Total layers. |
| `errorCode` | string | Empty when healthy. A **hard fault** (thermal, etc.). |
| `cameraStreamUrl` | string | MJPEG stream URL, see §5. |
| `nozzleTemps` / `nozzleTargetTemps` | array of 4 numbers | Per-tool current / target °C. |
| `platTemp` / `platTargetTemp` | number | Bed current / target °C. |
| `chamberTemp` / `chamberTargetTemp` | number | Chamber current / target °C. |
| `doorStatus` | string | `open` / `close`. |
| `lightStatus` | string | `open` / `close`. |

### About `status`

Normalise by lowercasing. Verified on firmware 1.9.9 (2026-09-15), the machine reports these
strings — note the **short forms** for pause and cancel:

| `status` | Meaning |
|---|---|
| `ready` | Idle. |
| `printing` | A job has been accepted. **This includes the warm-up phase**, before anything is extruded: `printDuration` and `printLayer` stay `0` until the first layer starts. Use `printDuration > 0` (or `printLayer > 0`) to tell "warming up" from "really printing". |
| `pause` | Paused. **Not** `paused`. |
| `cancel` | Cancelled. **Not** `cancelled`. |

Map `pause` → `paused` and `cancel` → `cancelled` if the rest of your code speaks the long forms,
and keep accepting the long forms in case a future firmware changes. `completed`, `error`, `busy`
and `heating` were anticipated by this implementation but have **not** been observed; treat any
value you do not recognise as unknown and keep going — do not assume an unfamiliar state means
"not printing".

### Job control is ignored during warm-up

Observed 2026-09-15 on firmware 1.9.9, one occurrence plus one control: a `pause` sent while
`status` was `printing` with `printDuration = 0` (bed climbing to 110 °C) was **acknowledged with
`code: 0` and never happened** — the print went on to layer 3. The same payload sent ~20 s later,
once extruding, took effect within 16 s. So the reply code is an acknowledgement, not a
confirmation. Consequences for a bridge:

- Never fire-and-forget a job-control command. Send it, poll `detail`, and only treat it as done
  when `status` actually changed. Retry a few times; surface a real failure otherwise.
- During warm-up, either keep re-sending the pause until it takes (the flashforge-obico agent
  keeps it pending and re-issues it as soon as `printDuration` moves off zero), or tell the user
  the printer cannot be paused yet. "I changed my mind during warm-up" is exactly when people press
  pause, so this is worth handling deliberately.
- Whether the firmware *queues or drops* the command has not been settled by a deliberate
  reproduction; the observed behaviour (still printing at layer 3, 21 s later) says drop.

### There is no failure detection here

Worth stating plainly, because it is the reason an Obico bridge exists at all. The full `detail`
payload contains roughly fifty fields and **none of them report print-failure detection**. There is
a `lidar` flag set to `1`, but it is a capability flag — the hardware exists — not a detection
result; nothing reports what it concluded. `errorCode` covers hard faults only, not spaghetti,
layer shift, or a part coming off the plate.

Whether the machine's own inspection still runs locally in LAN mode and simply does not report over
this API is **not answerable from the network side**. Check the printer's own screen.

### A real response

Captured from the machine, idle. MAC redacted; two registration-code fields were empty on this
unit and are shown as such.

```json
{
  "status": "ready", "printFileName": "", "printProgress": 0.0,
  "printDuration": 0, "estimatedTime": 0.0, "printLayer": 0, "targetPrintLayer": 0,
  "errorCode": "", "doorStatus": "close", "lightStatus": "close",
  "model": "Creator 5 Pro", "name": "Creator 5 Pro", "firmwareVersion": "1.9.9",
  "ipAddr": "10.0.0.10", "macAddr": "<redacted>", "location": "Den",
  "measure": "256X256X256", "nozzleCnt": 4, "nozzleModel": "0.4mm;0.4mm;0.4mm;0.4mm",
  "nozzleStyle": 0, "nozzleTemps": [28, 29, 29, 29], "nozzleTargetTemps": [0, 0, 0, 0],
  "platTemp": 27, "platTargetTemp": 0, "chamberTemp": 27, "chamberTargetTemp": 0,
  "leftTemp": 29, "leftTargetTemp": 0, "rightTemp": 28, "rightTargetTemp": 0,
  "leftFilamentType": "", "rightFilamentType": "",
  "chamberFanSpeed": 0, "coolingFanSpeed": 0,
  "internalFanStatus": "close", "externalFanStatus": "close",
  "camera": 1, "cameraStreamUrl": "http://10.0.0.10:8080/?action=stream",
  "lidar": 1, "tvoc": 0, "remainingDiskSpace": 4.91,
  "cumulativePrintTime": 1314, "cumulativeFilament": 49.77,
  "currentPrintSpeed": 0, "printSpeedAdjust": 0.0, "zAxisCompensation": 0.0,
  "fillAmount": 0, "autoShutdown": "close", "autoShutdownTime": 0,
  "estimatedLeftLen": 0, "estimatedLeftWeight": 0.0,
  "estimatedRightLen": 0, "estimatedRightWeight": 0.0,
  "flashRegisterCode": "", "polarRegisterCode": "", "pid": 41,
  "matlStationInfo": {
    "currentLoadSlot": 0, "currentSlot": 0, "slotCnt": 4,
    "stateAction": 0, "stateStep": 0,
    "slotInfos": [
      {"slotId": 1, "hasFilament": false, "materialName": "",    "materialColor": ""},
      {"slotId": 2, "hasFilament": true,  "materialName": "ABS", "materialColor": "#8C8C89"},
      {"slotId": 3, "hasFilament": false, "materialName": "",    "materialColor": ""},
      {"slotId": 4, "hasFilament": false, "materialName": "",    "materialColor": ""}
    ]
  }
}
```

### Polling cadence

What this implementation uses, and what the machine tolerates without complaint:

| Situation | Interval |
|---|---|
| Printing, heating or busy | 2 s |
| Material load/unload in progress | 1 s |
| Idle | 5 s |

---

## 3. Commands — `control`

Every command is the same envelope with a different `cmd` and `args`:

```
POST http://<ip>:8898/control
{
  "serialNumber": "...",
  "checkCode": "...",
  "payload": {"cmd": "<command>", "args": { ... }}
}
```

### Job control — what a bridge actually needs

```json
{"cmd": "jobCtl_cmd", "args": {"jobID": "", "action": "pause"}}
{"cmd": "jobCtl_cmd", "args": {"jobID": "", "action": "continue"}}
{"cmd": "jobCtl_cmd", "args": {"jobID": "", "action": "cancel"}}
```

Note **`continue`**, not `resume`. `jobID` is sent as an **empty string** and the firmware applies
the command to the running job; that is what this implementation does and it works on 1.9.9.

### Other commands, for completeness

```json
{"cmd": "lightControl_cmd",   "args": {"status": "open"}}        // or "close"
{"cmd": "temperatureCtl_cmd", "args": { ... see below ... }}
```

`printerCtl_cmd` and `circulateCtl_cmd` exist for print speed / Z-offset and for the fan and
filtration switches. **Each carries every field it owns**, so you must read the current values out
of a fresh `detail` and resend the ones you are not changing — otherwise setting one silently
resets its siblings. A watchdog does not need either.

### Temperatures, and the "leave alone" sentinel

```json
{
  "platform": -200, "chamber": -200,
  "nozzles": [-200, -200, -200, -200],
  "rightNozzle": -200, "leftNozzle": -200
}
```

- **`-200` means "do not change this"**. It is a sentinel, not a temperature.
- **`0` means "turn this off"** — explicitly different from `-200`.
- `nozzles` is always padded to 4 entries.
- `rightNozzle` mirrors `nozzles[0]` for legacy single-nozzle firmware; `leftNozzle` is sent as the
  sentinel.

Verified on hardware: setting the bed while passing the sentinel for every nozzle leaves a nozzle
target already at 40 °C untouched.

---

## 4. Files — `gcodeList` and `printGcode`

Not needed for a failure-detection bridge; listed so you know they exist.

`gcodeList` takes the bare credentials payload and returns the stored file names. Accept both a
plain string element and a `{"gcodeFileName": ...}` object, and drop nameless entries rather than
failing the call.

`printGcode` starts a stored file and takes a file name, a bed-levelling flag, and an optional
material-mapping array pairing project tools to material-station slots.

---

## 5. The camera

`cameraStreamUrl` in `detail` — on this machine `http://<ip>:8080/?action=stream`, a plain MJPEG
stream served over the LAN with no cloud in the path and no credential of its own.

**It is single-client, and it has no snapshot action.** Verified 2026-09-15 on firmware 1.9.9,
three trials of three: while one client is streaming, a second connection to `?action=stream` is
reset by the printer within about 10 ms (`Connection reset by peer`, no HTTP response at all), and
`?action=snapshot` gets an empty reply even with no other client connected. So a bridge has to be
the **sole consumer** of this URL and re-serve frames onward to anything else that wants them; the
reference implementation is the [flashforge-obico](https://github.com/okets/flashforge-obico)
agent, which exposes `/cameras/<i>/stream` and `/cameras/<i>/snapshot` on port 8081 and advertises
those URLs to Obico in the printer's webcam list.

---

## 6. Notes for a pausing watchdog

Protocol aside, these are the judgement calls worth making deliberately.

- **Confirm before acting on a detection.** A false positive that pauses a 7-hour ABS print at hour
  6 is an expensive mistake. Requiring the model to fire on several consecutive frames costs
  seconds and removes most single-frame noise.
- **Pausing ABS is not free.** The head parks, that region stops being heated by fresh extrusion,
  and a long pause invites warping, a visible seam, or a part detaching in a chamber that is still
  hot. Pausing is the right *immediate* action, but treat it as "stop and fetch a human", and alert
  loudly — not as a state the print can sit in for an hour.
- **Make pause idempotent.** Re-sending `pause` to an already-paused machine should be harmless in
  your logic; do not let a retry loop turn into a cancel.
- **Read state back after commanding.** The printer is the authority. Send the pause, then poll
  `detail` and confirm `status` actually changed rather than trusting the command's return.
- **Decide about `cancel` deliberately.** Cancelling is unrecoverable and wastes the filament and
  hours already spent. Recommendation: never let the model trigger `cancel`. Pause and alert; let a
  human cancel.
- **Fail safe on a network drop.** Losing the printer mid-print is not itself a failure to act on.
  Distinguish "cannot reach the printer" from "the print is failing", and never pause on a poll
  timeout alone.
- **The check code again.** It is in every single request. Load it from the environment or a
  0600 file, keep it out of logs, and redact it from anything you send to the Obico server.

---

## 7. Where this came from

This implementation lives in OrcaMCP, a fork of OrcaSlicer with first-class Creator 5 support:

| File | What is in it |
|---|---|
| `src/slic3r/Utils/FlashforgeApi.{hpp,cpp}` | Pure parsing and payload construction. No GUI, no network — the reusable half. |
| `src/slic3r/Utils/Flashforge.{hpp,cpp}` | The HTTP calls and the print-host integration. Coupled to wxWidgets and the slicer's networking, so read it as a reference, not a library. |
| `tests/slic3rutils/test_flashforge_live.cpp` | The hardware test the facts above were verified with. |

The parsing layer is genuinely dependency-free and is the part worth reading closely. The transport
layer is 40 lines of HTTP in any language — reimplement it rather than trying to lift it out.
