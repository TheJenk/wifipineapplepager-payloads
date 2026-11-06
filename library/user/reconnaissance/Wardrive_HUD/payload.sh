#!/bin/bash
# Title: Wardrive HUD
# Author: Jenx
# Description: The ultimate road HUD, built entirely on the Pager UI Kit (ui.sh + the prebuilt renderer) - no custom C. A wardriving heads-up display: a scrolling compass heading tape, big speed, GPS fix/sats, access points logged (total + new this trip, from the stock WiGLE logger), trip distance and lat/lon. AT THE SAME TIME it passively hunts Flock Safety ALPR cameras, logging every hit to its own WiGLE CSV + KML map; on a detection it flashes a FLOCK DETECTED warning and plays an alert tone. Left/Right change speed units, Green (A) resets the trip, Red (B) exits. Needs a USB GPS. fw 2.0 + 1.1.x.
# Version: 3.1
# Category: reconnaissance

# ---- find the UI kit next to this payload (works from a /tmp copy too) --------
HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)"
for d in "${_PAYLOAD_HOME:-}" "$HERE" "/root/payloads/user/reconnaissance/Wardrive_HUD"; do
    [ -n "$d" ] && [ -f "$d/ui.sh" ] && { KIT="$d"; [ -z "$HERE" ] && HERE="$d"; break; }
done
[ -z "$KIT" ] && { command -v LOG >/dev/null && LOG red "ui.sh not found. Reinstall the folder."; exit 1; }
. "$KIT/ui.sh"
FLK="$HERE/flock"

# ---- Flock signatures (flock-you by colonelpanichacks, MIT; see README) ------
SSID_RE='flock|penguin|fs ext battery|pigvision'
OUI_FLOCK="b4:1e:52"
OUI_COMMUNITY="70:c9:4e 3c:91:80 d8:f3:bc 80:30:49 b8:35:32 14:5a:fc 74:4c:a1 08:3a:88
9c:2f:9d c0:35:32 94:08:53 e4:aa:ea f4:6a:dd e0:0a:f6 24:b2:b9 00:f4:8d d0:39:57 e8:d0:fc
e0:4f:43 b8:1e:a4 70:08:94 58:8e:81 ec:1b:bd 3c:71:bf 58:00:e3 90:35:ea 5c:93:a2 64:6e:69
48:27:ea a4:cf:12 14:b5:cd 82:6b:f2 00:03:7f"
BLE_MFG=0x09c8
ALERT_MIN=high; LOG_MIN=medium; REPEAT=5; FALERT_SECS=20
# alert tone - short urgent "!" stab (homage to the MGS alert; not the original)
MGS_RTTTL="flockalert:d=16,o=7,b=300:c,c,16p,c,c,8p,c,c,16p,c,c"

# ---- files -------------------------------------------------------------------
STOP=/tmp/wd.stop; GPSF=/tmp/wd_gps; FIX=/tmp/wd_fix; FIFO=/tmp/wd_flock.fifo
FHITS=/tmp/wd_flock_seen; FALERT=/tmp/wd_flock_at
rm -f "$STOP" "$GPSF" "$FIX" "$FHITS" "$FALERT" "$FIFO"
echo "0 0 0 0 0 0 0" > "$GPSF"; : > "$FHITS"; echo 0 > "$FALERT"; mkfifo "$FIFO" 2>/dev/null

# ---- GPS source: pineapd API (fw 2.x) or gpsd (fw 1.1.x) ---------------------
GPS_API=0
if curl -s --unix-socket /tmp/api.sock http://localhost/api/pineap/gps/get 2>/dev/null | grep -q '"connected"'; then
    GPS_API=1
else
    command -v gpspipe >/dev/null 2>&1 || {
        if ping -c1 -W3 1.1.1.1 >/dev/null 2>&1 || ping -c1 -W3 8.8.8.8 >/dev/null 2>&1; then
            opkg update >/tmp/wd_opkg.log 2>&1; opkg install gpsd gpsd-clients >>/tmp/wd_opkg.log 2>&1; fi; }
    pgrep -x gpsd >/dev/null 2>&1 || for dev in /dev/ttyUSB0 /dev/ttyACM0 /dev/ttyUSB1 /dev/ttyACM1; do
        [ -e "$dev" ] && { gpsd -n "$dev" 2>/dev/null; sleep 1; break; }; done
fi

# One GPS reader -> $GPSF ("mode spd10 hdg sats late5 lone5 distcum") for the HUD
# and $FIX ("lat lon alt acc date time uptime") for Flock. Heading is derived.
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
/"connected":/{ fix=jint("fix"); if(fix<0)fix=0; if($0 !~ /"connected":true/)fix=0;
    sp=jflt("speed"); if(sp<0)sp=0; spd=int(sp*10+0.5); st=jint("sats_tracked"); if(st>=0)sats=st;
    alt=jflt("alt"); if(alt<0)alt=0; acc=jflt("hprecision"); if(acc<0)acc=0; lat=jflt("lat"); lon=jflt("lon");
    emit(fix,spd,sats,(lat==-9999?0:lat),(lon==-9999?0:lon)); next }
/"class":"SKY"/{ u=jint("uSat"); if(u<0)u=jint("nSat"); if(u>=0)sats=u; next }
/"class":"TPV"/{ mode=jint("mode"); if(mode<0)mode=0; sp=jflt("speed"); if(sp<0)sp=0; spd=int(sp*10+0.5);
    alt=jflt("altHAE"); if(alt<0)alt=jflt("alt"); if(alt<0)alt=0; acc=jflt("eph"); if(acc<0)acc=0;
    lat=jflt("lat"); lon=jflt("lon"); emit(mode,spd,sats,(lat==-9999?0:lat),(lon==-9999?0:lon)); next }'
if [ "$GPS_API" = 1 ]; then
    ( while [ ! -f "$STOP" ]; do curl -s --unix-socket /tmp/api.sock http://localhost/api/pineap/gps/get 2>/dev/null; echo; sleep 1; done | awk -v G="$GPSF" -v FIXF="$FIX" "$GPSAWK" ) &
else
    ( while [ ! -f "$STOP" ]; do gpspipe -w 2>/dev/null; sleep 2; done | awk -v G="$GPSF" -v FIXF="$FIX" "$GPSAWK" ) &
fi

# ---- stock WiGLE logger + Flock detection (own loot) ------------------------
WLOG="$(WIGLE_START 2>/tmp/wd_wigle.err)"; [ -n "$WLOG" ] && [ -f "$WLOG" ] || WLOG=""
SES="/root/loot/wardrive_hud_flock/$(date +%Y%m%d_%H%M%S)"; mkdir -p "$SES"
echo "time_utc,lat,lon,type,mac,name,rssi,confidence,reason" > "$SES/hits.csv"
FW=""; read -r FW _ < /etc/pineapplepager/version 2>/dev/null
{ echo "WigleWifi-1.6,appRelease=1.1,model=WiFi Pineapple Pager,release=${FW},device=Pager,display=Flock Wardrive,board=MT7628,brand=Hak5,star=Sol,body=3,subBody=0"
  echo "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type"; } > "$SES/wigle.csv"
prog(){ cat "$FLK/lib.awk" "$FLK/$1"; }
AWKV=(-v FIXF="$FIX" -v HITS="$SES/hits.csv" -v WIG="$SES/wigle.csv" -v REPEAT="$REPEAT" -v ALERTMIN="$ALERT_MIN" -v LOGMIN="$LOG_MIN")

# alert sink: each hit -> bump counter, stamp time, play the tone
( while IFS= read -r line; do [ -z "$line" ] && continue; echo "$line" >> "$FHITS"; date +%s > "$FALERT"; RINGTONE "$MGS_RTTTL" >/dev/null 2>&1 & logger -t Wardrive_HUD "$line" 2>/dev/null; done ) < "$FIFO" &

iw dev 2>/dev/null | awk '/Interface/{i=$2} /type monitor/{print i}' > /tmp/wd_ifaces
WIFI_OK=0
if command -v tcpdump >/dev/null; then while IFS= read -r IF; do [ -n "$IF" ] || continue; WIFI_OK=1
    ( while [ ! -f "$STOP" ]; do tcpdump -i "$IF" -e -n -l -s 256 type mgt 2>/dev/null | awk "${AWKV[@]}" -v SSIDRE="$SSID_RE" -v OUI_FLOCK="$OUI_FLOCK" -v OUI_COMMUNITY="$(echo $OUI_COMMUNITY)" "$(prog wifi.awk)"; sleep 1; done > "$FIFO" ) &
  done < /tmp/wd_ifaces; fi
BLE_OK=0
if command -v blue5ctl >/dev/null && blue5ctl ping >/dev/null 2>&1; then BLE_OK=1
    ( while [ ! -f "$STOP" ]; do blue5ctl fetch -n 0 -g 3 2>/dev/null; sleep 2; done | awk "${AWKV[@]}" -v MODE=blue5 -v BLE_MFG="$BLE_MFG" "$(prog ble.awk)" > "$FIFO" ) &
elif command -v bluetoothctl >/dev/null && bluetoothctl power on >/dev/null 2>&1; then BLE_OK=1
    ( while [ ! -f "$STOP" ]; do bluetoothctl --timeout 60 scan on 2>/dev/null; done | awk "${AWKV[@]}" -v MODE=bluez -v BLE_MFG="$BLE_MFG" "$(prog ble.awk)" > "$FIFO" ) &
fi

# =============================== HUD (UI kit) ================================
# speed (0.1 m/s) -> the chosen unit ; distance (m) -> mi/km
conv_speed(){ case "$2" in 0) echo $(( $1*2237/10000 ));; 1) echo $(( $1*36/100 ));; *) echo $(( $1*19438/100000 ));; esac; }
unit_name(){ case "$1" in 0) echo MPH;; 1) echo KMH;; *) echo KN;; esac; }

# heading (deg) -> cardinal letters
cardinal(){ local c=(N NE E SE S SW W NW); echo "${c[$(( (($1+22)/45) % 8 ))]}"; }
# the FLOCK DETECTED banner, drawn on top of whatever page is up (then re-shown)
flock_banner(){
    UI_RECT 40 72 400 62 e9b900; UI_BOX 40 72 400 62 000000; UI_BOX 42 74 396 58 000000
    UI_TEXT 92 86 "FLOCK DETECTED" 000000 3
    UI_TEXT 132 118 "ALPR CAMERA NEARBY" 302200 1
    UI_SHOW
}

UI_BEGIN
units=0; apbase=-1; distbase=0; pg=0
PAGES=(drive wardrive flock gps); NP=${#PAGES[@]}
hud_cleanup(){
    [ -n "$_WD_CLEANED" ] && return; _WD_CLEANED=1
    touch "$STOP" 2>/dev/null
    local p
    for pat in "tcpdump -i" "gpspipe -w" "blue5ctl fetch" "pineap/gps/get" "FIXF=$FIX"; do
        for p in $(pgrep -f "$pat" 2>/dev/null); do [ "$p" = "$$" ] || kill "$p" 2>/dev/null; done; done
    [ "$BLE_OK" = 1 ] && command -v bluetoothctl >/dev/null && bluetoothctl scan off >/dev/null 2>&1
    WIGLE_STOP >/dev/null 2>&1
    UI_END
    if [ -f "$SES/hits.csv" ]; then
        awk -v TITLE="Wardrive HUD Flock $(basename "$SES")" "$(cat "$FLK/kml.awk")" "$SES/hits.csv" > "$SES/flock.kml" 2>/dev/null
        local fsum; fsum=$(awk -F, 'NR>1 && !s[$5]++ { c[$8]++; n++ } END { printf "%d flock devices (high %d, med %d, low %d)", n+0, c["high"]+0, c["medium"]+0, c["low"]+0 }' "$SES/hits.csv")
        LOG "${fsum:-0 flock devices}"; LOG "Flock loot: $SES"
    fi
    [ -n "$WLOG" ] && LOG green "WiGLE log -> $WLOG"
    rm -f "$STOP" "$GPSF" "$FIX" "$FIFO" "$FHITS" "$FALERT" /tmp/wd_ifaces 2>/dev/null
}
trap hud_cleanup EXIT INT TERM

# Each page is a view file; Left/Right flips through them. The FLOCK banner
# overlays whatever page is up. Variables below feed \(...) in the views.
while true; do
    read mode spd10 hdg sats le lo cum < "$GPSF" 2>/dev/null
    : "${mode:=0}" "${spd10:=0}" "${hdg:=0}" "${sats:=0}" "${le:=0}" "${lo:=0}" "${cum:=0}"
    [ -f "$FIX" ] && read flat flon alt acc _ < "$FIX" 2>/dev/null   # alt/acc for the GPS page (only exists once there's a fix)
    : "${alt:=0}" "${acc:=0}"

    total=0; [ -n "$WLOG" ] && [ -f "$WLOG" ] && { n=$(wc -l < "$WLOG" 2>/dev/null); total=$(( n>2 ? n-2 : 0 )); }
    [ "$apbase" -lt 0 ] && apbase=$total
    apnew=$(( total-apbase )); [ "$apnew" -lt 0 ] && apnew=0
    distm=$(( cum-distbase )); [ "$distm" -lt 0 ] && distm=0

    flk=$(wc -l < "$FHITS" 2>/dev/null); : "${flk:=0}"
    at=$(cat "$FALERT" 2>/dev/null); : "${at:=0}"; now=$(date +%s); falert=0
    [ "$at" -gt 0 ] && [ $((now-at)) -lt "$FALERT_SECS" ] && falert=1

    # ---- derive the display vars the views interpolate ----
    page="$((pg+1))/$NP"
    sp=$(conv_speed "$spd10" "$units"); unit=$(unit_name "$units")
    card=$(cardinal "$hdg")
    if   [ "$mode" -ge 3 ]; then fixtxt="3D"; fixcol=green
    elif [ "$mode" -ge 2 ]; then fixtxt="2D"; fixcol=amber
    else fixtxt="NONE"; fixcol=red; fi
    if [ "$units" = 0 ]; then m10=$(( distm*6214/10000000 )); printf -v dist "%d.%d MI" $((m10/10)) $((m10%10))
    else m10=$(( distm/100 )); printf -v dist "%d.%d KM" $((m10/10)) $((m10%10)); fi
    [ -n "$WLOG" ] && { wig=ON; wigcol=green; } || { wig=OFF; wigcol=red; }
    [ "$flk" -gt 0 ] && flkcol=red || flkcol=green
    [ "$WIFI_OK" = 1 ] && { wifi=ON; wifch=green; } || { wifi=OFF; wifch=dim; }
    [ "$BLE_OK" = 1 ] && { ble=ON; blech=green; } || { ble=OFF; blech=dim; }
    lasthit="$(tail -1 "$FHITS" 2>/dev/null)"; lasthit="${lasthit#*: }"; : "${lasthit:=none yet}"
    printf -v lat "%d.%05d" $((le/100000)) $(( (le<0?-le:le)%100000 ))
    printf -v lon "%d.%05d" $((lo/100000)) $(( (lo<0?-lo:lo)%100000 ))
    [ "$mode" -lt 2 ] && { lat="--"; lon="--"; }

    UI_RENDER "views/${PAGES[$pg]}.view"
    [ "$falert" = 1 ] && flock_banner

    case "$(UI_KEY 1)" in
        LEFT)  pg=$(( (pg+NP-1)%NP )) ;;
        RIGHT) pg=$(( (pg+1)%NP )) ;;
        UP)    units=$(( (units+1)%3 )) ;;
        DOWN)  units=$(( (units+2)%3 )) ;;
        A)     apbase=$total; distbase=$cum ;;
        B)     break ;;
    esac
done
hud_cleanup
