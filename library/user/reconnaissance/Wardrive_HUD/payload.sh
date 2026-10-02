#!/bin/bash
# Title: Wardrive HUD
# Author: Jenx
# Description: A framebuffer wardriving heads-up display - rotating compass + heading, big speed, GPS fix/sat status, a live count of access points discovered (total + new this trip), trip distance and lat/lon. Reads a GPS via gpsd and scans Wi-Fi in the background. Left/Right change speed units, Green (A) resets the trip, Red (B) exits. Needs a USB GPS.
# Version: 1.0
# Category: reconnaissance

# ---------------------------------------------------------------------------
# Controls:  Left/Right = speed units (mph/kmh/kn)   Green (A) = reset trip
#            Red (B) / Power = exit
#
# Architecture (all integer-only native rendering; the Pager has no FPU):
#   * gpspipe -w  -> awk  : parses gpsd JSON (mode/lat/lon/speed/track/sats),
#                           accumulates trip distance, writes /tmp/wd_gps.
#   * scan loop            : adds a temp managed vif on phy1 (all bands incl 6E),
#                           tallies UNIQUE BSSIDs seen -> /tmp/wd_total.
#   * compose loop         : merges GPS + AP totals, applies trip baselines,
#                           writes /tmp/wardrive.dat for the renderer.
#   * native binary        : renders the HUD at ~10fps, handles the buttons.
# Uses UI_TAKEOVER / UI_RELEASE to borrow and return the screen.
#
# Needs a USB GPS and the gpsd tools (gpspipe). On OpenWrt:
#   opkg update && opkg install gpsd gpsd-clients
# ---------------------------------------------------------------------------

BIN=""
for d in "${_PAYLOAD_HOME:-}" \
         "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)" \
         "/root/payloads/user/reconnaissance/Wardrive_HUD"; do
    [ -n "$d" ] || continue
    [ -f "$d/wardrive" ] && { BIN="$d/wardrive"; break; }
done
[ -z "$BIN" ] && { LOG red "wardrive binary not found. Reinstall the folder."; exit 1; }
chmod +x "$BIN" 2>/dev/null

# Ensure gpsd tools present; auto-install via opkg if missing (needs internet
# once). If already installed this is a no-op.
ensure_gps() {
    command -v gpspipe >/dev/null 2>&1 && return 0
    LOG "gpspipe missing - installing gpsd..."
    if ! ping -c 1 -W 3 8.8.8.8 >/dev/null 2>&1 && ! ping -c 1 -W 3 1.1.1.1 >/dev/null 2>&1; then
        LOG red "No internet. Connect the Pager to a network"
        LOG red "(client mode) with internet, then retry."
        return 1
    fi
    opkg update >/tmp/wd_opkg.log 2>&1
    opkg install gpsd gpsd-clients >>/tmp/wd_opkg.log 2>&1
    command -v gpspipe >/dev/null 2>&1 && { LOG green "gpsd installed."; return 0; }
    LOG red "Install failed - see /tmp/wd_opkg.log"
    return 1
}
ensure_gps || exit 1

# Start gpsd on a common USB GPS node if it isn't already serving.
if ! gpspipe -w -n 1 >/dev/null 2>&1; then
    for dev in /dev/ttyUSB0 /dev/ttyACM0 /dev/ttyUSB1 /dev/ttyACM1; do
        [ -e "$dev" ] && { gpsd -n "$dev" 2>/dev/null; sleep 1; break; }
    done
fi

DAT=/tmp/wardrive.dat
STOP=/tmp/wd.stop
RESET=/tmp/wardrive.reset
rm -f "$STOP" "$RESET" /tmp/wd_gps /tmp/wd_total /tmp/wd_seen
echo "0 0 0 0 0 0 0" > /tmp/wd_gps
echo 0 > /tmp/wd_total
: > /tmp/wd_seen

LOG "Wardrive HUD"
LOG "L/R units  GRN reset  RED exit"
sleep 1

# --- GPS reader: gpsd JSON -> /tmp/wd_gps ("mode spd10 hdg sats late5 lone5 distcum")
( gpspipe -w 2>/dev/null | awk '
    function jflt(k,  re,s){ re="\""k"\":-?[0-9]+(\\.[0-9]+)?";
        if(match($0,re)){ s=substr($0,RSTART,RLENGTH); sub(/^.*:/,"",s); return s+0 } return -9999 }
    function jint(k,  v){ v=jflt(k); return (v==-9999)?-9999:int(v) }
    function emit(mo,sp,hd,sa,le,lo,ds){ print mo,sp,hd,sa,le,lo,ds > F; close(F) }
    BEGIN{ F="/tmp/wd_gps"; PI=3.14159265; R=6371000; cum=0; sats=0; plat=999; plon=999 }
    /"class":"SKY"/{ u=jint("uSat"); if(u<0)u=jint("nSat"); if(u>=0)sats=u }
    /"class":"TPV"/{
        mode=jint("mode"); if(mode<0)mode=0;
        sp=jflt("speed"); if(sp<0)sp=0; spd=int(sp*10+0.5);
        tr=jflt("track"); if(tr<0)tr=0; hdg=int(tr+0.5)%360;
        lat=jflt("lat"); lon=jflt("lon");
        if(mode>=2 && lat!=-9999 && lon!=-9999){
            if(plat!=999){ dlat=(lat-plat)*PI/180; dlon=(lon-plon)*PI/180; ml=(lat+plat)/2*PI/180;
                x=dlon*cos(ml); y=dlat; d=R*sqrt(x*x+y*y); if(d>0 && d<200) cum+=d }
            plat=lat; plon=lon;
            emit(mode,spd,hdg,sats,int(lat*100000),int(lon*100000),int(cum));
        } else emit(mode,spd,hdg,sats,0,0,int(cum));
    }
' ) &
GPSPID=$!

# --- Wi-Fi scanner: unique BSSID count -> /tmp/wd_total (all bands via temp vif)
( while [ ! -f "$STOP" ]; do
    iw phy phy1 interface add wlan1scan type managed >/dev/null 2>&1
    ip link set wlan1scan up >/dev/null 2>&1
    iw dev wlan1scan scan 2>/dev/null \
        | grep -oiE 'BSS [0-9a-f:]{17}' | awk '{print tolower($2)}' >> /tmp/wd_hits
    iw dev wlan1scan del >/dev/null 2>&1
    if [ -s /tmp/wd_hits ]; then
        cat /tmp/wd_seen /tmp/wd_hits 2>/dev/null | sort -u > /tmp/wd_seen.tmp
        mv /tmp/wd_seen.tmp /tmp/wd_seen; : > /tmp/wd_hits
    fi
    wc -l < /tmp/wd_seen | tr -d ' ' > /tmp/wd_total.tmp 2>/dev/null
    mv /tmp/wd_total.tmp /tmp/wd_total 2>/dev/null
    sleep 4
  done ) &
SCANPID=$!

# --- Composer: merge GPS + AP totals, apply trip baselines -> /tmp/wardrive.dat
( apbase=0; distbase=0
  while [ ! -f "$STOP" ]; do
    read mode spd hdg sats le lo cum < /tmp/wd_gps 2>/dev/null
    read total < /tmp/wd_total 2>/dev/null
    [ -z "$total" ] && total=0; [ -z "$cum" ] && cum=0
    if [ -f "$RESET" ]; then apbase=$total; distbase=$cum; rm -f "$RESET"; fi
    apnew=$((total - apbase)); [ "$apnew" -lt 0 ] && apnew=0
    dist=$((cum - distbase)); [ "$dist" -lt 0 ] && dist=0
    echo "$mode $spd $hdg $sats $le $lo $total $apnew $dist" > "$DAT.tmp"
    mv "$DAT.tmp" "$DAT" 2>/dev/null
    sleep 1
  done ) &
COMPPID=$!

cleanup() {
    touch "$STOP" 2>/dev/null
    kill "$GPSPID" "$SCANPID" "$COMPPID" 2>/dev/null
    pkill -f "gpspipe -w" 2>/dev/null
    iw dev wlan1scan del >/dev/null 2>&1
    UI_RELEASE 2>/dev/null
    rm -f "$DAT" "$STOP" "$RESET" /tmp/wd_gps /tmp/wd_total /tmp/wd_seen /tmp/wd_hits 2>/dev/null
}
trap cleanup EXIT INT TERM

UI_TAKEOVER 2>/dev/null
"$BIN"
