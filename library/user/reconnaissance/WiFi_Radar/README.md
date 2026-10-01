# WiFi Radar

A **20 fps framebuffer radar scope** of the Wi-Fi around you. A rotating sweep
plots every nearby AP as a blip — the first graphical recon display for the Pager
(the stock UI can only print text).

- **Author:** Jenx
- **Version:** 1.0
- **Category:** reconnaissance

## What it shows

- A radar scope (rings + crosshairs) with a **rotating sweep** and afterglow trail.
- Each AP is a **blip**:
  - **angle** — stable per BSSID (same AP always sits in the same direction),
  - **radius** — signal strength (**stronger = closer to centre**),
  - **colour** — band: **green = 2.4 GHz**, **cyan = 5 GHz**,
  - blips **flash** (with a halo) as the sweep passes and **fade** when an AP
    drops out of range.
- **Count** of visible APs, top-left. **Band-filter** indicator dot, top-right.

## Controls

- **B (Green)** — cycle band filter: all → 2.4 GHz → 5 GHz
- **A (Red)** or **Power** — exit

## How it works

The slow part (a Wi-Fi scan takes ~1–3 s) runs in `payload.sh` in the background:
it `iwinfo scan`s `wlan0cli`/`wlan1cli`, parses each AP to `hash signal channel`
(hash = a stable value from the BSSID), and writes `/tmp/radar.dat`. The native
binary reads that file ~once a second and **renders at 20 fps**, so the sweep and
blip-fades stay smooth even though the data updates slowly.

Screen handoff uses the official **`UI_TAKEOVER`/`UI_RELEASE`**. The binary is
**freestanding** (no libc, raw MIPS syscalls, pure integer math — the MT7628 has
no FPU) and draws in upright 480×222 space, rotated into the 222×480 framebuffer.

## Safety / footprint

Read-only: it scans and draws. **Captures nothing, changes no settings, mounts
nothing.** Scanning briefly uses a client radio.

## Files

- `radar` — prebuilt static MIPS binary
- `radar.c` — full source (has a `-DHOST_PREVIEW` mode that renders frames to raw
  RGB565 for off-device preview)
- `build.sh` — cross-compile it

## Ideas for later

- Select a blip (d-pad) and show its SSID/BSSID/channel (needs a small font).
- Client/probe mode (toggle APs ↔ clients).
- Persist strongest-ever ring, or a "closest AP" callout.
