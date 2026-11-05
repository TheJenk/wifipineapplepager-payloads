#!/bin/bash
# Title: Wardrive HUD
# Author: Jenx
# Description: The ultimate road HUD. A framebuffer wardriving heads-up display - rotating compass + heading, big speed, GPS fix/sat status, access points logged (total + new this trip), trip distance and lat/lon - driving the Pager's built-in WiGLE logger (stock recon, uploads to wigle.net). AT THE SAME TIME it passively hunts Flock Safety ALPR cameras, logging every hit to its own WiGLE CSV + KML map; on a detection it flashes a FLOCK DETECTED warning and plays an alert tone. Left/Right change speed units, Green (A) resets the trip, Red (B) exits. Needs a USB GPS. fw 2.0 + 1.1.x.
# Version: 3.0
# Category: reconnaissance

# ---------------------------------------------------------------------------
# Controls:  Left/Right = speed units (mph/kmh/kn)   Green (A) = reset trip
#            Red (B) = exit                          Power = display sleep
#
# Two jobs at once, no radio conflict:
#   * WiGLE wardriving  - WIGLE_START runs the STOCK logger; pineapd does the
#     Wi-Fi scanning + GPS stamping + CSV, upload-ready for wigle.net. The HUD's
#     AP count = rows in that CSV.
#   * Flock hunting     - passively listens on pineapd's monitor vif(s) + BLE
#     (blue5) for Flock ALPR Wi-Fi/BLE signatures (flock-you, MIT), tags each
#     with GPS, writes its OWN WiGLE CSV + KML under /root/loot. On a hit it
#     flashes FLOCK DETECTED and plays an alert tone (RINGTONE).
#
# GPS: fw 2.0 reads pineapd's GPS API (gpsd is off on 2.0); fw 1.1.x uses gpsd.
# Heading is course-over-ground derived from position (the API has no track).
# To upload the wardrive log: WIGLE_LOGIN once, then WIGLE_UPLOAD <logfile>.
# ---------------------------------------------------------------------------

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)"
BIN=""
for d in "${_PAYLOAD_HOME:-}" "$HERE" "/root/payloads/user/reconnaissance/Wardrive_HUD"; do
    [ -n "$d" ] && [ -f "$d/wardrive" ] && { BIN="$d/wardrive"; [ -z "$HERE" ] && HERE="$d"; break; }
done
[ -z "$BIN" ] && { LOG red "wardrive binary not found. Reinstall the folder."; exit 1; }
chmod +x "$BIN" 2>/dev/null
FLK="$HERE/flock"

# ---- Flock signatures (flock-you by colonelpanichacks, MIT; see README) ------
SSID_RE='flock|penguin|fs ext battery|pigvision'
OUI_FLOCK="b4:1e:52"
OUI_COMMUNITY="70:c9:4e 3c:91:80 d8:f3:bc 80:30:49 b8:35:32 14:5a:fc 74:4c:a1 08:3a:88
9c:2f:9d c0:35:32 94:08:53 e4:aa:ea f4:6a:dd e0:0a:f6 24:b2:b9 00:f4:8d d0:39:57 e8:d0:fc
e0:4f:43 b8:1e:a4 70:08:94 58:8e:81 ec:1b:bd 3c:71:bf 58:00:e3 90:35:ea 5c:93:a2 64:6e:69
48:27:ea a4:cf:12 14:b5:cd 82:6b:f2 00:03:7f"
BLE_MFG=0x09c8
ALERT_MIN=high        # fire the FLOCK DETECTED box/tone for: high | medium | low
LOG_MIN=medium        # log Flock hits at/above: high | medium | low
REPEAT=5              # min seconds between log rows for the same device
FALERT_SECS=20        # how long the FLOCK DETECTED box stays up after a hit

# Alert tone - a short, urgent "!" stab (homage to the Metal Gear Solid alert;
# not the original sound). Two quick high stabs, repeated.
MGS_RTTTL="flockalert:d=16,o=7,b=300:c,c,16p,c,c,8p,c,c,16p,c,c"

# ---- files -------------------------------------------------------------------
DAT=/tmp/wardrive.dat
STOP=/tmp/wd.stop
RESET=/tmp/wardrive.reset
GPSF=/tmp/wd_gps
FIX=/tmp/wd_fix
FIFO=/tmp/wd_flock.fifo
FHITS=/tmp/wd_flock_seen   # one line per alert-worthy hit -> the HUD counter
FALERT=/tmp/wd_flock_at    # epoch of the last alert -> drives the flashing box
rm -f "$STOP" "$RESET" "$GPSF" "$FIX" "$FHITS" "$FALERT" "$FIFO"
echo "0 0 0 0 0 0 0" > "$GPSF"; : > "$FHITS"; echo 0 > "$FALERT"
mkfifo "$FIFO" 2>/dev/null

# ---- GPS source: pineapd API (fw 2.x) or gpsd (fw 1.1.x) ---------------------
GPS_API=0
if curl -s --unix-socket /tmp/api.sock http://localhost/api/pineap/gps/get 2>/dev/null | grep -q '"connected"'; then
    GPS_API=1
else
    command -v gpspipe >/dev/null 2>&1 || {
        LOG "gpspipe missing - installing gpsd..."
        if ping -c1 -W3 1.1.1.1 >/dev/null 2>&1 || ping -c1 -W3 8.8.8.8 >/dev/null 2>&1; then
            opkg update >/tmp/wd_opkg.log 2>&1; opkg install gpsd gpsd-clients >>/tmp/wd_opkg.log 2>&1
        fi
    }
    if ! pgrep -x gpsd >/dev/null 2>&1; then
        for dev in /dev/ttyUSB0 /dev/ttyACM0 /dev/ttyUSB1 /dev/ttyACM1; do
            [ -e "$dev" ] && { gpsd -n "$dev" 2>/dev/null; sleep 1; break; }
        done
    fi
fi

# One GPS reader for both the HUD (-> $GPSF: "mode spd10 hdg sats late5 lone5 distcum")
# and Flock (-> $FIX: "lat lon alt acc date time uptime"). Heading + distance are
# derived from successive positions. Handles API JSON and gpsd TPV/SKY alike.
GPSAWK='
function jflt(k, re,s){ re="\""k"\":-?[0-9]+(\\.[0-9]+)?"; if(match($0,re)){ s=substr($0,RSTART,RLENGTH); sub(/^.*:/,"",s); return s+0 } return -9999 }
function jint(k, v){ v=jflt(k); return (v==-9999)?-9999:int(v) }
function up( u,a){ u="";getline u < "/proc/uptime"; close("/proc/uptime"); split(u,a," "); return int(a[1]) }
function emit(mode,spd,sats,lat,lon){
    if(mode>=2 && !(lat==0&&lon==0)){
        if(plat!=999){ dlat=(lat-plat)*PI/180; dlon=(lon-plon)*PI/180; ml=(lat+plat)/2*PI/180;
            x=dlon*cos(ml); y=dlat; d=R*sqrt(x*x+y*y);
            if(d>0&&d<200){ cum+=d; if(d>2){ brg=atan2(sin(dlon)*cos(lat*PI/180), cos(plat*PI/180)*sin(lat*PI/180)-sin(plat*PI/180)*cos(lat*PI/180)*cos(dlon)); hdg=((int(brg*180/PI+0.5))%360+360)%360 } } }
        plat=lat; plon=lon;
        print mode,spd,hdg,sats,int(lat*100000),int(lon*100000),int(cum) > G; close(G);
        print lat,lon,alt,acc,strftime("%Y-%m-%d %H:%M:%S",systime(),1),up() > FIXF; close(FIXF);
    } else { print mode,spd,hdg,sats,0,0,int(cum) > G; close(G) }
}
BEGIN{ PI=3.14159265; R=6371000; cum=0; sats=0; hdg=0; plat=999; plon=999; alt=0; acc=0 }
/"connected":/{
    fix=jint("fix"); if(fix<0)fix=0; if($0 !~ /"connected":true/)fix=0;
    sp=jflt("speed"); if(sp<0)sp=0; spd=int(sp*10+0.5);
    st=jint("sats_tracked"); if(st>=0)sats=st;
    alt=jflt("alt"); if(alt<0)alt=0; acc=jflt("hprecision"); if(acc<0)acc=0;
    lat=jflt("lat"); lon=jflt("lon");
    emit(fix,spd,sats,(lat==-9999?0:lat),(lon==-9999?0:lon)); next
}
/"class":"SKY"/{ u=jint("uSat"); if(u<0)u=jint("nSat"); if(u>=0)sats=u; next }
/"class":"TPV"/{
    mode=jint("mode"); if(mode<0)mode=0;
    sp=jflt("speed"); if(sp<0)sp=0; spd=int(sp*10+0.5);
    alt=jflt("altHAE"); if(alt<0)alt=jflt("alt"); if(alt<0)alt=0; acc=jflt("eph"); if(acc<0)acc=0;
    lat=jflt("lat"); lon=jflt("lon");
    emit(mode,spd,sats,(lat==-9999?0:lat),(lon==-9999?0:lon)); next
}'
if [ "$GPS_API" = 1 ]; then
    ( while [ ! -f "$STOP" ]; do curl -s --unix-socket /tmp/api.sock http://localhost/api/pineap/gps/get 2>/dev/null; echo; sleep 1; done \
        | awk -v G="$GPSF" -v FIXF="$FIX" "$GPSAWK" ) &
else
    ( while [ ! -f "$STOP" ]; do gpspipe -w 2>/dev/null; sleep 2; done \
        | awk -v G="$GPSF" -v FIXF="$FIX" "$GPSAWK" ) &
fi

# ---- start the STOCK WiGLE logger -------------------------------------------
WLOG="$(WIGLE_START 2>/tmp/wd_wigle.err)"
[ -n "$WLOG" ] && [ -f "$WLOG" ] || { WLOG=""; LOG red "WIGLE_START failed (see /tmp/wd_wigle.err)"; }

# ---- Flock detection -> its own loot session + the on-screen alert -----------
SES="/root/loot/wardrive_hud_flock/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$SES"
echo "time_utc,lat,lon,type,mac,name,rssi,confidence,reason" > "$SES/hits.csv"
FW=""; read -r FW _ < /etc/pineapplepager/version 2>/dev/null
{
  echo "WigleWifi-1.6,appRelease=1.1,model=WiFi Pineapple Pager,release=${FW},device=Pager,display=Flock Wardrive,board=MT7628,brand=Hak5,star=Sol,body=3,subBody=0"
  echo "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type"
} > "$SES/wigle.csv"
echo "$SES" > /tmp/wd_flock_ses

prog(){ cat "$FLK/lib.awk" "$FLK/$1"; }
AWKV=(-v FIXF="$FIX" -v HITS="$SES/hits.csv" -v WIG="$SES/wigle.csv" -v REPEAT="$REPEAT" -v ALERTMIN="$ALERT_MIN" -v LOGMIN="$LOG_MIN")

# sink: every alert-worthy hit -> bump the counter, stamp the time, play the tone
( while IFS= read -r line; do
    [ -z "$line" ] && continue
    echo "$line" >> "$FHITS"
    date +%s > "$FALERT"
    RINGTONE "$MGS_RTTTL" >/dev/null 2>&1 &
    logger -t Wardrive_HUD "$line" 2>/dev/null
  done ) < "$FIFO" &

# Wi-Fi: passive tcpdump on pineapd's existing monitor vif(s)
iw dev 2>/dev/null | awk '/Interface/{i=$2} /type monitor/{print i}' > /tmp/wd_ifaces
WIFI_OK=0
if command -v tcpdump >/dev/null; then
  while IFS= read -r IF; do
    [ -n "$IF" ] || continue; WIFI_OK=1
    ( while [ ! -f "$STOP" ]; do
        tcpdump -i "$IF" -e -n -l -s 256 type mgt 2>/dev/null \
          | awk "${AWKV[@]}" -v SSIDRE="$SSID_RE" -v OUI_FLOCK="$OUI_FLOCK" -v OUI_COMMUNITY="$(echo $OUI_COMMUNITY)" "$(prog wifi.awk)"
        sleep 1
      done > "$FIFO" ) &
  done < /tmp/wd_ifaces
fi

# BLE: blue5 (fw 2.x) or BlueZ (fw 1.1.x)
BLE_OK=0
if command -v blue5ctl >/dev/null && blue5ctl ping >/dev/null 2>&1; then
    BLE_OK=1
    ( while [ ! -f "$STOP" ]; do blue5ctl fetch -n 0 -g 3 2>/dev/null; sleep 2; done \
        | awk "${AWKV[@]}" -v MODE=blue5 -v BLE_MFG="$BLE_MFG" "$(prog ble.awk)" > "$FIFO" ) &
elif command -v bluetoothctl >/dev/null && bluetoothctl power on >/dev/null 2>&1; then
    BLE_OK=1
    ( while [ ! -f "$STOP" ]; do bluetoothctl --timeout 60 scan on 2>/dev/null; done \
        | awk "${AWKV[@]}" -v MODE=bluez -v BLE_MFG="$BLE_MFG" "$(prog ble.awk)" > "$FIFO" ) &
fi

# ---- composer: GPS + WiGLE count + Flock state -> $DAT (12 fields) -----------
( apbase=0; distbase=0
  while [ ! -f "$STOP" ]; do
    read mode spd hdg sats le lo cum < "$GPSF" 2>/dev/null; [ -z "$cum" ] && cum=0
    total=0
    if [ -n "$WLOG" ] && [ -f "$WLOG" ]; then
        n=$(wc -l < "$WLOG" 2>/dev/null | tr -d ' '); [ -z "$n" ] && n=0
        total=$((n>2 ? n-2 : 0))
    fi
    if [ -f "$RESET" ]; then apbase=$total; distbase=$cum; rm -f "$RESET"; fi
    apnew=$((total-apbase)); [ "$apnew" -lt 0 ] && apnew=0
    dist=$((cum-distbase)); [ "$dist" -lt 0 ] && dist=0
    wig=0; [ -n "$WLOG" ] && [ -f "$WLOG" ] && wig=1
    flk=$(wc -l < "$FHITS" 2>/dev/null | tr -d ' '); [ -z "$flk" ] && flk=0
    at=$(cat "$FALERT" 2>/dev/null); [ -z "$at" ] && at=0
    now=$(date +%s); falert=0
    [ "$at" -gt 0 ] && [ $((now-at)) -lt "$FALERT_SECS" ] && falert=$((FALERT_SECS-(now-at)))
    echo "${mode:-0} ${spd:-0} ${hdg:-0} ${sats:-0} ${le:-0} ${lo:-0} $total $apnew $dist $wig $flk $falert" > "$DAT.tmp"
    mv "$DAT.tmp" "$DAT" 2>/dev/null
    sleep 1
  done ) &

LOG "Wardrive HUD + Flock"
[ -n "$WLOG" ] && LOG green "WiGLE -> $WLOG" || LOG red "WiGLE off"
[ "$WIFI_OK" = 1 ] && LOG green "Flock Wi-Fi: $(tr '\n' ' ' < /tmp/wd_ifaces)" || LOG yellow "Flock Wi-Fi: no monitor vif"
[ "$BLE_OK" = 1 ] && LOG green "Flock BLE: on" || LOG yellow "Flock BLE: off"
LOG "Flock loot -> $SES"
sleep 1

# this device has no pkill - kill by pattern via pgrep (never ourselves)
kill_pat() { local p; for p in $(pgrep -f "$1" 2>/dev/null); do [ "$p" = "$$" ] || kill "$p" 2>/dev/null; done; }
cleanup() {
    touch "$STOP" 2>/dev/null
    # kill the long-runners; the while-loops around them then see $STOP and exit,
    # and the alert sink gets EOF once the matchers (its writers) are gone.
    kill_pat "tcpdump -i"
    kill_pat "gpspipe -w"
    kill_pat "blue5ctl fetch"
    kill_pat "pineap/gps/get"
    kill_pat "FIXF=$FIX"
    [ "$BLE_OK" = 1 ] && command -v bluetoothctl >/dev/null && bluetoothctl scan off >/dev/null 2>&1
    WIGLE_STOP >/dev/null 2>&1
    UI_RELEASE 2>/dev/null
    # build the Flock KML + report
    if [ -f "$SES/hits.csv" ]; then
        awk -v TITLE="Wardrive HUD Flock $(basename "$SES")" "$(cat "$FLK/kml.awk")" "$SES/hits.csv" > "$SES/flock.kml" 2>/dev/null
        fsum=$(awk -F, 'NR>1 && !s[$5]++ { c[$8]++; n++ } END { printf "%d flock devices (high %d, med %d, low %d)", n+0, c["high"]+0, c["medium"]+0, c["low"]+0 }' "$SES/hits.csv")
    fi
    if [ -n "$WLOG" ] && [ -f "$WLOG" ]; then
        rows=$(wc -l < "$WLOG" 2>/dev/null | tr -d ' '); rows=$((rows>2 ? rows-2 : 0))
        LOG green "WiGLE log: $rows APs -> $WLOG"
    fi
    LOG "${fsum:-0 flock devices}"
    LOG "Flock loot: $SES"
    rm -f "$STOP" "$RESET" "$GPSF" "$FIX" "$FIFO" "$FHITS" "$FALERT" "$DAT" /tmp/wd_ifaces /tmp/wd_flock_ses 2>/dev/null
}
trap cleanup EXIT INT TERM

UI_TAKEOVER 2>/dev/null
"$BIN"
