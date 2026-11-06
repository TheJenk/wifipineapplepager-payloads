# Wardrive HUD

A **wardriving heads-up display** for the Pager — and, at the same time, a
**Flock Safety ALPR hunter**. The ultimate take-it-on-the-road HUD: a scrolling
**compass heading tape**, a big **speed** readout, live **GPS fix / satellite**
status, a running count of **access points logged** (total + new this trip),
**trip distance**, and **lat/lon** — while it passively watches for Flock cameras
and **flashes a warning + plays an alert tone** the moment one is near.

> Built entirely on the **[Pager UI Kit](https://github.com/TheJenk/pager-ui-kit)**
> (`ui.sh` + the prebuilt `pguid`/`pgviewc` renderer) — the whole HUD is bash, no
> custom C.

It drives the Pager's **built-in WiGLE logger** (stock recon / `pineapd`) for the
actual Wi-Fi capture, so there's **no radio conflict** and the log is written in
the stock, upload-ready format. The HUD is the display layer on top, and the Flock
hunter listens passively on the same radios.

- **Author:** Jenx
- **Category:** reconnaissance

![HUD](preview.png)

## Pages
The HUD is split into four **pages**, each its own view file in `views/`, that you
flip through with the D-pad:

| Page | Shows |
|---|---|
| **DRIVE** | big speed + unit, heading, cardinal, GPS fix |
| **WARDRIVE** | APs logged, new this trip, trip distance, WiGLE on/off |
| **FLOCK** | Flock hits, the last detection, Wi-Fi + BLE source status |
| **GPS** | lat/lon, altitude, sats, accuracy, heading |

The **FLOCK DETECTED** banner overlays whatever page you're on, so you never miss
a camera no matter where you've flipped to.

## Controls
- **Left / Right** — flip between pages (DRIVE · WARDRIVE · FLOCK · GPS)
- **Up / Down** — cycle speed units (MPH / KMH / KN); distance follows (mi/km)
- **Green (A)** — reset the **trip meters** (zeroes "new this trip" APs + trip
  distance only; the session total and the WiGLE log keep going)
- **Red (B)** — exit (stops the WiGLE log, writes the Flock map, prints a summary)
- **Power** — display sleep (press again / any button to wake); the HUD keeps running

## Two jobs at once

**1. WiGLE wardriving (stock).** On launch the payload runs **`WIGLE_START`**, so
`pineapd` does the Wi-Fi scanning and writes GPS-stamped rows to a stock
**WigleWifi-1.6 CSV** in `/root/loot/wigle/` (upload-ready). The HUD's **APS
LOGGED** counter is the row count of that CSV. On exit it runs **`WIGLE_STOP`**.
To upload: **`WIGLE_LOGIN`** once, then **`WIGLE_UPLOAD /root/loot/wigle/<file>.csv`**.

**2. Flock hunting (passive).** At the same time it listens for **Flock Safety**
ALPR cameras and their accessories — their Wi-Fi SSIDs/OUIs and BLE signatures
(from [flock-you](https://github.com/colonelpanichacks/flock-you), MIT) — on
`pineapd`'s monitor interface(s) and BLE (`blue5`). Every hit is GPS-tagged and
written to its **own** session under `/root/loot/wardrive_hud_flock/<timestamp>/`:
a `wigle.csv`, a `hits.csv`, and a `flock.kml` map (built on exit). Listen-only.

When a high-confidence Flock device is detected the HUD **flashes a yellow
`FLOCK DETECTED` banner** and plays a short **alert tone** (`RINGTONE`, an homage
to the Metal Gear Solid alert — not the original sound). A persistent **`FLK n`**
tally in the top bar counts hits for the trip.

## Firmware
Works on **fw 2.0** and **1.1.x**. The important 2.0 change: **gpsd is off** on
2.0, so the HUD reads GPS from **`pineapd`'s GPS API** (`/api/pineap/gps/get`);
on 1.1.x it falls back to **gpsd** (`gpspipe`). Because that API reports no
heading, **course is derived from successive positions** (so the compass needs
you to be moving). BLE uses **`blue5`** on 2.0, BlueZ on 1.1.x.

## Requirements
A **USB GPS**. On 1.1.x the payload auto-installs gpsd if missing (needs internet
once). WiGLE/Flock rows get a position only once there's a fix (mode ≥ 2D) —
standard for wardriving; Flock hits with no fix are still logged (without a
position, so they're not placed on the map).

## How it works
Background producers write small state files; a **bash HUD loop** reads them and
draws with the UI Kit:

- One **GPS reader** polls the pineapd API (or gpsd) and writes both the HUD state
  (`/tmp/wd_gps`: mode, speed, heading, sats, lat/lon, trip) and the Flock fix
  (`/tmp/wd_fix`). Heading + trip distance are derived from successive positions
  (equirectangular, glitch-filtered).
- **`WIGLE_START`** runs the stock logger; the AP counts are that CSV's row count.
- **Flock matchers** (`tcpdump` + `blue5ctl` → awk) log hits to the separate loot
  session and push an alert line through a fifo; a sink bumps the `FLK` counter,
  stamps the time, and plays the tone.
- The **HUD loop** (`UI_BEGIN` … `UI_END` from `ui.sh`) sets the display variables
  each tick and `UI_RENDER`s the current page's `.view` (`views/drive.view`, etc.),
  so `\(sp)`, `\(total)`, `\(lat)` … are filled in from the shell. Left/Right swaps
  the page; the `FLOCK DETECTED` banner is drawn on top with the command API when a
  hit is live. `ui.sh` does the `UI_TAKEOVER`/`UI_RELEASE` handoff.

Because it's bash on the kit, there's **no compiler/toolchain** to build anything
— edit `payload.sh` and go.

### Speed units note
The pineapd GPS API reports speed in **m/s** (as gpsd does); the HUD converts to
MPH/KMH/KN. If your unit ever looks off by a constant factor, that's the field to
check.

> Verified on-device (fw 2.0): the HUD renders (status bar, compass tape, speed,
> AP odometer, trip, lat/lon), `WIGLE_START` logs, the GPS-API reader runs, and a
> **live** Flock injection raised the `FLOCK DETECTED` banner, bumped `FLK`, fired
> the tone, and wrote the separate Flock loot. A live GPS fix (outdoors) is needed
> for non-zero position/speed; a real Flock camera for a real detection. (The
> clean-exit teardown uses the same `pgrep`-based kills as the native version; it
> was hard to confirm end-to-end here because the test rig's USB-SSH link kept
> dropping — worth a real RED-button exit on a drive.)

## Credits
Flock signatures from **flock-you** by colonelpanichacks (MIT). This payload is
listen-only and logs to WiGLE/KML; it does not transmit.
