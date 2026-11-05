# hits.csv -> KML: one placemark per device, at the position where it was strongest.
# hits.csv: time,lat,lon,type,mac,name,rssi,confidence,reason
BEGIN { FS = "," }
NR == 1 || $2 == "" || $3 == "" { next }
{
    k = $5; r = ($7 == "") ? -999 : $7 + 0
    if (!(k in best) || r > best[k]) { best[k] = r; row[k] = $0 }
    cnt[k]++
}
function x(s) { gsub(/&/, "\\&amp;", s); gsub(/</, "\\&lt;", s); gsub(/>/, "\\&gt;", s); return s }
END {
    print "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    print "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><name>" x(TITLE) "</name>"
    print "<Style id=\"high\"><IconStyle><color>ff0000ff</color></IconStyle></Style>"
    print "<Style id=\"medium\"><IconStyle><color>ff00a5ff</color></IconStyle></Style>"
    print "<Style id=\"low\"><IconStyle><color>ff00ffff</color></IconStyle></Style>"
    for (k in row) {
        split(row[k], f, ",")
        label = (f[6] != "") ? f[6] : f[5]
        print "<Placemark><name>" x(label) "</name><styleUrl>#" f[8] "</styleUrl>"
        print "<description>" x(f[4] " " f[5] " | " f[8] " (" f[9] ") | best " f[7] " dBm at " f[1] " UTC | " cnt[k] " sightings") "</description>"
        print "<Point><coordinates>" f[3] "," f[2] ",0</coordinates></Point></Placemark>"
    }
    print "</Document></kml>"
}
