# Wardrive HUD

A framebuffer **wardriving heads-up display** for the Pager: an aviation-style
HUD with a rotating **compass rose** + heading, a big **speed** readout, live
**GPS fix / satellite** status, a running count of **access points logged**
(total + new this trip), **trip distance**, and **lat/lon**.

It drives the Pager's **built-in WiGLE logger** (stock recon / `pineapd`) for the
actual Wi-Fi capture, so there's **no radio conflict** and the log is written in
the stock, upload-ready format — the HUD is the display layer on top.

- **Author:** Jenx
- **Category:** reconnaissance

![HUD](preview.png)

## Controls
- **Left / Right** — cycle speed units (MPH / KMH / KN); distance follows (mi/km)
- **Green (A)** — reset the **trip meters** (zeroes "new this trip" APs + trip
  distance only; the session total and the WiGLE log keep going)
- **Red (B)** — exit (stops the WiGLE log)
- **Power** — display sleep (press again / any button to wake); the HUD keeps running

## WiGLE logging (stock)
On launch the payload runs **`WIGLE_START`**, which starts the Pager's built-in
WiGLE logger — `pineapd` does the Wi-Fi scanning and writes GPS-stamped rows to a
stock **WigleWifi-1.6 CSV** in `/root/loot/wigle/` (upload-ready). The HUD's
**ACCESS POINTS** counter is simply the row count of that CSV, so it tracks the
stock logger exactly. On exit the payload runs **`WIGLE_STOP`** and prints the log
path.

To send captures to **wigle.net**: run **`WIGLE_LOGIN`** once (stores an upload
token), then **`WIGLE_UPLOAD /root/loot/wigle/<file>.csv`**. The exit message
tells you the exact path (and whether a token is already stored).

Because the capture is the stock logger (not a second scanner), there is **no
radio contention** with recon/PineAP, and nothing to reconcile — one log, the
stock format, the stock uploader.

## Requirements
A **USB GPS** and the gpsd tools (`gpspipe`). The payload **auto-installs** gpsd
the first time if it's missing (`opkg update && opkg install gpsd gpsd-clients`)
— needs the Pager to have internet (client mode) that one time. gpsd is normally
already running (the stock firmware manages it); the payload only starts it (on a
common node like `/dev/ttyACM0`) if it isn't running at all. WiGLE rows are only
written once there's a GPS fix (mode ≥ 2D) — standard for wardriving.

## How it works
`WIGLE_START` kicks off the stock logger, then two background producers feed one
`/tmp/wardrive.dat` that the native binary renders at ~10fps:

- `gpspipe -w` → **awk** parses gpsd JSON (mode, lat/lon, speed, track, used
  sats) and accumulates trip distance (equirectangular, glitch-filtered).
- a **composer** reads the GPS state, counts rows in the stock WiGLE CSV for the
  AP total, and applies the trip baselines (reset via a flag the binary drops
  when you press Green).

The binary is freestanding, integer-only (the MT7628 has no FPU), drawn upright
480×222 and rotated into the 222×480 framebuffer. It borrows and returns the
screen with the official `UI_TAKEOVER` / `UI_RELEASE` handoff.

> The HUD layout, compass, unit switching and counters were verified via the host
> preview (`-DHOST_PREVIEW`, synthetic GPS). The stock `WIGLE_START`/`WIGLE_STOP`
> integration and the `/tmp/wardrive.dat` pipeline were verified on-device;
> `WIGLE_START` returns the log path and the composer tracks its row count. A live
> GPS fix (outdoors) is needed to see non-zero position/APs.
