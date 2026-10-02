#!/bin/bash
# Title: WiFi Radar
# Author: Jenx
# Description: A 20fps framebuffer radar scope of nearby Wi-Fi APs. A rotating sweep plots each AP as a blip - angle is stable per BSSID, radius is signal (stronger = centre), colour is band (green 2.4 / cyan 5 GHz); blips flash as the sweep passes and fade with age. Up/Down select a blip, Red opens a recon detail card, B cycles band filter, Power exits. Read-only.
# Version: 1.6
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

# Scan interface. The 5/6 GHz radio (phy1) is a tri-band 6E part but is only
# exposed as a monitor iface (wlan1mon). We add a temporary managed vif on phy1
# to actively scan ALL bands (2.4/5/6) at once, and remove it on exit. If that
# fails, fall back to the 2.4 GHz client iface wlan0cli.
SCANIF=""
SCANVIF=""
if command -v iw >/dev/null 2>&1; then
    iw dev wlan1scan del >/dev/null 2>&1               # clear any stale one
    if iw phy phy1 interface add wlan1scan type managed >/dev/null 2>&1; then
        ip link set wlan1scan up >/dev/null 2>&1
        SCANIF="wlan1scan"; SCANVIF="wlan1scan"
    fi
fi
[ -z "$SCANIF" ] && { [ -e /sys/class/net/wlan0cli ] && SCANIF="wlan0cli" || SCANIF="wlan0cli"; }

# emits "hash signal channel enc band bssid ssid"  (band 0=2.4 1=5 2=6)

# Preferred: iw scan gives frequency -> correct band (5 & 6 GHz share channel #s).
parse_iw() {
    awk '
    function hv(c){ c=tolower(c); return index("0123456789abcdef",c)-1 }
    function flush(   aa,nn,jj,hh,band,chan,enc){
        if(bss==""){ return }
        if(freq<2500){ band=0; chan=(freq==2484)?14:int((freq-2407)/5) }
        else if(freq<5925){ band=1; chan=int((freq-5000)/5) }
        else { band=2; chan=int((freq-5950)/5) }
        enc=0; if(rsn){ enc=sae?4:3 } else if(wpa){ enc=2 } else if(priv){ enc=1 } else enc=0
        aa=bss; gsub(":","",aa); nn=length(aa); hh=0
        if(nn>=4){ for(jj=nn-3;jj<=nn;jj++) hh=hh*16+hv(substr(aa,jj,1)); hh=hh%1024 }
        print hh, sigint, chan, enc, band, bss, ssid
    }
    /^BSS /    { flush(); bss=$2; sub(/\(.*/,"",bss); freq=0;sigint=0;ssid="";rsn=0;wpa=0;sae=0;priv=0 }
    /freq:/    { for(i=1;i<=NF;i++) if($i=="freq:") freq=$(i+1)+0 }
    /signal:/  { for(i=1;i<=NF;i++) if($i=="signal:") sigint=int($(i+1)) }
    /SSID:/    { ssid=$0; sub(/.*SSID: ?/,"",ssid) }
    /capability:/ { if($0 ~ /Privacy/) priv=1 }
    /RSN:/     { rsn=1 }
    /WPA:/     { wpa=1 }
    /SAE/      { sae=1 }
    END { flush() }
    '
}

# Fallback: iwinfo scan (no frequency -> band guessed from channel, 2.4 vs 5 only).
parse_iwinfo() {
    awk '
    function hv(c){ c=tolower(c); return index("0123456789abcdef",c)-1 }
    function flush(   aa,nn,jj,hh,band){
        if(addr==""){ return }
        band=(ch<=14)?0:1
        aa=addr; gsub(":","",aa); nn=length(aa); hh=0
        if(nn>=4){ for(jj=nn-3;jj<=nn;jj++) hh=hh*16+hv(substr(aa,jj,1)); hh=hh%1024 }
        print hh, sig+0, ch+0, enc+0, band, addr, ess
    }
    /Address:/ { flush(); addr=$NF; ess=""; enc=0; sig=0; ch=0 }
    /ESSID:/   { ess=$0; sub(/.*ESSID:[ ]*/,"",ess); gsub(/^"|"$/,"",ess); if(ess=="unknown") ess="" }
    /Channel:/ { for(i=1;i<=NF;i++) if($i=="Channel:") ch=$(i+1) }
    /Encryption:/ { e=tolower($0)
                 if(e~/wpa3|sae/)enc=4; else if(e~/wpa2/)enc=3; else if(e~/wpa/)enc=2
                 else if(e~/wep/)enc=1; else enc=0 }
    /Signal:/  { for(i=1;i<=NF;i++) if($i=="Signal:") sig=$(i+1) }
    END { flush() }
    '
}

HAVE_IW=0; command -v iw >/dev/null 2>&1 && HAVE_IW=1

LOG "WiFi Radar"
LOG "UP/DN select  Red details  B band  Power exit"
sleep 1

# background scanner: refresh the AP list every cycle
( while [ ! -f "$STOP" ]; do
    if [ "$HAVE_IW" -eq 1 ]; then iw dev "$SCANIF" scan 2>/dev/null | parse_iw > "$DAT.tmp" 2>/dev/null
    else iwinfo "$SCANIF" scan 2>/dev/null | parse_iwinfo > "$DAT.tmp" 2>/dev/null; fi
    mv "$DAT.tmp" "$DAT" 2>/dev/null
  done ) &
SCANPID=$!

cleanup() {
    touch "$STOP" 2>/dev/null; kill "$SCANPID" 2>/dev/null
    [ -n "$SCANVIF" ] && { ip link set "$SCANVIF" down 2>/dev/null; iw dev "$SCANVIF" del 2>/dev/null; }
    UI_RELEASE 2>/dev/null; rm -f "$DAT" "$STOP" 2>/dev/null
}
trap cleanup EXIT INT TERM

UI_TAKEOVER 2>/dev/null
"$BIN"
