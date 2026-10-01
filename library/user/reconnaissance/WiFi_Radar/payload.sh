#!/bin/bash
# Title: WiFi Radar
# Author: Jenx
# Description: A 20fps framebuffer radar scope of nearby Wi-Fi APs. A rotating sweep plots each AP as a blip - angle is stable per BSSID, radius is signal (stronger = centre), colour is band (green 2.4 / cyan 5 GHz); blips flash as the sweep passes and fade with age. Up/Down select a blip, Red opens a recon detail card, B cycles band filter, Power exits. Read-only.
# Version: 1.2
# Category: reconnaissance

# ---------------------------------------------------------------------------
# Controls:  Up/Down = select a blip       Red (A) = open/close detail card
#            B (Green) = band filter       Power = exit
#
# The slow Wi-Fi scan runs here in the background (writes /tmp/radar.dat); the
# native binary just reads it and renders, which keeps the sweep smooth at the
# panel's 20fps. We use the official UI_TAKEOVER / UI_RELEASE handoff. Read-only:
# it scans and draws, captures nothing, changes no settings. Source + build.sh
# included; pure integer / freestanding (the MT7628 has no FPU).
# ---------------------------------------------------------------------------

BIN=""
for d in "${_PAYLOAD_HOME:-}" \
         "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)" \
         "/root/payloads/user/reconnaissance/WiFi_Radar"; do
    [ -n "$d" ] || continue
    [ -f "$d/radar" ] && { BIN="$d/radar"; break; }
done
[ -z "$BIN" ] && { LOG red "radar binary not found. Reinstall the WiFi_Radar folder."; exit 1; }
chmod +x "$BIN" 2>/dev/null

DAT=/tmp/radar.dat
STOP=/tmp/radar.stop
rm -f "$STOP"

# interfaces to scan
IFACES=""
for i in wlan0cli wlan1cli; do [ -e "/sys/class/net/$i" ] && IFACES="$IFACES $i"; done
[ -z "$IFACES" ] && IFACES="wlan0cli"

# parse an iwinfo scan into "hash signal channel" lines (hash = stable per BSSID)
parse_scan() {
    awk '
    function hv(c){ c=tolower(c); return index("0123456789abcdef",c)-1 }
    function flush(   aa,nn,jj,hh){
        if(addr==""){ return }
        aa=addr; gsub(":","",aa); nn=length(aa); hh=0
        if(nn>=4){ for(jj=nn-3;jj<=nn;jj++) hh=hh*16+hv(substr(aa,jj,1)); hh=hh%1024 }
        print hh, sig+0, ch+0, enc+0, addr, ess
    }
    /Address:/ { flush(); addr=$NF; ess=""; enc=0; sig=0; ch=0; fr=0 }
    /ESSID:/   { ess=$0; sub(/.*ESSID:[ ]*/,"",ess); gsub(/^"|"$/,"",ess)
                 if(ess=="unknown") ess="" }
    /Channel:/ { for(i=1;i<=NF;i++) if($i=="Channel:") ch=$(i+1) }
    /Encryption:/ { e=tolower($0)
                 if(e~/wpa3|sae/)enc=4; else if(e~/wpa2/)enc=3; else if(e~/wpa/)enc=2
                 else if(e~/wep/)enc=1; else if(e~/none|open/)enc=0; else enc=0 }
    /Signal:/  { for(i=1;i<=NF;i++) if($i=="Signal:") sig=$(i+1) }
    END { flush() }
    '
}

LOG "WiFi Radar"
LOG "B: band filter   Red: exit"
sleep 1

# background scanner: refresh the AP list ~every 2s
( while [ ! -f "$STOP" ]; do
    { for i in $IFACES; do iwinfo "$i" scan 2>/dev/null | parse_scan; done; } > "$DAT.tmp" 2>/dev/null
    mv "$DAT.tmp" "$DAT" 2>/dev/null
  done ) &
SCANPID=$!

cleanup() { touch "$STOP" 2>/dev/null; kill "$SCANPID" 2>/dev/null; UI_RELEASE 2>/dev/null; rm -f "$DAT" "$STOP" 2>/dev/null; }
trap cleanup EXIT INT TERM

UI_TAKEOVER 2>/dev/null
"$BIN"
