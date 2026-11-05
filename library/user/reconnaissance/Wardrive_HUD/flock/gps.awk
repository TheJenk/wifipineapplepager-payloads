# GPS -> FIXF holds the latest 2D/3D fix: "lat lon alt acc date time uptime"
# Input is either pineapd's API (fw 2.x: one JSON object per line from
# /api/pineap/gps/get) or gpsd's `gpspipe -w` stream (fw 1.1.x).
function jnum(k,  p) {
    if (match($0, "\"" k "\":-?[0-9.]+")) { p = substr($0, RSTART, RLENGTH); sub(/.*:/, "", p); return p }
    return ""
}
function jstr(k,  p) {
    if (match($0, "\"" k "\":\"[^\"]*\"")) { p = substr($0, RSTART, RLENGTH); sub(/^[^:]*:"/, "", p); sub(/"$/, "", p); return p }
    return ""
}
function put(lat, lon, alt, acc, t,  u, ua) {
    u = ""; getline u < "/proc/uptime"; close("/proc/uptime"); split(u, ua, " ")
    print lat, lon, alt, acc, t, int(ua[1]) > FIXF; close(FIXF)
}
# pineapd API (fw 2.x)
/"connected":/ {
    if ($0 !~ /"connected":true/ || jnum("fix") + 0 < 1) next
    lat = jnum("lat"); lon = jnum("lon")
    if (lat == "" || lon == "" || (lat + 0 == 0 && lon + 0 == 0)) next
    alt = jnum("alt"); if (alt == "") alt = 0
    acc = jnum("hprecision"); if (acc == "") acc = 0
    put(lat, lon, alt, acc, strftime("%Y-%m-%d %H:%M:%S", systime(), 1))
    next
}
# gpsd (fw 1.1.x)
/"class":"TPV"/ {
    if (jnum("mode") + 0 < 2) next
    lat = jnum("lat"); lon = jnum("lon"); if (lat == "" || lon == "") next
    alt = jnum("altHAE"); if (alt == "") alt = jnum("alt"); if (alt == "") alt = 0
    acc = jnum("eph"); if (acc == "") acc = 0
    t = jstr("time"); sub(/\..*/, "", t); sub(/Z$/, "", t); sub(/T/, " ", t)
    if (t == "") t = "0000-00-00 00:00:00"
    put(lat, lon, alt, acc, t)
}
