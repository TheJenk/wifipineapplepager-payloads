#!/bin/bash
# Title: ADS-B Radar
# Author: Jenx
# Description: Live aircraft tracking from an RTL-SDR on 1090 MHz decoded on the Pager itself (no internet needed), or from a free internet ADS-B feed when no dongle is plugged in (frees the USB port for a GPS). Aircraft icons by type (airliner, heavy, light, helicopter, glider, balloon, drone...) and filters (ground, altitude, kind, emergencies). Hold Green to pan the map with the D-pad. MAP view with a built-in offline map of the whole US (states, coastline, highways, rivers, lakes, cities, airports with runways; zoom from runway to country scale) and RADAR view (scope + list), planes pointing along their track and coloured by altitude, with trails. Pick any plane for a full INFO card: registration, aircraft type (internet), country, squawk, altitude history and a mini-map of its track. Flashing banner for emergency squawks (7500/7600/7700). Logs positions to loot as CSV.
# Version: 0.3.0
# Category: reconnaissance

# ---------------------------------------------------------------------------
# Controls (MAP / RADAR):  Left/Right  pick a plane     Up/Down  zoom in / out
#                          Green (A)   plane INFO        Red (B)  menu
#                          hold Green on the MAP = PAN MODE
# Controls (PAN MODE):     D-pad move the map (hold to glide)   Green/Red zoom in/out
#                          hold Green = done (map stays put)    hold Red = re-centre
# Controls (INFO):         Left/Right  prev/next plane   Red (B)  back to the map/radar
# Menu: VIEW map/radar, LABELS callsign/altitude/off, CENTRE me-auto/selected plane,
#       filters GROUND show/hide, ALTITUDE all/<10k/>10k, KIND all/airliners/light&GA/
#       helicopters/military, EMERGENCY all/only; SOURCE radio (RTL-SDR) / internet, BACK, EXIT
#
# SOURCE
#   Radio    - an RTL-SDR on USB, decoded on the Pager. No internet needed.
#   Internet - planes within 60 NM from free community feeds (adsb.fi, then adsb.lol
#              as backup), every 10 s; planes glide along their track in between.
#              Needs the Pager on Wi-Fi with internet AND a position (GPS fix or
#              home.txt). Your position is sent rounded to 0.1 deg (about 11 km).
#   The choice is remembered (source.txt). With no RTL-SDR plugged in it starts on
#   Internet, so the USB port is free for a GPS receiver.
#
# Map: map.bin next to this file = whole US (lower 48, Alaska, Hawaii), ~800 KB,
# from Natural Earth + OurAirports (public domain). Without GPS or home.txt the
# map centres on the first plane heard (planes in range are always near you).
#
# Home position (optional, used when there is no GPS fix): put one line in
# home.txt next to this file:   39.7392 -104.9903      (decimal lat lon)
#
# Antenna: anything works close in; a 1090 MHz antenna (a 6.9 cm quarter-wave
# whip over a ground plane) reaches much farther.
#
# Pipeline: rtl_sdr streams 2 MS/s I/Q into the native `radar` binary, which
# demodulates Mode S, checks CRCs (with 1-bit repair), decodes CPR positions,
# draws the screen and writes the CSV log on stdout. In Internet mode the binary
# reads /tmp/adsb_net.json instead. Exit code 10 from the binary = switch source.
# ---------------------------------------------------------------------------

GAIN="${GAIN:-auto}"          # auto, or a tuner gain in dB such as 40.2 / 49.6
FEED_RADIUS_NM=60
FEED_INTERVAL=10             # seconds between downloads (planes glide in between)

DIR=""
for d in "${_PAYLOAD_HOME:-}" \
         "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)" \
         "/root/payloads/user/reconnaissance/ADSB_Radar"; do
    [ -n "$d" ] || continue
    [ -f "$d/radar" ] && { DIR="$d"; break; }
done
[ -z "$DIR" ] && { LOG red "radar binary not found. Reinstall the folder."; exit 1; }
BIN="$DIR/radar"
chmod +x "$BIN" 2>/dev/null

has_dongle() { grep -qsE "^(2838|2832)$" /sys/bus/usb/devices/*/idProduct; }
kill_rtl() { for p in $(pgrep -x rtl_sdr) $(pgrep -x rtl_tcp) $(pgrep -x rtl_power); do kill -9 "$p" 2>/dev/null; done; }
notice() { echo "$1" > /tmp/adsb_notice; }   # shown on screen for a few seconds by the binary

# source: remembered choice, but no dongle means internet
SRC_FILE="$DIR/source.txt"
SRC=$(cat "$SRC_FILE" 2>/dev/null)
[ "$SRC" = net ] || SRC=sdr
if [ "$SRC" = sdr ] && ! has_dongle; then
    SRC=net
    notice "NO RTL-SDR FOUND: USING INTERNET FEED"
fi
if [ "$SRC" = sdr ] && ! command -v rtl_sdr >/dev/null 2>&1; then
    LOG "rtl_sdr missing - installing rtl-sdr (needs internet once)..."
    opkg update >/tmp/adsb_opkg.log 2>&1 && opkg install rtl-sdr >>/tmp/adsb_opkg.log 2>&1
    command -v rtl_sdr >/dev/null 2>&1 || { LOG red "Install failed - see /tmp/adsb_opkg.log"; exit 1; }
fi

# free the dongle: DVB driver, a previous run, Pager SDR's rtl_tcp, a stray rtl_power
rmmod dvb_usb_rtl28xxu 2>/dev/null; rmmod rtl2832_sdr 2>/dev/null
kill_rtl

# reference position: GPS fix (mode 2/3) > home.txt (mode 1) > none (0)
REF=/tmp/adsb_ref
HOME_LINE=""
[ -f "$DIR/home.txt" ] && HOME_LINE=$(awk 'NF>=2 && $1+0==$1 {printf "1 %d %d", $1*1000000, $2*1000000; exit}' "$DIR/home.txt")
echo "${HOME_LINE:-0 0 0}" > "$REF"
gps_loop() {
    while :; do
        # /tmp/adsb_ref_pin (same format as $REF) overrides GPS/home for this session,
        # e.g. to record a demo somewhere other than where you are. Deleted on exit.
        if [ -f /tmp/adsb_ref_pin ]; then cp /tmp/adsb_ref_pin "$REF.tmp" && mv "$REF.tmp" "$REF"; sleep 2; continue; fi
        J=$(curl -s --max-time 2 --unix-socket /tmp/api.sock http://localhost/api/pineap/gps/get 2>/dev/null)
        L=$(echo "$J" | awk -v RS=',' -F: '
            /"fix"/ {f=$2+0} /"lat"/ {la=$2+0} /"lon"/ {lo=$2+0}
            END { if (f >= 2 && (la != 0 || lo != 0)) printf "%d %d %d", f, la*1000000, lo*1000000 }')
        if [ -n "$L" ]; then echo "$L" > "$REF.tmp" && mv "$REF.tmp" "$REF"
        else echo "${HOME_LINE:-0 0 0}" > "$REF.tmp" && mv "$REF.tmp" "$REF"; fi
        sleep 2
    done
}
gps_loop &
GPSPID=$!

# internet feed: planes around our (rounded) position every FEED_INTERVAL s.
# adsb.fi first (also gives aircraft description + operator), adsb.lol as backup.
# adsb.lol answers a rate-limited "point" request with an EMPTY list (HTTP 200), so an
# empty adsb.lol answer counts as a failure; a 429 / network error fails curl -f.
fetch_feed() {   # $1 lat $2 lon -> /tmp/adsb_net.tmp, prints feed name on success
    if curl -sf --connect-timeout 5 -m 12 -o /tmp/adsb_net.tmp \
            "https://opendata.adsb.fi/api/v2/lat/$1/lon/$2/dist/$FEED_RADIUS_NM" &&
       grep -q '"aircraft"' /tmp/adsb_net.tmp; then echo adsb.fi; return 0; fi
    if curl -sf --connect-timeout 5 -m 12 -o /tmp/adsb_net.tmp \
            "https://api.adsb.lol/v2/point/$1/$2/$FEED_RADIUS_NM" &&
       grep -q '"ac"' /tmp/adsb_net.tmp && ! grep -q '"total": *0[^0-9]' /tmp/adsb_net.tmp; then echo adsb.lol; return 0; fi
    return 1
}
net_loop() {
    while :; do
        read -r M LA LO < "$REF" 2>/dev/null
        if [ "${M:-0}" -ge 1 ] 2>/dev/null; then
            LAT=$(awk -v a="$LA" 'BEGIN { printf "%.1f", a / 1000000 }')
            LON=$(awk -v b="$LO" 'BEGIN { printf "%.1f", b / 1000000 }')
            if F=$(fetch_feed "$LAT" "$LON"); then
                mv /tmp/adsb_net.tmp /tmp/adsb_net.json
                echo "ok $(date +%s) $F" > /tmp/adsb_net.status
            else
                echo "err $(date +%s)" > /tmp/adsb_net.status
            fi
        else
            echo "nopos" > /tmp/adsb_net.status
        fi
        sleep "$FEED_INTERVAL"
    done
}
NETPID=""

rm -f /tmp/adsb_map.bin /tmp/adsb_ui /tmp/adsb_net.json /tmp/adsb_net.status
[ -f "$DIR/map.bin" ] && ln -s "$DIR/map.bin" /tmp/adsb_map.bin

LOOT=/root/loot/adsb
mkdir -p "$LOOT"
CSV="$LOOT/adsb_$(date +%Y%m%d-%H%M%S).csv"
echo "epoch,icao,callsign,alt_ft,lat,lon,speed_kt,track_deg,vrate_fpm,squawk" > "$CSV"

cleanup() {
    kill "$GPSPID" 2>/dev/null
    [ -n "$NETPID" ] && kill "$NETPID" 2>/dev/null
    rm -f /tmp/adsb_map.bin /tmp/adsb_net.json /tmp/adsb_net.tmp /tmp/adsb_net.status /tmp/adsb_ui /tmp/adsb_source /tmp/adsb_notice /tmp/adsb_ref_pin
    kill_rtl
    UI_RELEASE 2>/dev/null
    # keep the log only if it holds at least one position
    if [ -f "$CSV" ] && [ "$(wc -l < "$CSV")" -le 1 ]; then rm -f "$CSV"; fi
}
trap cleanup EXIT INT TERM

GOPT=""; [ "$GAIN" != "auto" ] && GOPT="-g $GAIN"

UI_TAKEOVER 2>/dev/null
while :; do
    echo "$SRC" > /tmp/adsb_source
    if [ "$SRC" = sdr ]; then
        kill_rtl
        # shellcheck disable=SC2086
        rtl_sdr -f 1090000000 -s 2000000 $GOPT - 2>/tmp/adsb_rtl.log | "$BIN" >> "$CSV"
        RC=${PIPESTATUS[1]}
        kill_rtl
    else
        net_loop &
        NETPID=$!
        "$BIN" < /dev/null >> "$CSV"
        RC=$?
        kill "$NETPID" 2>/dev/null; NETPID=""
    fi
    [ "$RC" = 10 ] || break                  # anything but "switch source" = exit
    if [ "$SRC" = sdr ]; then
        SRC=net
    elif ! has_dongle; then
        notice "NO RTL-SDR FOUND: PLUG ONE IN FIRST"
    elif ! command -v rtl_sdr >/dev/null 2>&1; then
        notice "RTL-SDR TOOLS MISSING: OPKG INSTALL RTL-SDR"
    else
        SRC=sdr
    fi
    echo "$SRC" > "$SRC_FILE"
done
exit 0
