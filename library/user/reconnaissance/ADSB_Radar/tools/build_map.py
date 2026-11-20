#!/usr/bin/env python3
"""build_map.py - make a regional map pack (map.bin) for the Pager ADS-B Radar.

Sources (public domain): Natural Earth 10m (states/provinces lines, coastline,
lakes, rivers, roads, populated places) and OurAirports (airports, runways).

    python3 -I build_map.py --src ../mapdata/src --lat 39.74 --lon -104.99 --radius 350 -o map.bin
    python3 -I build_map.py --src ../mapdata/src --us -o map.bin        # lower 48 + Alaska + Hawaii

map.bin layout v2 (little endian, every record 4-byte aligned; angles are int32
"units" where 2^32 = 360 degrees):
    'ADSM' u16 version=2 u16 nlines u16 nlabels u16 pad i32 clat i32 clon
    lines : u8 layer u8 0 u16 npts i16 minlat i16 maxlat i16 minlon i16 maxlon  (bbox in units>>16, floor/ceil)
            first point i32 lat, i32 lon (units); then per point i16 dlat, i16 dlon in units>>8
            (~2.4 m steps); a pair (-32768, -32768) escapes to an absolute i32 lat, i32 lon
    labels: u8 kind u8 rank u8 len u8 0 i32 lat i32 lon, name bytes padded to 4
layers: 1 state/province border, 2 coastline, 3 lake shore, 4 river,
        5 major highway, 6 secondary highway, 7 runway
label kinds: 1 city (rank 0 = biggest), 2 airport (rank 0 large, 1 medium)
"""
import argparse, csv, json, math, os, struct, sys, unicodedata

U = 2**32 / 360.0
def units(deg):
    v = int(round(deg * U)) & 0xFFFFFFFF
    return v - 2**32 if v >= 2**31 else v

def km_xy(lat, lon, clat, clon):
    return ((lon - clon) * 111.32 * math.cos(math.radians(clat)), (lat - clat) * 110.57)

def dp(points, tol_km, clat, clon):
    """Douglas-Peucker on (lat, lon) using local km coordinates."""
    if len(points) < 3:
        return points
    xy = [km_xy(a, b, clat, clon) for a, b in points]
    keep = [False] * len(points); keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        i, j = stack.pop()
        (x1, y1), (x2, y2) = xy[i], xy[j]
        dx, dy = x2 - x1, y2 - y1; L = math.hypot(dx, dy) or 1e-9
        best, bi = -1, -1
        for k in range(i + 1, j):
            x, y = xy[k]
            d = abs(dy * x - dx * y + x2 * y1 - y2 * x1) / L
            if d > best: best, bi = d, k
        if best > tol_km:
            keep[bi] = True; stack += [(i, bi), (bi, j)]
    return [p for p, k in zip(points, keep) if k]

def lines_of(geom):
    t, c = geom["type"], geom["coordinates"]
    if t == "LineString": return [c]
    if t == "MultiLineString": return c
    if t == "Polygon": return c
    if t == "MultiPolygon": return [ring for poly in c for ring in poly]
    return []

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True); ap.add_argument("-o", "--out", default="map.bin")
    ap.add_argument("--lat", type=float); ap.add_argument("--lon", type=float)
    ap.add_argument("--us", action="store_true", help="lower 48 + Alaska + Hawaii (with border areas)")
    ap.add_argument("--radius", type=float, default=350, help="km")
    ap.add_argument("--tol", type=float, default=0.15, help="simplify tolerance, km")
    a = ap.parse_args()
    if a.us:
        boxes = [(24.0, 50.0, -125.5, -66.0), (51.0, 71.6, -179.9, -129.5), (18.5, 22.6, -160.6, -154.5)]
        a.lat, a.lon = 39.0, -98.0                          # header centre (fallback view)
    else:
        if a.lat is None or a.lon is None: ap.error("--lat/--lon or --us")
        dlat = a.radius / 110.57; dlon = a.radius / (111.32 * math.cos(math.radians(a.lat)))
        boxes = [(a.lat - dlat, a.lat + dlat, a.lon - dlon, a.lon + dlon)]
    inside = lambda la, lo: any(b[0] <= la <= b[1] and b[2] <= lo <= b[3] for b in boxes)

    out_lines = []   # (layer, [(lat, lon)])
    def add_line(layer, coords):
        # split into runs inside the box (keep one point beyond each edge so lines reach the border)
        run = []
        pts = [(c[1], c[0]) for c in coords]
        for i, p in enumerate(pts):
            near = inside(*p) or (i > 0 and inside(*pts[i - 1])) or (i + 1 < len(pts) and inside(*pts[i + 1]))
            if near: run.append(p)
            elif run:
                if len(run) > 1: out_lines.append((layer, run))
                run = []
        if len(run) > 1: out_lines.append((layer, run))

    def geo(name):
        with open(os.path.join(a.src, name + ".geojson")) as f:
            return json.load(f)["features"]

    for name, layer, filt in [
        ("ne_10m_admin_1_states_provinces_lines", 1, None),
        ("ne_10m_coastline", 2, None),
        ("ne_10m_lakes", 3, None),
        ("ne_10m_rivers_lake_centerlines", 4, None),
        ("ne_10m_roads", 5, lambda p: p.get("type") in ("Major Highway", "Secondary Highway", "Beltway")),
    ]:
        n0 = len(out_lines)
        for ft in geo(name):
            p = ft.get("properties") or {}
            if filt and not filt(p): continue
            lay = layer
            if name == "ne_10m_roads": lay = 5 if p.get("type") in ("Major Highway", "Beltway") else 6
            for line in lines_of(ft["geometry"] or {"type": "", "coordinates": []}):
                add_line(lay, line)
        print(f"{name}: {len(out_lines) - n0} lines", file=sys.stderr)

    labels = []      # (kind, rank, lat, lon, name)
    for ft in geo("ne_10m_populated_places_simple"):
        p = ft["properties"]; lo, la = ft["geometry"]["coordinates"][:2]
        if not inside(la, lo): continue
        pop = p.get("pop_max") or 0
        rank = 0 if pop >= 1000000 else 1 if pop >= 250000 else 2 if pop >= 50000 else 3
        labels.append((1, rank, la, lo, (p.get("name") or "")[:20]))

    ap_ids = {}
    with open(os.path.join(a.src, "airports.csv"), newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["type"] not in ("large_airport", "medium_airport"): continue
            try: la, lo = float(r["latitude_deg"]), float(r["longitude_deg"])
            except ValueError: continue
            if not inside(la, lo): continue
            code = r["iata_code"] or r["ident"]
            labels.append((2, 0 if r["type"] == "large_airport" else 1, la, lo, code[:8]))
            ap_ids[r["id"]] = 1
    nrw = 0
    with open(os.path.join(a.src, "runways.csv"), newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["airport_ref"] not in ap_ids or r["closed"] == "1": continue
            try: p1 = (float(r["le_latitude_deg"]), float(r["le_longitude_deg"])); p2 = (float(r["he_latitude_deg"]), float(r["he_longitude_deg"]))
            except ValueError: continue
            out_lines.append((7, [p1, p2])); nrw += 1
    print(f"labels: {len(labels)}  runways: {nrw}", file=sys.stderr)

    # simplify (runways untouched) and drop degenerate lines
    final = []
    for layer, pts in out_lines:
        if layer != 7: pts = dp(pts, a.tol, pts[0][0], pts[0][1])
        if len(pts) >= 2: final.append((layer, pts))
    # big lines get split so the 16-bit point count never overflows
    split = []
    for layer, pts in final:
        while len(pts) > 4000:
            split.append((layer, pts[:4000])); pts = pts[3999:]
        split.append((layer, pts))
    final = split
    # draw order: water/borders first, roads, runways last
    final.sort(key=lambda t: {3: 0, 4: 1, 2: 2, 1: 3, 6: 4, 5: 5, 7: 6}[t[0]])

    out = bytearray(b"ADSM") + struct.pack("<HHHHii", 2, len(final), len(labels), 0, units(a.lat), units(a.lon))
    npts = nesc = 0
    for layer, pts in final:
        u = [(units(la), units(lo)) for la, lo in pts]
        lats = [p[0] for p in u]; lons = [p[1] for p in u]
        out += struct.pack("<BBHhhhh", layer, 0, len(u), min(lats) >> 16, -((-max(lats)) >> 16), min(lons) >> 16, -((-max(lons)) >> 16))
        out += struct.pack("<ii", u[0][0], u[0][1])
        qla, qlo = u[0][0] >> 8, u[0][1] >> 8
        for la, lo in u[1:]:
            dla, dlo = (la >> 8) - qla, (lo >> 8) - qlo
            if -32767 <= dla <= 32767 and -32767 <= dlo <= 32767:
                out += struct.pack("<hh", dla, dlo); qla += dla; qlo += dlo
            else:
                out += struct.pack("<hhii", -32768, -32768, la, lo); qla, qlo = la >> 8, lo >> 8; nesc += 1
        npts += len(u)
    for kind, rank, la, lo, name in labels:
        plain = "".join(c for c in unicodedata.normalize("NFKD", name) if not unicodedata.combining(c))
        nb = plain.upper().encode("ascii", "replace")[:24]
        out += struct.pack("<BBBBii", kind, rank, len(nb), 0, units(la), units(lo)) + nb + b"\0" * ((-len(nb)) % 4)
    with open(a.out, "wb") as f: f.write(out)
    print(f"wrote {a.out}: {len(final)} lines, {npts} points ({nesc} long jumps), {len(labels)} labels, {len(out)} bytes", file=sys.stderr)

if __name__ == "__main__":
    main()
