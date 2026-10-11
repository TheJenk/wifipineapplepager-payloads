# Shark Jack Controller

Control a **Hak5 Shark Jack Cable** from the WiFi Pineapple Pager — no laptop or
phone. Plug the Cable into the Pager's USB host port, switch it to **arming**,
and launch this payload. The Pager takes over its screen and shows a live status
panel read straight off the Cable's serial console.

Minimal-kit, authorized-engagement use: you own/are cleared to operate both
devices.

## What it does

Three screens, reached from the status panel:

**Status** — opens the Cable's serial console, syncs to the shell, and renders:

- **Port** — the CP2102 serial node in use (`/dev/ttyUSB0`)
- **Firmware** — the Cable's `/root/VERSION`
- **Switch / mode** — OFF / ARMING / ATTACK (from `SWITCH`)
- **Power** — battery/USB state
- **Active payload** — the title of the payload currently staged on the Cable
- **Ethernet link** — UP/DOWN on the Cable's attack port

**Payloads** — lists the Cable's payload library as `category/name` (read
straight off `/root/payload/library`, so it sidesteps the `LIST` basename
noise). Scroll with the D-pad, press Green to **arm** (`ACTIVATE`) the selected
payload. Arming only *stages* it — it runs when you flip the Cable switch to
attack mode.

**Live output** — passively reads the Cable's serial console and shows it on the
Pager. Arm a payload, open this view, flip the Cable switch to **ATTACK**, and
the payload's `SERIAL_WRITE` narration scrolls here as it runs — the same thing
you'd see tethered to a phone, with no laptop on-site. It does not inject
anything; it only reads.

**Loot** — lists the files under the Cable's `/root/loot`, with sizes. Pick one
and press Green to pull it off the Cable over serial (base64, so binary files
survive) into the Pager's `/root/loot/sharkjack/<path>`, where the Loot Browser
can open it. Text loot is shown inline with scroll; binary loot is saved and
flagged.

> The Cable attacks over its **own ethernet port**, not the Pager tether (that
> link reads `link:down` while on USB). The Pager supplies power + console: arm,
> watch, and pull loot. Connect the Cable's ethernet end to the target to run.

### Controls

| Screen | Button | Action |
|--------|--------|--------|
| Status | **Green** | Refresh (or retry the connection if it failed) |
| Status | **Down** | Open the payload list |
| Status | **Up** | Open the loot list |
| Status | **Right** | Open the live-output view |
| Status | **Red** | Exit |
| Payloads | **Up / Down** | Move selection |
| Payloads | **Green** | Arm (`ACTIVATE`) the selected payload |
| Payloads | **Red** | Back to status |
| Loot | **Up / Down** | Move selection |
| Loot | **Green** | Pull the selected file to the Pager + view it |
| Loot | **Red** | Back to status |
| Loot view | **Up / Down** | Scroll · **Red** back to the list |
| Live output | **Red** | Back to status (set the switch to ARMING first) |
| any | **Power** | Sleep the display |

## Requirements

- Shark Jack **Cable** (CP2102 USB-UART, `10c4:ea60`) in the Pager's USB host port
- Cable switch in the **arming** position (the serial root shell only exists there)
- Pager firmware 1.1.3 (24.10.1)

## How it talks to the Cable

The hard parts, all learned on hardware:

- **One USB host port, shared with GPS.** The Pager's `gpsd` is pinned to the
  same serial node. This tool stops `gpsd` while it holds the link and restarts
  it on exit, so GPS and the Cable take turns on the one port.
- **DTR-on-open resets the Cable.** The CP2102's DTR line reboots the Shark Jack
  when the port opens, so the tool opens the fd **once**, holds it for the whole
  session (`clocal -hupcl`), and syncs to the shell prompt (tolerating the reset
  boot banner) before issuing commands.
- **57600 8N1 raw** — not the conventional 115200.
- Commands are wrapped in `echo __B__; … ; echo __E__` markers so the echoed
  command line and prompt are stripped from the output reliably.

## Build

Cross-compiles to the Pager (MT7628, MIPS 24KEc, soft-float). Requires Go.

```sh
./build.sh      # or: make build
```

Produces `sharkjack-controller` (mipsle, soft-float, static, ~2.7 MB). The
committed binary is built this way; rebuild to verify its SHA-256:

```
5f47d854036d23757b10e518f5ce36e6f631e55b2a37fc73423e700373c1002e
```

## Debugging over SSH

Run the binary directly on the Pager (outside the framebuffer UI):

```sh
sharkjack-controller --probe   # one-shot text dump of the status fields + payload list
sharkjack-controller --raw     # unparsed serial output of a couple of commands + payload list
```

## Files

| File | Purpose |
|------|---------|
| `payload.sh` | Pager payload wrapper (UI takeover, locates + runs the binary) |
| `main.go` | Serial link state machine + framebuffer UI (status / payloads / loot / live output) |
| `font.go` | 5x7 bitmap font (shared look with the Loot Browser) |
| `Makefile`, `build.sh` | Reproducible MIPS cross-build |
| `sharkjack-controller` | Prebuilt device binary |
