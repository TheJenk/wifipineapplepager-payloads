# Field Marker

Tag GPS waypoints by label with the Pager's D-pad. Make your own labels, assign
them to the four buttons, then on an engagement just tap a button when you see
something — it drops a GPS-tagged, labeled pin. Afterwards you get a map of every
finding by category.

Self-contained: works on a stock **firmware 2.0** Pager with no other payloads or
tools installed.

- Author: Jenx
- Category: user/general
- Firmware: 2.0.0

## How it works

The payload opens a small menu. From there you:

1. **New label** — type a label on the Pager (e.g. "Security camera", "Open door").
2. **Assign buttons** — pick which label each D-pad direction (Up / Down / Left /
   Right) drops.
3. **Start tagging** — the screen turns into a four-way compass showing your four
   labels with live counts. **Tap a direction** and it records that observation:
   it reads the GPS fix and writes a waypoint. Red exits back to the menu.
4. **Export GPX/KML** — turn the recorded markers into map files.

The tag screen is a tiny native binary (`fieldtag`, ~7 KB, freestanding MIPS). A
bash helper (`recorder.sh`) runs alongside it, reads each tap, grabs the GPS fix
from PineAP's API, and writes the waypoint — so a tap is instant and the heavy
lifting stays out of the on-screen view.

## Where things are saved

Everything lives under `/root/loot/FieldMarker/` (loot survives firmware updates):

- `labels.txt` — your label library (one per line).
- `slots.conf` — the current Up/Down/Left/Right assignment.
- `tags-YYYYMMDD.csv` — the raw markers: `epoch,time,label,fix,lat,lon,alt`.
- `tags-YYYYMMDD.gpx` / `.kml` — written by **Export**, open in any map tool
  (Google Earth, gpx.studio, QGIS…).

A marker tapped with **no GPS fix** is still counted and logged in the CSV (so you
don't lose the observation), but it has no coordinates, so it's skipped in the
GPX/KML.

**Confirmation is silent** — a tag gives a short vibrate (haptic), never a sound,
so marking a target in the field makes no noise. One short buzz means it was
logged with a GPS fix; a double buzz means it was logged but has no fix yet. The
on-screen label also flashes and its count ticks up.

## Controls

**Menu:** Up/Down to move, Green to choose.
**Tag screen:** Up/Down/Left/Right = drop that label's pin · Red = exit.
Power is left to the Pager (display sleep).

## Install

Copy the `Field_Marker` folder to `/root/payloads/user/general/` on the Pager
(via SSH/SCP or the Virtual Pager Files page), then launch it from
**Payloads › user › general › Field Marker**.

## Building `fieldtag`

`fieldtag` is prebuilt. To rebuild (reproducible):

```sh
# brew install messense/macos-cross-toolchains/mipsel-unknown-linux-gnu
bash build.sh
# UI preview on your computer:  cc -DHOST_PREVIEW -O2 -o fieldtag_host fieldtag.c && ./fieldtag_host OUTDIR
```

## Notes

- The compass labels shrink from large to small text automatically so longer
  labels still fit; very long labels are truncated on screen (the full label is
  still what's saved).
- Four quick-tag buttons (the D-pad). A/B are kept for menu/exit.
- Menu-launched payloads run from a `/tmp` copy, so the payload finds its own
  files via `$_PAYLOAD_HOME` / the install path rather than `$0`.
