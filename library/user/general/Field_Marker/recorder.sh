#!/bin/bash
# recorder.sh <csv> - background helper for the Field Marker tag view.
# Polls the queue the on-screen binary appends to; for each tap it reads the GPS
# fix and writes one waypoint row to the CSV. Runs under setsid; payload.sh stops
# it with a process-group kill when the tag view exits.
CSV="$1"
Q=/tmp/fieldtag.q
ST=/tmp/fieldtag.status
SLOTS=/tmp/fieldtag.slots
GO=/tmp/fieldtag.go

# load the 4 slot labels (UP, DOWN, LEFT, RIGHT)
L0=""; L1=""; L2=""; L3=""
i=0
while IFS= read -r line; do
    case $i in 0) L0="$line";; 1) L1="$line";; 2) L2="$line";; 3) L3="$line";; esac
    i=$((i+1))
done < "$SLOTS"
label_for() { case "$1" in 0) echo "$L0";; 1) echo "$L1";; 2) echo "$L2";; 3) echo "$L3";; esac; }

iso() { awk -v e="$1" 'BEGIN{print strftime("%Y-%m-%dT%H:%M:%SZ", e)}'; }
jget() { echo "$1" | sed -n "s/.*\"$2\":\"\{0,1\}\([^,\"}]*\)\"\{0,1\}.*/\1/p" | head -1; }

last=0
while [ -f "$GO" ]; do
    n=$(wc -l < "$Q" 2>/dev/null); n=${n//[^0-9]/}; : "${n:=0}"
    if [ "$n" -gt "$last" ]; then
        sed -n "$((last+1)),${n}p" "$Q" | while IFS=, read -r epoch slot; do
            [ -n "$epoch" ] || continue
            j=$(curl -s -m3 --unix-socket /tmp/api.sock http://localhost/api/pineap/gps/get)
            conn=$(jget "$j" connected); fix=$(jget "$j" fix)
            lat=$(jget "$j" lat); lon=$(jget "$j" lon); alt=$(jget "$j" alt)
            lbl=$(label_for "$slot")
            # Discreet, silent confirmation: haptic only (no ringtone), so tagging
            # a target in the field makes no sound. One short buzz = logged with a
            # GPS fix; a double buzz = logged but no fix yet.
            if [ "$conn" = "true" ] && [ "${fix:-0}" -ge 1 ] 2>/dev/null; then
                printf 'FIX\n' > "$ST"; VIBRATE "ok:d=32,o=6,b=900:c" 2>/dev/null &
                gf=1
            else
                printf 'NO FIX\n' > "$ST"; VIBRATE "nf:d=16,o=5,b=640:c,p,c" 2>/dev/null &
                lat=""; lon=""; alt=""; gf=0
            fi
            printf '%s,%s,"%s",%s,%s,%s,%s\n' "$epoch" "$(iso "$epoch")" "$lbl" "$gf" "$lat" "$lon" "$alt" >> "$CSV"
        done
        last=$n
    fi
    sleep 0.3
done
