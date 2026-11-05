# Shared helpers for the Wi-Fi and BLE matchers (prepended to each).
# Vars from payload.sh: FIXF HITS WIG REPEAT ALERTMIN LOGMIN

function uptime(  l, a) {
    l = ""; getline l < "/proc/uptime"; close("/proc/uptime")
    split(l, a, " "); return int(a[1])
}

# Load the latest GPS fix written by gps.awk: "lat lon alt acc date time uptime".
# A fix older than 10 s counts as no fix.
function loadfix(  l, f, n, c) {
    FX = 0; LAT = ""; LON = ""; ALT = ""; ACC = ""
    l = ""; getline l < FIXF; close(FIXF)
    n = split(l, f, " ")
    if (n >= 7 && uptime() - f[7] <= 10) {
        LAT = f[1]; LON = f[2]; ALT = f[3]; ACC = f[4]; TS = f[5] " " f[6]; FX = 1
    } else {
        c = "date \"+%Y-%m-%d %H:%M:%S\""; TS = ""; c | getline TS; close(c)
    }
}

function rank(c) { return (c == "high") ? 3 : (c == "medium") ? 2 : (c == "low") ? 1 : 0 }

# CSV-safe text: no commas, quotes or control chars.
function clean(s) { gsub(/[,"\r\n\t]/, " ", s); return s }

# Log one detection. Rows for the same device are rate-limited to one per REPEAT
# seconds, so a parked camera doesn't flood the log but a drive-by still gives
# several positions (the KML picks the strongest one).
function record(type, mac, name, ch, freq, rssi, conf, why, mfgr, auth,   t) {
    if (rank(conf) < rank(LOGMIN)) return
    mac = tolower(mac)
    t = uptime()
    if ((mac in lastlog) && t - lastlog[mac] < REPEAT) return
    lastlog[mac] = t
    loadfix()
    name = clean(name)
    print TS "," LAT "," LON "," type "," mac "," name "," rssi "," conf "," why >> HITS
    close(HITS)
    if (FX) {
        print mac "," name "," auth "," TS "," ch "," freq "," rssi "," LAT "," LON "," ALT "," ACC ",," mfgr "," type >> WIG
        close(WIG)
    }
    if (rank(conf) >= rank(ALERTMIN) && !(mac in alerted)) {
        alerted[mac] = 1
        print "Flock " type " (" conf "): " (name != "" ? name : mac) " " rssi "dBm" (FX ? "" : " NO GPS")
        fflush()
    }
}
