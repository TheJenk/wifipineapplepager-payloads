# ui.sh - draw custom screens and menus in your Pager payloads, the easy way.
#
#   . /path/to/ui.sh
#   UI_BEGIN
#   choice="$(UI_MENU "Main Menu" "Scan" "Report" "Settings" "Exit")"
#   UI_MESSAGE "Done" "You chose:" "$choice"
#   UI_END
#
# No framebuffer, no font, no C - the bundled renderer does all of that. Commands
# are UPPERCASE so they read like the rest of a payload. Colors can be names
# (RED, GREEN, BLUE, AMBER, ...) or a plain RRGGBB hex.

UI_DIR="${UI_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)}"
UI_BIN="${UI_BIN:-$UI_DIR/pguid}"
UI_VIEWC="${UI_VIEWC:-$UI_DIR/pgviewc}"
_UI_CMD=/tmp/pgui.cmd
_UI_EVT=/tmp/pgui.evt
_UI_W=480; _UI_H=222

# ---- palette (override any of these before UI_BEGIN to reskin) ----
UI_BG=0e1016; UI_BAR=1a2234; UI_LINE=2b3a52
UI_TEXT=e8ecf3; UI_DIM=7f8aa0; UI_ACCENT=5aa0dc
UI_SELECT=2f7d57; UI_SELECT_TEXT=ffffff

# resolve a color name or RRGGBB hex -> hex, into $REPLY (NO subshell, NO pipe).
# Every drawing primitive calls this once per command; doing it fork-free is what
# keeps redraws fast on the device - a command substitution or a `tr` pipe here
# would fork tens of processes per frame. Needs bash (uses ${x^^}).
_col() {
    local c="${1:-}"
    case "${c^^}" in
        WHITE) REPLY=ffffff;; BLACK) REPLY=000000;;
        GREY|GRAY|DIM) REPLY="$UI_DIM";; TEXT) REPLY="$UI_TEXT";; ACCENT) REPLY="$UI_ACCENT";;
        RED) REPLY=e0483f;; GREEN) REPLY=2fae6a;; BLUE) REPLY=3a86c8;; CYAN) REPLY=3ac8c8;;
        YELLOW) REPLY=e0c23f;; AMBER) REPLY=e0a53f;; ORANGE) REPLY=e07b3f;;
        PURPLE) REPLY=9b7ede;; PINK) REPLY=e06ba0;;
        "") REPLY="${2:-$UI_TEXT}";;
        [0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]) REPLY="$c";;   # RRGGBB (either case)
        *) REPLY="${2:-$UI_TEXT}";;
    esac
}

# ---- session ----
UI_BEGIN() {
    rm -f "$_UI_CMD" "$_UI_EVT"; mkfifo "$_UI_CMD" "$_UI_EVT"
    command -v UI_TAKEOVER >/dev/null 2>&1 && UI_TAKEOVER
    # run the renderer with the UI dir as its cwd so Image/UI_IMAGE paths can be
    # relative to the payload (your .pgi files live next to ui.sh).
    ( cd "$UI_DIR" && exec "$UI_BIN" "$_UI_CMD" "$_UI_EVT" ) & _UI_PID=$!
    exec 4>"$_UI_CMD"; exec 3<"$_UI_EVT"
    local r; read -r r <&3                 # wait for READY
    trap UI_END EXIT INT TERM
}
UI_END() {
    trap - EXIT INT TERM
    [ -n "$_UI_PID" ] && { echo "quit" >&4 2>/dev/null; sleep 0.1; kill "$_UI_PID" 2>/dev/null; }
    exec 4>&- 2>/dev/null; exec 3<&- 2>/dev/null
    rm -f "$_UI_CMD" "$_UI_EVT"
    command -v UI_RELEASE >/dev/null 2>&1 && UI_RELEASE
    _UI_PID=""
}

# ---- drawing primitives ----
UI_CLEAR() { _col "${1:-}" "$UI_BG";   echo "clear $REPLY" >&4; }
UI_RECT()  { _col "${5:-}";             echo "rect $1 $2 $3 $4 $REPLY" >&4; }   # filled
UI_BOX()   { _col "${5:-}" "$UI_LINE";  echo "frame $1 $2 $3 $4 $REPLY" >&4; }  # outline
UI_BAR()   { _col "${5:-}" "$UI_ACCENT"; echo "rect $1 $2 $3 $4 $REPLY" >&4; }  # filled bar
# UI_TEXT x y "text" [color] [size 1-4]
UI_TEXT()  { _col "${4:-}";             echo "text $1 $2 ${5:-2} $REPLY $3" >&4; }
UI_SHOW()  { echo "present" >&4; }
# UI_IMAGE x y path   - blit a .pgi image with its top-left at x,y. Path is
# relative to the payload dir (where ui.sh lives) unless absolute. Make a .pgi
# from a PNG with tools/mkimg.sh.
UI_IMAGE() { echo "image $1 $2 $3" >&4; }

# text width in px -> $REPLY (fork-free; used in arithmetic by the callers)
_tw() { local s=${2:-2}; REPLY=$(( ${#1} * 6 * s - s )); }
# UI_TEXT_CENTER cx y "text" [color] [size]
UI_TEXT_CENTER() { _tw "$3" "${5:-2}"; UI_TEXT $(( $1 - REPLY/2 )) "$2" "$3" "${4:-}" "${5:-2}"; }

# ---- layout widgets ----
UI_HEADER() {                                   # title bar across the top
    UI_RECT 0 0 $_UI_W 26 "$UI_BAR"; UI_RECT 0 26 $_UI_W 1 "$UI_LINE"
    UI_TEXT 10 6 "$1" ACCENT 2
}
UI_FOOTER() {                                   # bottom hints: left, right
    UI_RECT 0 $((_UI_H-16)) $_UI_W 1 "$UI_LINE"
    [ -n "$1" ] && UI_TEXT 8 $((_UI_H-12)) "$1" DIM 1
    [ -n "$2" ] && { _tw "$2" 1; UI_TEXT $(( _UI_W-8-REPLY )) $((_UI_H-12)) "$2" DIM 1; }
}
UI_CARD() {                                     # panel x y w h [Title]
    UI_RECT "$1" "$2" "$3" "$4" "$UI_BAR"; UI_BOX "$1" "$2" "$3" "$4" "$UI_LINE"
    [ -n "$5" ] && UI_TEXT $(( $1+10 )) $(( $2+8 )) "$5" TEXT 2
}
# UI_METER x y w h percent [color]   - a 0-100 progress/level bar
UI_METER() {
    local x=$1 y=$2 w=$3 h=$4 pct=$5 col=${6:-GREEN} fw
    UI_BOX "$x" "$y" "$w" "$h" "$UI_LINE"
    fw=$(( (w-2) * pct / 100 )); [ "$fw" -lt 0 ] && fw=0; [ "$fw" -gt $((w-2)) ] && fw=$((w-2))
    [ "$fw" -gt 0 ] && UI_BAR $((x+1)) $((y+1)) "$fw" $((h-2)) "$col"
}
# UI_GRAPH x y w h "v1 v2 v3 ..." [color]   - auto-scaled bar graph of a series
UI_GRAPH() {
    local x=$1 y=$2 w=$3 h=$4 col=${6:-ACCENT}
    set -- $5
    local n=$# max=1 v bw gap=1 i=0 bh
    [ "$n" -gt 0 ] || return
    for v in "$@"; do [ "$v" -gt "$max" ] 2>/dev/null && max=$v; done
    bw=$(( w / n )); [ "$bw" -lt 1 ] && bw=1
    [ "$bw" -gt 2 ] && bw=$((bw-gap))
    for v in "$@"; do
        bh=$(( v * (h-1) / max )); [ "$bh" -lt 1 ] && bh=1
        UI_BAR $(( x + i*(bw+gap) )) $(( y + h - bh )) "$bw" "$bh" "$col"
        i=$((i+1))
    done
}

# ---- view files (SwiftUI-style layouts) ----
# UI_RENDER file.view   - lay out and draw a .view file. \(var) placeholders in
# the view are filled in from the current shell variables. Followed by UI_SHOW
# implicitly (the view includes its own present).
_UI_VF=""; _UI_VTPL=""
UI_RENDER() {
    local vf="$1" content v
    [ -f "$vf" ] || vf="$UI_DIR/$1"
    [ -f "$vf" ] || { echo "[UI_RENDER] no such view: $1" >&2; return 1; }
    # cache the view text so a loop that re-renders the same view every frame
    # reads and forks as little as possible (the layout is recomputed, but the
    # file isn't re-read). $(<file) avoids a `cat` fork on the first read.
    [ "$vf" = "$_UI_VF" ] || { _UI_VTPL="$(<"$vf")"; _UI_VF="$vf"; }
    content="$_UI_VTPL"
    # fill \(name) -> $name  (pure bash, no forks)
    while [[ "$content" =~ \\\(([a-zA-Z_][a-zA-Z0-9_]*)\) ]]; do
        v="${BASH_REMATCH[1]}"; content="${content//\\($v)/${!v}}"
    done
    # compile to draw commands straight into the renderer - one fork per frame
    printf '%s' "$content" | "$UI_VIEWC" >&4
}

# ---- input ----
# UI_KEY [timeout_s]  -> UP / DOWN / LEFT / RIGHT / A / B / POWER (A=Green, B=Red)
UI_KEY() {
    local line
    if [ -n "$1" ]; then read -r -t "$1" line <&3 || { echo ""; return 1; }
    else read -r line <&3; fi
    echo "${line#KEY }"
}

# ---- one-line screens ----
# UI_MENU "Title" item1 item2 ...  -> echoes the chosen item (exit 0),
#   or exits 1 if the user backs out. Up/Down move, Green picks, Red backs out.
UI_MENU() {
    local title="$1"; shift
    local items=("$@") n=${#items[@]} sel=0 top=0 rowh=24 rows=7 ly=34 i y idx
    while true; do
        [ "$sel" -lt "$top" ] && top=$sel
        [ "$sel" -ge $((top+rows)) ] && top=$((sel-rows+1))
        UI_CLEAR; UI_HEADER "$title"
        for ((i=0; i<rows && top+i<n; i++)); do
            idx=$((top+i)); y=$((ly + i*rowh))
            if [ "$idx" -eq "$sel" ]; then
                UI_RECT 6 "$y" $((_UI_W-12)) $((rowh-4)) "$UI_SELECT"
                UI_TEXT 16 $((y+4)) "${items[$idx]}" "$UI_SELECT_TEXT" 2
            else
                UI_TEXT 16 $((y+4)) "${items[$idx]}" TEXT 2
            fi
        done
        [ "$top" -gt 0 ] && UI_TEXT $((_UI_W-16)) 32 "^" DIM 1
        [ $((top+rows)) -lt "$n" ] && UI_TEXT $((_UI_W-16)) $((_UI_H-26)) "v" DIM 1
        UI_FOOTER "GREEN  SELECT" "RED  BACK"; UI_SHOW
        case "$(UI_KEY)" in
            UP)   sel=$(( sel>0 ? sel-1 : n-1 )) ;;
            DOWN) sel=$(( sel<n-1 ? sel+1 : 0 )) ;;
            A)    echo "${items[$sel]}"; return 0 ;;
            B)    return 1 ;;
        esac
    done
}

# UI_MESSAGE "Title" "line one" "line two" ...  -> show, wait for any key
UI_MESSAGE() {
    local title="$1"; shift
    UI_CLEAR; UI_HEADER "$title"
    local y=$(( (_UI_H-26)/2 - (${#}*11)/2 + 26 )) ln
    for ln in "$@"; do UI_TEXT_CENTER $((_UI_W/2)) "$y" "$ln" TEXT 2; y=$((y+22)); done
    UI_FOOTER "" "ANY KEY"; UI_SHOW
    UI_KEY >/dev/null
}

# UI_CONFIRM "Question?"  -> exit 0 if Green (yes), 1 if Red (no)
UI_CONFIRM() {
    UI_CLEAR; UI_HEADER "CONFIRM"
    UI_TEXT_CENTER $((_UI_W/2)) 90 "$1" TEXT 2
    UI_TEXT_CENTER $((_UI_W/2)) 150 "GREEN = YES    RED = NO" DIM 1
    UI_FOOTER "" ""; UI_SHOW
    while true; do case "$(UI_KEY)" in A) return 0;; B) return 1;; esac; done
}
