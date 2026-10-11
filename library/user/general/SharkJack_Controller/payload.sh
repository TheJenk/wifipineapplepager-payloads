#!/bin/bash
# Title: Shark Jack Controller
# Author: Jenx
# Description: Use the Pager as a field console for a Hak5 Shark Jack Cable on its USB host port, over the Cable's serial console. Shows a live status panel (port, firmware, switch/mode, power, ethernet link, active payload), browses the Cable's payload library and ACTIVATEs (stages) a chosen payload with the D-pad, pulls loot off the Cable into the Pager's /root/loot/sharkjack, and has a live-output view that shows the payload's SERIAL_WRITE narration on the Pager when you flip the Cable switch to ATTACK. Stops gpsd while connected and restarts it on exit.
# Version: 1.1
# Category: general

# ---------------------------------------------------------------------------
# Controls (status):  Green = refresh   Down = payloads   Up = loot
#                      Right = watch output   Red = exit   Power = display sleep
# In the payload list: Up/Down move, Green arms, Red goes back.
# In the loot list:    Up/Down move, Green pulls+views a file, Red goes back.
# In live output:      Red goes back (set the Cable switch to ARMING first).
#
# A small Go binary (source + Makefile/build.sh included) that takes the screen
# with the official UI_TAKEOVER / UI_RELEASE handoff. It talks to the Cable's
# CP2102 serial console (/dev/ttyUSB0, 57600 8N1). Because the Pager's gpsd owns
# that same port, the binary stops gpsd while it holds the link and restarts it
# on exit -- so GPS and this tool use the one USB host port one at a time.
# Requires the Cable plugged into the Pager's USB host port with its switch in
# the ARMING position.
# ---------------------------------------------------------------------------

BIN="sharkjack-controller"
DIR=""
for d in "${_PAYLOAD_HOME:-}" \
         "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)" \
         "/root/payloads/user/general/SharkJack_Controller"; do
    [ -n "$d" ] || continue
    [ -f "$d/$BIN" ] && { DIR="$d"; break; }
done
[ -z "$DIR" ] && { LOG red "$BIN not found. Reinstall the SharkJack_Controller folder."; exit 1; }
chmod +x "$DIR/$BIN" 2>/dev/null

# Quick presence hint (non-fatal): the CP2102 serial node for the Cable.
if [ ! -e /dev/ttyUSB0 ] && [ ! -e /dev/serial/by-path/1.1_1-1.1:1.0 ]; then
    LOG yellow "No Shark Jack serial port found - plug the Cable into the USB host port (arming mode)."
    sleep 2
fi

cleanup() { UI_RELEASE 2>/dev/null; }
trap cleanup EXIT INT TERM

UI_TAKEOVER 2>/dev/null
"$DIR/$BIN"
