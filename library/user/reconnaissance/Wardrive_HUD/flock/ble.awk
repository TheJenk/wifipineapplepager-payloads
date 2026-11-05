# Bluetooth LE input -> detections. Two sources (MODE):
#   blue5  fw 2.x: `blue5ctl fetch -n 0 -g S` tables, polled. NAME column carries
#          "name svc=XXXX,YYYY mfr=.. sd=..", INFO carries "co 0xXXXX".
#   bluez  fw 1.1.x: `bluetoothctl scan on` stream; the first time a MAC shows up
#          we also ask `bluetoothctl info` for its name/UUIDs.
# Vars: MODE, BLE_MFG (company id, e.g. 0x09c8)

function strip(s) { gsub(/\033\[[0-9;]*[a-zA-Z]/, "", s); gsub(/\r/, "", s); return s }

function evaluate(mac,  n, ln, u, conf, why, d10) {
    n = (mac in nm) ? nm[mac] : (mac in al) ? al[mac] : ""
    ln = tolower(n); u = uu[mac]; conf = ""; why = ""
    d10 = (n ~ /^[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]$/)
    if (u ~ /e8ccbb38-9532-46a8-9fe5-1814df172e6f/)                                 { conf = "high"; why = "flock_gatt" }
    else if (u ~ /0000(3[1-4][0-9a-f][0-9a-f]|3500)-0000-1000-8000-00805f9b34fb/)   { conf = "high"; why = "raven_svc" }
    else if (ln ~ /^penguin-[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]$/)   { conf = "high"; why = "penguin_name" }
    else if (ln ~ /fs ext battery|flock|pigvision/)                                 { conf = "high"; why = "name" }
    else if (mf[mac] == BLE_MFG && (d10 || ln ~ /^penguin/))                        { conf = "high"; why = "xuntong+name" }
    else if (mf[mac] == BLE_MFG)                                                    { conf = "medium"; why = "xuntong_mfg" }
    else if (d10)                                                                   { conf = "low"; why = "10digit_name" }
    else if (ln ~ /dfutarg/ || u ~ /00001530-1212-efde-1523-785feabcd123/)          { conf = "low"; why = "nordic_dfu" }
    if (conf != "") record("BLE", mac, n, 0, 0, (mac in rs) ? rs[mac] : "", conf, why, mf[mac], "Misc [LE]")
}

# ---------------- blue5 (fw 2.x) ----------------
MODE == "blue5" && /ADDRESS +TYPE +PWR/ { ci = index($0, "INFO"); cn = index($0, "NAME"); next }
MODE == "blue5" && ci > 0 && $1 ~ /^[0-9A-F][0-9A-F]:[0-9A-F][0-9A-F]:/ {
    mac = $1; delete_dev(mac)
    rs[mac] = $3 + 0
    info = substr($0, ci, cn - ci)
    if (match(info, /0x[0-9A-Fa-f]+/)) mf[mac] = tolower(substr(info, RSTART, RLENGTH))
    k = split(substr($0, cn), t, " "); name = ""
    for (i = 1; i <= k; i++) {
        if (t[i] ~ /^svc=/) {
            s = substr(t[i], 5); m = split(s, sv, ",")
            for (j = 1; j <= m; j++) uu[mac] = uu[mac] " " (length(sv[j]) == 4 ? "0000" tolower(sv[j]) "-0000-1000-8000-00805f9b34fb" : tolower(sv[j]))
        } else if (t[i] ~ /^[a-z]+=/) continue
        else name = (name == "" ? t[i] : name " " t[i])
    }
    if (name != "") nm[mac] = name
    evaluate(mac)
    next
}
function delete_dev(m) { delete nm[m]; delete uu[m]; delete mf[m]; delete rs[m] }

# ---------------- bluez (fw 1.1.x) ----------------
function prop(mac, line,  v) {
    if (line ~ /Name: /)  { v = line; sub(/.*Name: /, "", v); if (v != "") nm[mac] = v }
    if (line ~ /Alias: /) { v = line; sub(/.*Alias: /, "", v); if (!(mac in nm) && v != "" && v !~ /^[0-9A-F][0-9A-F]-/) al[mac] = v }
    if (line ~ /UUID/)    { uu[mac] = uu[mac] " " tolower(line) }
    if (line ~ /ManufacturerData/ && match(line, /0x[0-9a-fA-F]+/)) mf[mac] = tolower(substr(line, RSTART, RLENGTH))
    if (line ~ /RSSI: /)  { v = line; sub(/.*RSSI: /, "", v); if (match(v, /\(-?[0-9]+\)/)) v = substr(v, RSTART + 1, RLENGTH - 2); rs[mac] = v + 0 }
}
function lookup(mac,  c, l) {
    c = "bluetoothctl info " mac " 2>/dev/null"
    while ((c | getline l) > 0) prop(mac, strip(l))
    close(c)
}
MODE == "bluez" {
    line = strip($0)
    if (!match(line, /Device [0-9A-Fa-f][0-9A-Fa-f](:[0-9A-Fa-f][0-9A-Fa-f])(:[0-9A-Fa-f][0-9A-Fa-f])(:[0-9A-Fa-f][0-9A-Fa-f])(:[0-9A-Fa-f][0-9A-Fa-f])(:[0-9A-Fa-f][0-9A-Fa-f])/)) next
    mac = toupper(substr(line, RSTART + 7, 17))
    if (line ~ /\[DEL\]/) next
    rest = substr(line, RSTART + RLENGTH)
    if (line ~ /\[NEW\]/) { v = rest; sub(/^ +/, "", v); if (v != "" && v !~ /^[0-9A-F][0-9A-F]-/) al[mac] = v }
    else prop(mac, rest)
    if (!(mac in looked)) { looked[mac] = 1; lookup(mac) }
    evaluate(mac)
}
