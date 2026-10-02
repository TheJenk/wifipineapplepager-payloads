# Wardrive HUD

A framebuffer **wardriving heads-up display** for the Pager: an aviation-style
HUD with a rotating **compass rose** + heading, a big **speed** readout, live
**GPS fix / satellite** status, a running count of **access points discovered**
(total + new this trip), **trip distance**, and **lat/lon**.

- **Author:** Jenx
- **Category:** reconnaissance

![HUD](preview.png)

## Controls
- **Left / Right** — cycle speed units (MPH / KMH / KN); distance follows (mi/km)
- **Green (A)** — reset the trip (zeroes "new this trip" APs and trip distance)
- **Red (B) / Power** — exit

## Requirements
A **USB GPS** and the gpsd tools (`gpspipe`). The payload **auto-installs** gpsd
the first time if it's missing (`opkg update && opkg install gpsd gpsd-clients`)
— needs the Pager to have internet (client mode) that one time; afterwards it
skips straight to running. It will start `gpsd` on a common USB GPS node
(`/dev/ttyUSB0`, `/dev/ttyACM0`, …) if one isn't already serving. Wi-Fi
discovery uses the tri-band (incl. 6E) radio on phy1 via a temporary scan vif and
does not disturb an active `wlan1mon`.

## How it works
Three background producers feed one `/tmp/wardrive.dat` that the native binary
renders at ~10fps:

- `gpspipe -w` → **awk** parses gpsd JSON (mode, lat/lon, speed, track, used
  sats) and accumulates trip distance (equirectangular, glitch-filtered).
- a **scan loop** adds a temporary managed vif on phy1, scans all bands, and
  tallies **unique BSSIDs** ever seen this session.
- a **composer** merges the two and applies the trip baselines (reset via a flag
  the binary drops when you press Green).

The binary is freestanding, integer-only (the MT7628 has no FPU), drawn upright
480×222 and rotated into the 222×480 framebuffer. It borrows and returns the
screen with the official `UI_TAKEOVER` / `UI_RELEASE` handoff.

> The HUD layout, compass, unit switching and counters were verified via the
> host preview (`-DHOST_PREVIEW`, synthetic GPS). Live GPS parsing and the gpsd
> device wiring depend on the specific USB GPS and are confirmed on-device.
