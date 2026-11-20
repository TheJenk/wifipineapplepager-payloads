# ADS-B Radar

Live aircraft tracking on the WiFi Pineapple Pager. Planes are decoded **on the Pager itself** from an
RTL-SDR on 1090 MHz, or pulled from a free community ADS-B feed when you'd rather use the internet.
They're drawn on a built-in offline map of the whole US, on a radar scope, and on a full info card
for each plane.

> **Version 0.3.0, a work in progress.** It works and has been tested on a Pager running firmware
> 2.0.0, but expect changes. Feedback welcome.

## What you need

| | Radio (RTL-SDR) | Internet |
|---|---|---|
| Hardware | An RTL-SDR dongle (RTL2832U, USB ID `0bda:2838` or `0bda:2832`) and a 1090 MHz antenna | Nothing extra |
| Network | None. Works completely offline | The Pager in **client mode**, connected to Wi-Fi with internet access |
| Your position | Optional (see below) | **Required:** a GPS fix, or coordinates in `home.txt` |
| Software | `rtl-sdr` tools (installed automatically with `opkg` the first time, which needs internet once) | `curl` (already on the Pager) |

The Pager has one USB port, so you can't use a USB GPS and the RTL-SDR at the same time. That is a
good reason to use Internet mode when you want a GPS fix.

## Sources and how position works

Switch between the two from the menu (**Red → SOURCE**). The app restarts on the other source in about
a second, keeping your view and zoom. Your choice is remembered in `source.txt`. If no RTL-SDR is
plugged in, it starts on Internet automatically.

### Radio (RTL-SDR)

- Planes broadcast their **own** GPS positions on 1090 MHz, and the Pager decodes them. **You don't need
  a GPS or internet to see planes on the map.**
- **Without a GPS fix or `home.txt`** the map centres on the **first plane it hears**, since every
  plane you can receive is near you. Distance and bearing to planes are hidden, because there's
  nothing to measure from. The top bar shows `NO GPS`.
- **With a GPS fix or `home.txt`**, the map centres on you (white crosshair), distance and bearing
  are shown, and planes appear on the map **sooner**: one position message is enough, instead of
  waiting for a matching pair.
- The top bar shows `RX %` (share of the 2 MS/s stream processed; it should sit at 99-100%) and
  messages per second.

### Internet

- Downloads all planes within **60 NM** of your position every **10 s** from
  [adsb.fi](https://adsb.fi) (with [adsb.lol](https://adsb.lol) as a backup). Planes glide along
  their track between downloads.
- **Needs client mode:** connect the Pager to a Wi-Fi network with internet in its settings.
- **Needs a position**, either:
  - a **GPS fix** (anything that gives the Pager a fix works), or
  - your coordinates in **`home.txt`** (see below).

  Without one, the screen says `INTERNET MODE: PLUG IN GPS OR ADD HOME.TXT`.
- **Privacy:** your position is sent to the feed **rounded to 0.1°** (about 11 km).
- Adds details radio can't give you: **aircraft type** (e.g. `B738`), **registration**, and the
  **aircraft description and operator** (e.g. `AIRBUS A-321 / DELTA AIR LINES INC`).
- The feeds are free community services with rate limits. If they stop answering, the top bar says
  `FEEDS UNREACHABLE/BUSY`. Wait a bit before trying again.

### `home.txt`: a fixed home position

Put one line with decimal latitude and longitude in `home.txt` next to `payload.sh`:

```
39.7392 -104.9903
```

It's used whenever there's no GPS fix. A GPS fix always wins.

## Controls

| Screen | Left / Right | Up / Down | Green (A) | Red (B) |
|---|---|---|---|---|
| MAP | Previous / next plane | Zoom in / out | Open INFO for the selected plane · **hold: pan mode** | Menu |
| RADAR | Previous / next plane | Range closer / wider | Open INFO | Menu |
| INFO | Previous / next plane | – | Back | Back to the map or radar |
| Menu | – | Move | Choose | Close |

### Panning the map

**Hold Green** (about half a second) on the MAP to enter **pan mode**. You'll see an amber frame and
a crosshair.

| In pan mode | |
|---|---|
| D-pad | Move the map (a tap moves a quarter of the screen; hold to keep gliding) |
| Green / Red | Zoom in / out |
| Hold Green | Done. The map stays where you left it; the top right shows `PANNED` |
| Hold Red | Re-centre on you (or auto) and leave pan mode |

After you leave pan mode, all the normal controls work again. To jump back later, use
**Menu → CENTRE** (it reads `PANNED (RE-CENTRE)`). Taps on Green and Red now act when you let go,
so a tap and a hold can be told apart.

Planes are ordered nearest first. Planes on the ground (parked or taxiing) come last and are drawn
as small grey dots.

**Menu:**

| Item | Choices |
|---|---|
| VIEW | MAP or RADAR |
| LABELS | callsign, altitude (hundreds of feet) or off |
| CENTRE | you/auto, or **follow the selected plane**; after panning it shows `PANNED (RE-CENTRE)` |
| GROUND | SHOW or HIDE parked/taxiing aircraft |
| ALTITUDE | ALL, BELOW 10,000 FT or ABOVE 10,000 FT |
| KIND | ALL, AIRLINERS, LIGHT & GA, HELICOPTERS or MILITARY |
| EMERGENCY | ALL PLANES or ONLY those squawking 7500/7600/7700 |
| SOURCE | RADIO (RTL-SDR) or INTERNET |
| BACK | Close the menu |
| EXIT | Leave the app (tip: Up from the top of the menu jumps to EXIT) |

Filter items stay open so you can combine them. While any filter is on, the top bar shows
`FILTER …`. Filters are kept when you switch source.

## Screens

- **MAP**
  - planes over an offline map (states, coastline, lakes, rivers, highways, cities, airports and runways)
  - **icons by aircraft type**, pointing along each plane's track and coloured by altitude
    (orange = low, green = mid, blue = high, violet = very high):

    | Icon | Kind (ADS-B emitter category) |
    |---|---|
    | Airliner | Large (A3, A4), and smaller for small jets (A2) |
    | Big airliner | Heavy (A5) |
    | Straight-wing prop | Light (A1) |
    | Delta | High performance (A6) |
    | Helicopter with rotor disk | Rotorcraft (A7) |
    | Long thin wings | Glider (B1) and ultralight (B4) |
    | Balloon, drone (X), vehicle (square) | Balloon (B2), UAV (B6), surface vehicles (C1, C2) |
    | Arrow | Category not known yet |

    The category comes from the plane's own identification message, so icons work in both modes.
    Internet mode also recognises helicopters by type code and marks **military** aircraft.
  - dotted trails show the last couple of minutes
  - zoom runs from runway level (about 12 km across) to the whole country; detail fades in as you zoom
  - the bottom strip shows the selected plane
- **RADAR**
  - a north-up scope with range rings from 5 to 200 NM, and a nearest-first list
  - planes beyond the range show as ticks on the rim
- **INFO**
  - callsign, ICAO hex address, registration (also worked out from the address for US aircraft),
    aircraft type, country, size category or description and operator, a MILITARY tag, and the
    squawk code with its meaning
  - altitude with climb/descent rate, speed, track, and distance and bearing from you
  - an altitude-history graph and a mini-map of the plane's track
- **Emergencies:** a red banner flashes for squawk **7500** (hijack), **7600** (radio failure) and
  **7700** (emergency).

## Map pack

`map.bin` (about 800 KB) covers the lower 48, Alaska and Hawaii, plus the bordering parts of Canada
and Mexico. It's built from public-domain data:

- [Natural Earth](https://www.naturalearthdata.com/) 1:10m: state and province lines, coastline,
  lakes, rivers, roads, populated places
- [OurAirports](https://ourairports.com/data/): large and medium airports, and their runways

To build a pack for another region, download those datasets (GeoJSON from the
[natural-earth-vector](https://github.com/nvkelso/natural-earth-vector) repo, plus `airports.csv`
and `runways.csv`) into one folder, then run:

```sh
python3 tools/build_map.py --src <data folder> --lat <lat> --lon <lon> --radius <km> -o map.bin
python3 tools/build_map.py --src <data folder> --us -o map.bin     # the default US pack
```

## Loot

Positions are logged to `/root/loot/adsb/adsb_<date>-<time>.csv`, at most one row per plane every
5 s:

```
epoch,icao,callsign,alt_ft,lat,lon,speed_kt,track_deg,vrate_fpm,squawk
```

A session that logs no positions leaves no file behind.

## Antenna tips

- Even a bare SMA adapter picks up planes that are close by.
- A 1090 MHz antenna makes a big difference. A simple one is a quarter-wave whip about **6.9 cm**
  long over a ground plane.
- Dongles with a built-in 1090 MHz filter and amplifier hear the most.
- The default tuner gain is automatic. To change it, set `GAIN` at the top of `payload.sh`
  (e.g. `40.2` or `49.6`).

## Known limits (0.3.0)

- Decodes ADS-B extended squitter and Mode S replies (DF0/4/5/11/16/17/18/20/21). It does not do
  Mode A/C, MLAT or TIS-B.
- In Radio mode, aircraft type and operator aren't available; they come from Internet mode.
- Units are feet, knots and nautical miles only.
- One map pack at a time.

## How it works / building

The `radar` binary is a single, freestanding MIPS program (about 60 KB, no libc, whole-number math
only, because the Pager's SoC has no FPU). In Radio mode `payload.sh` pipes `rtl_sdr` (2 MS/s) into
it. The binary:

- finds Mode S preambles
- checks each message's CRC and repairs single-bit errors
- decodes CPR positions (globally from even/odd pairs, or locally against a known position)
- draws the screen and writes the CSV log

In Internet mode, `payload.sh` downloads the feed JSON and the binary reads it instead. At 2 MS/s
the decoder uses about a third of the CPU.

```sh
./build.sh                    # cross-compile src/ -> radar (needs a mipsel GCC; see the script)
python3 tools/gen_tables.py   # regenerate src/nl_table.h + src/trig_tables.h
```

`build.sh` also has a host-preview build for the Mac or Linux, which renders frames from a recording.

## Credits

Map data: Natural Earth and OurAirports (public domain). Internet traffic data from the adsb.fi and
adsb.lol community networks.
