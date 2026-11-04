#!/bin/bash
# Title: Field Marker
# Author: Jenx
# Description: Tag GPS waypoints by label with the D-pad. Create labels, assign them
#   to the four buttons, then tap one when you see something on an engagement. Saves
#   GPX/KML/CSV to loot. Self-contained - works on a stock firmware 2.0 Pager.
# Version: 1.0

# Menu-launched payloads run from a /tmp copy, so $0's dir isn't where our files
# are. Find the folder that actually holds the fieldtag binary.
HERE=""
for d in "${_PAYLOAD_HOME:-}" \
         "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)" \
         "/root/payloads/user/general/Field_Marker" \
         "/mmc/root/payloads/user/general/Field_Marker"; do
    [ -n "$d" ] && [ -f "$d/fieldtag" ] && { HERE="$d"; break; }
done
[ -z "$HERE" ] && { LOG red "Field Marker: fieldtag binary not found."; exit 1; }
chmod +x "$HERE/fieldtag" "$HERE/recorder.sh" 2>/dev/null

DATADIR=/root/loot/FieldMarker            # loot survives firmware updates
mkdir -p "$DATADIR"
LABELS="$DATADIR/labels.txt"
SLOTS="$DATADIR/slots.conf"
touch "$LABELS"

new_label() {
    local t; t="$(TEXT_PICKER 'New label (e.g. Security camera)')" || return
    [ -z "$t" ] && return
    t="${t//,/ }"                          # commas break the CSV
    grep -qxF "$t" "$LABELS" || printf '%s\n' "$t" >> "$LABELS"
    PROMPT "Added label:
$t"
}

delete_label() {
    [ -s "$LABELS" ] || { PROMPT "No labels yet."; return; }
    local opts=() l
    while IFS= read -r l; do [ -n "$l" ] && opts+=("$l"); done < "$LABELS"
    opts+=("<cancel>")
    local sel; sel="$(LIST_PICKER 'Delete which label?' "${opts[@]}")" || return
    [ "$sel" = "<cancel>" ] && return
    grep -vxF "$sel" "$LABELS" > "$LABELS.tmp" && mv "$LABELS.tmp" "$LABELS"
    PROMPT "Deleted:
$sel"
}

assign_buttons() {
    [ -s "$LABELS" ] || { PROMPT "No labels yet.
Create some first."; return; }
    : > "$SLOTS.tmp"
    local d opts l sel
    for d in UP DOWN LEFT RIGHT; do
        opts=()
        while IFS= read -r l; do [ -n "$l" ] && opts+=("$l"); done < "$LABELS"
        opts+=("(none)")
        sel="$(LIST_PICKER "Assign the $d button" "${opts[@]}")" || { rm -f "$SLOTS.tmp"; return; }
        [ "$sel" = "(none)" ] && sel=""
        printf '%s\n' "$sel" >> "$SLOTS.tmp"
    done
    mv "$SLOTS.tmp" "$SLOTS"
    PROMPT "Buttons set.
Pick 'Start tagging'."
}

start_tagging() {
    [ -s "$SLOTS" ] || { PROMPT "Assign the buttons first."; return; }
    cp "$SLOTS" /tmp/fieldtag.slots
    rm -f /tmp/fieldtag.q /tmp/fieldtag.status
    : > /tmp/fieldtag.go
    local CSV="$DATADIR/tags-$(date -u +%Y%m%d).csv"
    [ -f "$CSV" ] || printf 'epoch,time,label,fix,lat,lon,alt\n' > "$CSV"
    setsid bash "$HERE/recorder.sh" "$CSV" >/dev/null 2>&1 &
    local rec=$!
    UI_TAKEOVER
    "$HERE/fieldtag"
    UI_RELEASE
    rm -f /tmp/fieldtag.go
    kill -TERM -"$rec" 2>/dev/null
    sleep 0.4
    local n; n=$(($(wc -l < "$CSV") - 1))
    PROMPT "Session saved.
$n markers in
$(basename "$CSV")"
}

export_tracks() {
    local any=0 CSV base n
    for CSV in "$DATADIR"/tags-*.csv; do
        [ -f "$CSV" ] || continue
        any=1; base="${CSV%.csv}"
        # GPX waypoints
        { printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
            '<gpx version="1.1" creator="Field Marker"><metadata><name>Field Marker</name></metadata>'
          tail -n +2 "$CSV" | while IFS=, read -r epoch time label fix lat lon alt; do
              label="${label%\"}"; label="${label#\"}"
              [ -n "$lat" ] && [ -n "$lon" ] || continue
              printf '<wpt lat="%s" lon="%s"><name>%s</name><time>%s</time>' "$lat" "$lon" "$label" "$time"
              [ -n "$alt" ] && printf '<ele>%s</ele>' "$alt"
              printf '</wpt>\n'
          done
          printf '%s\n' '</gpx>'
        } > "$base.gpx"
        # KML
        { printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
            '<kml xmlns="http://www.opengis.net/kml/2.2"><Document><name>Field Marker</name>'
          tail -n +2 "$CSV" | while IFS=, read -r epoch time label fix lat lon alt; do
              label="${label%\"}"; label="${label#\"}"
              [ -n "$lat" ] && [ -n "$lon" ] || continue
              printf '<Placemark><name>%s</name><TimeStamp><when>%s</when></TimeStamp>' "$label" "$time"
              printf '<Point><coordinates>%s,%s,%s</coordinates></Point></Placemark>\n' "$lon" "$lat" "${alt:-0}"
          done
          printf '%s\n' '</Document></kml>'
        } > "$base.kml"
    done
    [ "$any" = 1 ] && PROMPT "Exported GPX + KML to
$DATADIR" || PROMPT "No markers to export yet."
}

session_count() {
    local CSV="$DATADIR/tags-$(date -u +%Y%m%d).csv"
    [ -f "$CSV" ] && PROMPT "Today: $(( $(wc -l < "$CSV") - 1 )) markers
in $(basename "$CSV")" || PROMPT "No markers today yet."
}

while true; do
    sel="$(LIST_PICKER 'Field Marker' \
        'Start tagging' 'New label' 'Assign buttons' 'Delete label' 'Export GPX/KML' "Today's count" 'Exit' \
        'Start tagging')"
    case "$sel" in
        'Start tagging')   start_tagging ;;
        'New label')       new_label ;;
        'Assign buttons')  assign_buttons ;;
        'Delete label')    delete_label ;;
        'Export GPX/KML')  export_tracks ;;
        "Today's count")   session_count ;;
        'Exit'|'')         rm -f /tmp/fieldtag.go; exit 0 ;;
    esac
done
