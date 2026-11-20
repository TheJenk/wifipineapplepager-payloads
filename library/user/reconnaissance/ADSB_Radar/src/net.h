/* net.h - INTERNET source: read the adsb.lol "point" JSON that payload.sh downloads to
 * /tmp/adsb_net.json and merge its aircraft into the same table the radio decoder fills.
 * Integer-only, no libc: a small scanner for this flat format (objects in "ac":[...]). */

#define NET_CAP (768 * 1024)
static uint8_t net_buf[NET_CAP];
static int64_t net_stamp = -1;         // value of "now" (ms) in the last file we merged
static int net_count;               // aircraft in the last file

static const uint8_t *ws(const uint8_t *p, const uint8_t *e) { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; return p; }
// copy a JSON string (p at the opening quote); returns pointer after the closing quote
static const uint8_t *jstr(const uint8_t *p, const uint8_t *e, char *o, int cap) {
    int n = 0; p++;
    while (p < e && *p != '"') { if (*p == '\\' && p + 1 < e) p++; if (n < cap - 1) o[n++] = (char)*p; p++; }
    if (cap) o[n] = 0;
    return p < e ? p + 1 : e;
}
// skip any JSON value (string, number, literal, array, object)
static const uint8_t *jskip(const uint8_t *p, const uint8_t *e) {
    p = ws(p, e); if (p >= e) return e;
    if (*p == '"') { char t[1]; return jstr(p, e, t, 0 + 1); }
    if (*p == '[' || *p == '{') {
        int d = 0;
        while (p < e) {
            if (*p == '"') { char t[1]; p = jstr(p, e, t, 1); continue; }
            if (*p == '[' || *p == '{') d++;
            else if (*p == ']' || *p == '}') { d--; if (!d) return p + 1; }
            p++;
        }
        return e;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}
// parse a JSON number as fixed point with `dec` decimals (e.g. dec=6 -> value*1e6); returns ok
static int jnum(const uint8_t *p, const uint8_t *e, int dec, int64_t *out) {
    p = ws(p, e); int neg = 0; int64_t v = 0; int any = 0, fd = -1;
    if (p < e && *p == '-') { neg = 1; p++; }
    for (; p < e; p++) {
        if (*p >= '0' && *p <= '9') { if (fd < 0) { v = v * 10 + (*p - '0'); any = 1; } else if (fd < dec) { v = v * 10 + (*p - '0'); fd++; any = 1; } }
        else if (*p == '.' && fd < 0) fd = 0;
        else break;
    }
    if (!any) return 0;
    if (fd < 0) fd = 0;
    while (fd < dec) { v *= 10; fd++; }
    *out = neg ? -v : v; return 1;
}
static int keyis(const char *k, const char *s) { while (*k && *k == *s) { k++; s++; } return *k == 0 && *s == 0; }

static void net_merge_one(const uint8_t *p, const uint8_t *e) {   // p after '{', e at matching '}'
    char hex[8] = "", flight[12] = "", reg[12] = "", type[8] = "", sq[8] = "", desc[24] = "", op[28] = "";
    long alt = NOALT, la = 0, lo = 0, gs = -1, trk = -1, vr = 0, seen = 0, seen_pos = -1;
    int has_la = 0, has_lo = 0, ground = 0;
    while (p < e) {
        p = ws(p, e);
        if (*p == ',') { p++; continue; }
        if (*p != '"') break;
        char k[20]; p = jstr(p, e, k, sizeof k);
        p = ws(p, e); if (p < e && *p == ':') p++; p = ws(p, e);
        if (p < e && *p == '"') {
            char v[32]; p = jstr(p, e, v, sizeof v);
            if (keyis(k, "hex")) memcpy(hex, v, 7);
            else if (keyis(k, "desc")) memcpy(desc, v, 23);
            else if (keyis(k, "ownOp")) memcpy(op, v, 27);
            else if (keyis(k, "flight")) memcpy(flight, v, 11);
            else if (keyis(k, "r")) memcpy(reg, v, 11);
            else if (keyis(k, "t")) memcpy(type, v, 7);
            else if (keyis(k, "squawk")) memcpy(sq, v, 7);
            else if (keyis(k, "alt_baro") && v[0] == 'g') ground = 1;
            continue;
        }
        int64_t n;
        if (keyis(k, "alt_baro") && jnum(p, e, 0, &n)) alt = n;
        else if (keyis(k, "lat") && jnum(p, e, 6, &n)) { la = n; has_la = 1; }
        else if (keyis(k, "lon") && jnum(p, e, 6, &n)) { lo = n; has_lo = 1; }
        else if (keyis(k, "gs") && jnum(p, e, 0, &n)) gs = n;
        else if (keyis(k, "track") && jnum(p, e, 0, &n)) trk = n;
        else if ((keyis(k, "baro_rate") || (keyis(k, "geom_rate") && !vr)) && jnum(p, e, 0, &n)) vr = n;
        else if (keyis(k, "seen") && jnum(p, e, 3, &n)) seen = n;
        else if (keyis(k, "seen_pos") && jnum(p, e, 3, &n)) seen_pos = n;
        p = jskip(p, e);
    }
    if (hex[0] == '~' || !hex[0]) return;            // non-ICAO (TIS-B) addresses: skip
    uint32_t icao = 0;
    for (int i = 0; hex[i]; i++) { char c = hex[i]; icao = icao * 16 + (uint32_t)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); }
    ac_t *a = find_ac(icao, 1);
    a->msgs++;
    a->seen = t_samples - seen * (FS / 1000);
    a->sig = 0;
    int k = 0; while (flight[k] && k < 8) { a->call[k] = flight[k]; k++; } a->call[k] = 0;
    for (k = 7; k >= 0 && (a->call[k] == ' ' || a->call[k] == 0); k--) a->call[k] = 0;
    memcpy(a->reg, reg, 11); a->reg[11] = 0;
    memcpy(a->type, type, 5); a->type[5] = 0;
    memcpy(a->desc, desc, 23); a->desc[23] = 0;
    memcpy(a->op, op, 27); a->op[27] = 0;
    a->ground = ground;
    if (ground) a->alt = 0; else if (alt != NOALT) a->alt = (int)alt;
    if (gs >= 0) { a->spd = (int)gs; a->hdg = trk >= 0 ? (int)(trk % 360) : a->hdg; a->has_vel = 1; }
    a->vr = (int)vr;
    if (sq[0]) { int v = 0; for (k = 0; sq[k]; k++) v = v * 10 + (sq[k] - '0'); a->squawk = v; }
    if (has_la && has_lo && seen_pos >= 0 && seen_pos < 60000) {
        set_pos(a, e6_units(la), e6_units(lo));
        a->pos_t = t_samples - seen_pos * (FS / 1000);
        a->b_lat = a->lat; a->b_lon = a->lon; a->b_t = a->pos_t;
    }
}

// merge /tmp/adsb_net.json if it changed; returns 1 when new data was merged
static int net_poll(const char *path) {
    long n = load_file(path, net_buf, NET_CAP - 1);
    if (n <= 0) return 0;
    const uint8_t *p = net_buf, *e = net_buf + n;
    int64_t now = -1; const uint8_t *ac = 0;
    // top level: find "ac" array and "now"
    p = ws(p, e); if (p >= e || *p != '{') return 0; p++;
    while (p < e) {
        p = ws(p, e);
        if (*p == ',') { p++; continue; }
        if (*p != '"') break;
        char k[12]; p = jstr(p, e, k, sizeof k);
        p = ws(p, e); if (p < e && *p == ':') p++; p = ws(p, e);
        if (keyis(k, "ac") || keyis(k, "aircraft")) ac = p;
        if (keyis(k, "now")) { int64_t v; if (jnum(p, e, 0, &v)) now = v; }
        p = jskip(p, e);
    }
    if (!ac || *ac != '[' || now == net_stamp) return 0;
    net_stamp = now; net_count = 0;
    p = ac + 1;
    while (p < e) {
        p = ws(p, e);
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        const uint8_t *end = jskip(p, e);          // just past the matching '}'
        net_merge_one(p + 1, end - 1); net_count++;
        p = end;
    }
    st_ok += net_count;
    return 1;
}

/* move each plane along its reported track since its last reported position, so the
 * map stays smooth between downloads (capped at 60 s of extrapolation) */
static void net_extrapolate(void) {
    for (int i = 0; i < nac; i++) {
        ac_t *a = &acs[i];
        if (!a->has_pos || !a->has_vel || a->spd <= 0 || !a->b_t || a->ground) continue;
        long dt_ms = (t_samples - a->b_t) / (FS / 1000);
        if (dt_ms < 0) dt_ms = 0; if (dt_ms > 60000) dt_ms = 60000;
        int64_t dist_mm = (int64_t)a->spd * 5144 * dt_ms / 10000;          // knots -> mm
        int64_t dn = dist_mm * SIN360[(a->hdg + 90) % 360] / 16384, de = dist_mm * SIN360[a->hdg % 360] / 16384;
        int d = (int)(((int64_t)(a->b_lat < 0 ? -(int64_t)a->b_lat : a->b_lat) * 360) >> 32);
        int cs = SIN360[(90 - d + 360) % 360]; if (cs < 500) cs = 500;
        a->lat = a->b_lat + (int32_t)((dn << 32) / 40007863000LL);
        a->lon = a->b_lon + (int32_t)(((de << 32) / 40075016000LL) * 16384 / cs);
    }
}
