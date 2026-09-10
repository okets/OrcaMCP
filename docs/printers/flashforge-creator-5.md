# FlashForge Creator 5 and Creator 5 Pro

**First-class support, not a compatibility layer.**

The Creator 5 and Creator 5 Pro are excellent four-head toolchangers, and they
deserve a slicer that keeps up with them. This is a deliberate, maintained
integration built for these two machines specifically — a live device console
designed around what the printer actually exposes, four-tool workflows, colour
mixing, and material mapping that understands the material station.

It is built on current OrcaSlicer. Vendor slicers are forks, and forks fall
behind: features that land upstream reach you whenever the fork next merges. Here,
upstream *is* the base. Tree supports, adaptive layer heights, the calibration
suite, per-object settings, the modern multi-material work — you get them as
OrcaSlicer ships them, on a Creator 5.

**What you get beyond a stock slicer**

- A device console written for this printer, not a generic panel with the wrong
  logos on it. Four nozzles, four material slots in their real colours, live job
  progress, the camera inline.
- No cloud account, and no closed network plugin. It speaks the printer's own
  local HTTP API on port 8898, on your network, and nothing leaves it.
- Colour mixing with an assistant that can do the arithmetic: name a colour and it
  works out which loaded filaments to combine, or generates a palette to choose
  from.
- A project that can match itself to the machine — read what is actually loaded in
  the four slots and set the presets and colours to match, before you slice
  something in the wrong material.
- Every one of these reachable by an AI assistant through the MCP server, which is
  what the rest of this project is about.

Honesty matters more than a feature list, so this page also says plainly what the
printer's firmware will *not* let any slicer do — see
[What you cannot control, and why](#what-you-cannot-control-and-why). Nothing here
ships a button that does nothing.

## Who maintains this, and why it will keep working

I maintain OrcaMCP, and I own a Creator 5 Pro. This is the slicer I print with —
not a side project I tested once against someone else's machine and moved on from.
When something about the Creator 5 support is broken or awkward, I hit it on my own
prints, which is the difference between support that decays and support that gets
fixed.

So the commitment is simple and I have every reason to keep it: I will keep
maintaining this slicer, keep the FlashForge-specific support working, and keep
tracking upstream OrcaSlicer releases so these printers get new Orca features
without waiting for a vendor fork to catch up.

Bug reports from other Creator 5 owners are the fastest way to make this better. If
something is wrong or missing, open an issue — a machine in someone else's room
finds things mine never will.

---

## The machines

Both are four-head toolchangers with a four-slot material station. OrcaMCP talks to
them over the printer's own local HTTP API on port 8898 — no cloud account, and no
FlashPrint or Orca-Flashforge network plugin.

Everything specific to the Creator 5 family lives on this page. The rest of the
documentation stays printer-agnostic.

---

## Connecting

You need two things from the printer itself, both shown on its touchscreen under
the network or LAN-mode screen: its **serial number** and its **check code**
(sometimes labelled access code).

In OrcaMCP, click the connection button — the signal icon on the printer title bar
in the sidebar, tooltip "Connection" — to open the printer connection dialog. Set
the host type to FlashForge and fill in:

| Dialog field | What goes in it |
|--------------|-----------------|
| Hostname / IP | The printer's IP address |
| **Serial Number** | The printer's serial number |
| **API Key / Password** | The check code |

The serial number field only appears once the host type is FlashForge, and both
fields are **Advanced-mode settings** — switch out of Simple mode or you will not
see them.

Give the printer a static address or a DHCP reservation: the console reconnects by
address, not by discovery.

Internally these are the printer preset's `flashforge_serial_number` and
`printhost_apikey` keys, so an assistant can set them with `apply_config` and read
them back with `get_edited_presets`.

> **LAN mode has to be on.** If the printer drops off after a network change, a
> full power cycle is usually needed — toggling LAN mode on the screen alone often
> does not bring the HTTP API back.

The printer can also be found by UDP broadcast (port 48899, replies on 18007);
`discover_printers` uses this.

---

## The device console

Selecting a Creator 5 replaces the Device tab with a purpose-built console for
these machines: live temperatures for all four nozzles plus bed and chamber, the
four material slots with their real colours, job progress, and the printer's own
camera inline.

Polling adapts to what the machine is doing — roughly every second while a job is
loading, every two seconds while printing, every five when idle — and stops
entirely when you switch away from the tab.

### What you can control

- Chamber light
- Nozzle, bed and chamber target temperatures (per nozzle)
- Pause, resume and stop a running job
- Print speed (while a job is running)
- Air filtration: internal circulation and external exhaust
- Z offset
- Material slot contents: the material type and colour of each of the four slots

### What you cannot control, and why

These are firmware limits, not gaps in OrcaMCP. The console does not show dead
buttons for them.

- **Motion — jogging, homing, manual extrusion.** The Creator 5 exposes no move
  command over the HTTP API and has no separate motion channel. FlashForge's own
  software has no jog controls for it either.
- **Loading and unloading filament.** The `ms_cmd` command that does this is
  specific to the AD5X. Load *progress* is reported, though, so when you push
  filament in at the machine the console follows it through heating, pushing,
  purging, cutting and retracting.

One firmware quirk worth knowing: **while the printer is idle, the filtration and
Z-offset commands answer "Success" and then do nothing.** The console sends them
anyway — clearing fumes after a print is exactly when you want the fans — and
reports that the value did not change if the printer ignored it.

---

## Printing to it

The normal flow works: slice, then `send_to_printer`. The job is uploaded straight
to the printer over the local API and can be started in the same call.

### Material mapping

Each tool in your project has to be fed by a material-station slot. OrcaMCP matches
them automatically: within the same material family, it picks the slot whose colour
is closest to the project filament's, and reports how far off each match was as
`color_delta_e`, so an assistant can warn you before a print rather than after.

A mapping is only sent if every tool got a slot and every slot named actually holds
filament — a half-mapped job never reaches the printer.

You can override it by passing explicit `{tool_id, slot_id}` pairs.

### Matching the project to what is loaded

`match_project_to_printer` reads what is actually in the four slots and updates the
project to match: for each loaded slot it selects the right filament preset,
preferring the vendor profile for this exact model (`Flashforge PETG Pro @FF C5P`)
over a generic one, and sets the project's filament colour from the slot's reported
colour.

This is worth running before slicing. Without it a project can happily claim
magenta PLA while the machine holds bronze PETG, which both blocks the send on a
material mismatch and makes the plate preview lie about what you are getting. The
console offers it as a dismissible suggestion when it notices the two disagree.

---

## Model differences

The two machines are told apart by product id — 40 is the Creator 5, 41 the
Creator 5 Pro — and the console shows the right machine photo for each. There is
no model picker: it reads the printer.

---

## Known limitations

- The camera is shown in the console. It is not available through the Bambu-style
  live-view player, which speaks a protocol these printers do not.
- Print-speed changes only apply while a job is running.
- Z-offset granularity cannot be determined while the printer is idle, because the
  printer accepts the command and does not move.
