# tcpdump -e -n -l (monitor mode, management frames)  ->  Wi-Fi detections.
# Vars: SSIDRE (lowercase regex), OUI_FLOCK, OUI_COMMUNITY (space-separated lists)
BEGIN {
    n = split(tolower(OUI_FLOCK), a, " ");     for (i = 1; i <= n; i++) strong[a[i]] = 1
    n = split(tolower(OUI_COMMUNITY), a, " "); for (i = 1; i <= n; i++) comm[a[i]] = 1
}
!/Probe Request|Probe Response|Beacon/ { next }
{
    freq = 0; sig = ""; sa = ""; bssid = ""
    for (i = 1; i <= NF; i++) {
        if ($i == "MHz") freq = $(i - 1) + 0
        else if ($i ~ /dBm$/ && sig == "") { sig = $i; sub(/dBm$/, "", sig) }
        else if ($i ~ /^SA:/) sa = tolower(substr($i, 4))
        else if ($i ~ /^BSSID:/) bssid = tolower(substr($i, 7))
    }
    kind = ""; ssid = ""
    if ((p = index($0, "Probe Request (")) > 0)       { kind = "probe";  r = substr($0, p + 15) }
    else if ((p = index($0, "Probe Response (")) > 0) { kind = "resp";   r = substr($0, p + 16) }
    else if ((p = index($0, "Beacon (")) > 0)         { kind = "beacon"; r = substr($0, p + 8) }
    if (kind == "") next
    if ((q = index(r, ")")) > 0) ssid = substr(r, 1, q - 1)

    mac = (sa != "") ? sa : bssid
    if (mac == "") next
    oui = substr(mac, 1, 8)

    conf = ""; why = ""
    if (ssid != "" && tolower(ssid) ~ SSIDRE)      { conf = "high";   why = "ssid" }
    else if (oui in strong)                        { conf = "high";   why = "flock_oui" }
    else if ((oui in comm) && kind == "probe" && ssid == "") { conf = "medium"; why = "oui+wildcard_probe" }
    else if (oui in comm)                          { conf = "low";    why = "oui" }
    if (conf == "") next

    ch = (freq == 2484) ? 14 : (freq < 2500) ? int((freq - 2407) / 5) : (freq < 5925) ? int((freq - 5000) / 5) : int((freq - 5950) / 5)
    auth = (kind == "probe") ? "[PROBE]" : "[ESS]"
    record("WIFI", mac, ssid, ch, freq, sig, conf, why, "", auth)
}
