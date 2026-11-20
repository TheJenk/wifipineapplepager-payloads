/* views.h - MAP / INFO / menu screens + controls for radar.c (included once, after render_radar). */

/* ======================= map pack ======================= */
#define MAP_CAP (1536 * 1024)
static uint8_t map_mem[MAP_CAP];
static long map_len;
static int map_nlines, map_nlabels;
static long map_lab_off;
static int32_t map_clat, map_clon;
static long load_file(const char *path, uint8_t *buf, long cap);   // platform

static int32_t rd32(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }
static int rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

#define MAXLINES 16384
static uint32_t line_off[MAXLINES];
static void map_load(const char *path) {
    map_len = load_file(path, map_mem, MAP_CAP);
    if (map_len < 20 || map_mem[0] != 'A' || map_mem[1] != 'D' || map_mem[2] != 'S' || map_mem[3] != 'M' || rd16(map_mem + 4) != 2) { map_len = 0; return; }
    map_nlines = rd16(map_mem + 6); map_nlabels = rd16(map_mem + 8);
    map_clat = rd32(map_mem + 12); map_clon = rd32(map_mem + 16);
    if (map_nlines > MAXLINES) { map_len = 0; return; }
    long off = 20;
    for (int i = 0; i < map_nlines; i++) {           // index lines (point records vary in size)
        if (off + 20 > map_len) { map_len = 0; return; }
        line_off[i] = (uint32_t)off;
        int n = rd16(map_mem + off + 2); off += 12 + 8;
        for (int k = 1; k < n; k++) {
            if (off + 4 > map_len) { map_len = 0; return; }
            off += (rd16(map_mem + off) == 0x8000 && rd16(map_mem + off + 2) == 0x8000) ? 12 : 4;
        }
    }
    if (off > map_len) { map_len = 0; return; }
    map_lab_off = off;
}

/* ======================= viewport projection ======================= */
typedef struct { int x0, y0, w, h; int32_t clat, clon; int mpp; int64_t fx, fy; int32_t hlat, hlon; } vp_t;
static void vp_init(vp_t *v, int x0, int y0, int w, int h, int32_t clat, int32_t clon, int mpp) {
    v->x0 = x0; v->y0 = y0; v->w = w; v->h = h; v->clat = clat; v->clon = clon; v->mpp = mpp;
    int d = (int)(((int64_t)(clat < 0 ? -(int64_t)clat : clat) * 360) >> 32);
    int cs = SIN360[(90 - d + 360) % 360]; if (cs < 200) cs = 200;
    v->fy = ((int64_t)40007863 << 8) / mpp;                         // y px = dlat * fy >> 40
    v->fx = (((int64_t)40075016 * cs >> 14) << 8) / mpp;            // x px = dlon * fx >> 40
    v->hlat = (int32_t)(((int64_t)(h / 2 + 8) << 40) / v->fy);     // half extents (+margin) in units
    int64_t hl = ((int64_t)(w / 2 + 8) << 40) / v->fx; v->hlon = hl > 0x7fffffff ? 0x7fffffff : (int32_t)hl;
}
static int clampi(int64_t v) { return v > 30000 ? 30000 : v < -30000 ? -30000 : (int)v; }
static void vp_xy(const vp_t *v, int32_t la, int32_t lo, int *x, int *y) {
    int32_t dla = la - v->clat, dlo = lo - v->clon;
    *x = v->x0 + v->w / 2 + clampi(((int64_t)dlo * v->fx) >> 40);
    *y = v->y0 + v->h / 2 - clampi(((int64_t)dla * v->fy) >> 40);
}
static int vp_inside(const vp_t *v, int x, int y, int m) { return x >= v->x0 - m && x < v->x0 + v->w + m && y >= v->y0 - m && y < v->y0 + v->h + m; }

/* Cohen-Sutherland against the viewport, then Bresenham */
static int oc(const vp_t *v, int x, int y) {
    int c = 0; if (x < v->x0) c |= 1; else if (x >= v->x0 + v->w) c |= 2; if (y < v->y0) c |= 4; else if (y >= v->y0 + v->h) c |= 8; return c;
}
static void cline(const vp_t *v, int x0, int y0, int x1, int y1, uint16_t col) {
    int c0 = oc(v, x0, y0), c1 = oc(v, x1, y1);
    for (int it = 0; it < 6; it++) {
        if (!(c0 | c1)) { line(x0, y0, x1, y1, col); return; }
        if (c0 & c1) return;
        int c = c0 ? c0 : c1; int64_t x, y;
        int xmin = v->x0, xmax = v->x0 + v->w - 1, ymin = v->y0, ymax = v->y0 + v->h - 1;
        if (c & 8) { y = ymax; x = x0 + (int64_t)(x1 - x0) * (ymax - y0) / (y1 - y0); }
        else if (c & 4) { y = ymin; x = x0 + (int64_t)(x1 - x0) * (ymin - y0) / (y1 - y0); }
        else if (c & 2) { x = xmax; y = y0 + (int64_t)(y1 - y0) * (xmax - x0) / (x1 - x0); }
        else { x = xmin; y = y0 + (int64_t)(y1 - y0) * (xmin - x0) / (x1 - x0); }
        if (c == c0) { x0 = (int)x; y0 = (int)y; c0 = oc(v, x0, y0); } else { x1 = (int)x; y1 = (int)y; c1 = oc(v, x1, y1); }
    }
}

/* simple label declutter: remember placed boxes for this frame */
#define MAXBOX 96
static int16_t bx0[MAXBOX], by0[MAXBOX], bx1[MAXBOX], by1[MAXBOX]; static int nbox;
static int place(int x, int y, int w, int h) {
    for (int i = 0; i < nbox; i++) if (x < bx1[i] && x + w > bx0[i] && y < by1[i] && y + h > by0[i]) return 0;
    if (nbox < MAXBOX) { bx0[nbox] = x; by0[nbox] = y; bx1[nbox] = x + w; by1[nbox] = y + h; nbox++; }
    return 1;
}

static uint16_t L_COL[8];
static void draw_map(const vp_t *v, int with_labels) {
    if (!map_len) return;
    if (!L_COL[1]) {
        L_COL[1] = rgb(50, 80, 150); L_COL[2] = rgb(70, 130, 210); L_COL[3] = rgb(45, 95, 170); L_COL[4] = rgb(30, 60, 110);
        L_COL[5] = rgb(105, 85, 55); L_COL[6] = rgb(60, 52, 38); L_COL[7] = rgb(185, 185, 195);
    }
    int64_t la0 = (int64_t)v->clat - v->hlat, la1 = (int64_t)v->clat + v->hlat;
    int64_t lo0 = (int64_t)v->clon - v->hlon, lo1 = (int64_t)v->clon + v->hlon;
    for (int i = 0; i < map_nlines; i++) {
        const uint8_t *h = map_mem + line_off[i]; int layer = h[0], n = rd16(h + 2);
        if (layer == 6 && v->mpp > 400) continue;                       // secondary highways: close in
        if ((layer == 5 || layer == 4) && v->mpp > 1600) continue;      // highways, rivers: not at country scale
        if (layer == 7 && v->mpp > 200) continue;                       // runways: very close
        int64_t blat0 = (int64_t)(int16_t)rd16(h + 4) << 16, blat1 = (int64_t)(int16_t)rd16(h + 6) << 16;
        int64_t blon0 = (int64_t)(int16_t)rd16(h + 8) << 16, blon1 = (int64_t)(int16_t)rd16(h + 10) << 16;
        if (blat1 < la0 || blat0 > la1 || blon1 < lo0 || blon0 > lo1) continue;
        const uint8_t *p = h + 12;
        int32_t la = rd32(p), lo = rd32(p + 4); p += 8;
        int32_t qla = la >> 8, qlo = lo >> 8;
        int px0, py0; vp_xy(v, la, lo, &px0, &py0);
        for (int k = 1; k < n; k++) {
            int d1 = (int16_t)rd16(p), d2 = (int16_t)rd16(p + 2);
            if (d1 == -32768 && d2 == -32768) { qla = rd32(p + 4) >> 8; qlo = rd32(p + 8) >> 8; p += 12; }
            else { qla += d1; qlo += d2; p += 4; }
            int x, y; vp_xy(v, (int32_t)((uint32_t)qla << 8), (int32_t)((uint32_t)qlo << 8), &x, &y);
            if (x != px0 || y != py0) {
                cline(v, px0, py0, x, y, L_COL[layer]);
                if (layer == 7) cline(v, px0 + 1, py0, x + 1, y, L_COL[layer]);   // runways 2 px wide
                px0 = x; py0 = y;
            }
        }
    }
    if (!with_labels) return;
    long lo = map_lab_off;
    for (int i = 0; i < map_nlabels; i++) {
        const uint8_t *h = map_mem + lo; int kind = h[0], rank = h[1], len = h[2];
        lo += 12 + ((len + 3) & ~3);
        int show = kind == 2 ? (rank == 0 ? v->mpp <= 800 : v->mpp <= 200)
                             : (rank <= (v->mpp >= 800 ? 0 : v->mpp >= 400 ? 1 : v->mpp >= 100 ? 2 : 3));
        if (kind == 1 && v->mpp >= 12800) show = 0;                         // country scale: shapes only
        if (!show) continue;
        int x, y; vp_xy(v, rd32(h + 4), rd32(h + 8), &x, &y);
        if (!vp_inside(v, x, y, 0)) continue;
        char nm[28]; int k; for (k = 0; k < len && k < 27; k++) nm[k] = (char)h[12 + k]; nm[k] = 0;
        int tw = len * 6;
        if (kind == 2) {
            if (!place(x - 2, y - 2, tw + 8, 9)) continue;
            box(x - 2, y - 2, 5, 5, rgb(170, 150, 255)); text(x + 5, y - 3, nm, rgb(170, 150, 255), 1);
        } else {
            if (!place(x - 1, y - 1, tw + 6, 9)) continue;
            fillrect(x - 1, y - 1, 2, 2, rgb(150, 155, 170)); text(x + 3, y - 3, nm, rgb(115, 120, 140), 1);
        }
    }
}

/* ======================= aircraft facts ======================= */
static const struct { uint32_t lo, hi; const char *name; } COUNTRY[] = {
    {0xA00000, 0xAFFFFF, "UNITED STATES"}, {0xC00000, 0xC3FFFF, "CANADA"}, {0x0D0000, 0x0D7FFF, "MEXICO"},
    {0x400000, 0x43FFFF, "UNITED KINGDOM"}, {0x3C0000, 0x3FFFFF, "GERMANY"}, {0x380000, 0x3BFFFF, "FRANCE"},
    {0x300000, 0x33FFFF, "ITALY"}, {0x340000, 0x37FFFF, "SPAIN"}, {0x480000, 0x487FFF, "NETHERLANDS"},
    {0x4B0000, 0x4B7FFF, "SWITZERLAND"}, {0x4B8000, 0x4BFFFF, "TURKEY"}, {0x4CA000, 0x4CAFFF, "IRELAND"},
    {0x100000, 0x1FFFFF, "RUSSIA"}, {0x780000, 0x7BFFFF, "CHINA"}, {0x7C0000, 0x7FFFFF, "AUSTRALIA"},
    {0x800000, 0x83FFFF, "INDIA"}, {0x840000, 0x87FFFF, "JAPAN"}, {0x718000, 0x71FFFF, "SOUTH KOREA"},
    {0x896000, 0x896FFF, "UAE"}, {0x06A000, 0x06AFFF, "QATAR"}, {0xC80000, 0xC87FFF, "NEW ZEALAND"},
    {0xE00000, 0xE3FFFF, "ARGENTINA"}, {0xE40000, 0xE7FFFF, "BRAZIL"},
};
static const char *country(uint32_t icao) {
    for (unsigned i = 0; i < sizeof COUNTRY / sizeof COUNTRY[0]; i++) if (icao >= COUNTRY[i].lo && icao <= COUNTRY[i].hi) return COUNTRY[i].name;
    return "";
}
/* US civil registration from the 24-bit address (A00001..ADF7C7 -> N1..N99999) */
static int n_number(uint32_t icao, char *o) {
    static const char L[] = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    const int SUF = 601, B4 = 35, B3 = 10 * 35 + 601, B2 = 10 * B3 + 601, B1 = 10 * B2 + 601;
    long i = (long)icao - 0xA00001; int n = 0;
    if (i < 0 || i >= 915399) return 0;
    o[n++] = 'N'; o[n++] = (char)('1' + i / B1); long r = i % B1;
    const int bs[2] = {B2, B3};
    for (int k = 0; k <= 2; k++) {
        if (r < SUF) {
            if (r) { o[n++] = L[(r - 1) / 25]; if ((r - 1) % 25) o[n++] = L[(r - 1) % 25 - 1]; }
            o[n] = 0; return 1;
        }
        r -= SUF;
        if (k < 2) { o[n++] = (char)('0' + r / bs[k]); r %= bs[k]; }
        else { o[n++] = (char)('0' + r / B4); r %= B4; if (r) o[n++] = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789"[r - 1]; o[n] = 0; return 1; }
    }
    o[n] = 0; return 1;
}
static const char *squawk_meaning(int sq) {
    switch (sq) { case 7700: return "EMERGENCY"; case 7600: return "RADIO FAILURE"; case 7500: return "HIJACK";
        case 1200: return "VFR"; case 7000: return "VFR (EU)"; case 2000: return "NO CODE ASSIGNED"; default: return ""; }
}

/* ======================= source + saved UI state ======================= */
static char notice[64]; static int notice_ms;        // one-off message from payload.sh
/* src_net (radar.c): 0 = radio (RTL-SDR), 1 = internet feed */
static int net_state;               // 0 waiting, 1 ok, 2 no internet, 3 no position
static long net_epoch;              // wall time of the last good download
static char net_feed[16] = "INTERNET";
static int save_file(const char *path, const char *buf, int n);   // platform
#include "net.h"

static int rd_ints(const char *path, long *v, int max) {   // whitespace-separated integers
    static uint8_t b[256]; long n = load_file(path, b, sizeof b - 1); if (n <= 0) return 0; b[n] = 0;
    int k = 0; const char *s = (const char *)b;
    while (*s && k < max) {
        while (*s == ' ' || *s == '\n') s++;
        if (!*s) break;
        int neg = 0; if (*s == '-') { neg = 1; s++; }
        long x = 0; int any = 0; while (*s >= '0' && *s <= '9') { x = x * 10 + (*s++ - '0'); any = 1; }
        if (!any) break;
        v[k++] = neg ? -x : x;
    }
    return k;
}
static void read_net_status(void) {
    static uint8_t b[64]; long n = load_file("/tmp/adsb_net.status", b, sizeof b - 1);
    if (n <= 0) { net_state = 0; return; } b[n] = 0;
    if (b[0] == 'o') {   // "ok <epoch> <feed>"
        net_state = 1; long e = 0; int i = 3; for (; b[i] >= '0' && b[i] <= '9'; i++) e = e * 10 + (b[i] - '0'); net_epoch = e;
        if (b[i] == ' ') { int k = 0; i++; while (b[i] > ' ' && k < 15) { char ch = (char)b[i++]; net_feed[k++] = ch >= 'a' && ch <= 'z' ? ch - 32 : ch; } net_feed[k] = 0; }
    }
    else if (b[0] == 'e') net_state = 2;
    else if (b[0] == 'n') net_state = 3;
}

/* ======================= screens ======================= */
enum { V_MAP, V_RADAR, V_INFO };
static int view = V_MAP, prev_view = V_MAP;
static const int MPP[] = {25, 50, 100, 200, 400, 800, 1600, 3200, 6400, 12800};   // metres per pixel
#define NMPP 10
#ifndef START_ZOOM
#define START_ZOOM 3
#endif
static int mpp_i = START_ZOOM;
static int menu_open, menu_sel;
static int follow;                 // map centres on the selected plane

static void map_center(int32_t *la, int32_t *lo) {
    int s = sel_index();
    if (follow && s >= 0 && vis[s].a->has_pos) { *la = vis[s].a->lat; *lo = vis[s].a->lon; }
    else if (have_ref) { *la = ref_lat; *lo = ref_lon; }          // GPS, home.txt, or the first plane heard
    else if (map_len) { *la = map_clat; *lo = map_clon; }
    else { *la = 0; *lo = 0; }
}
static void scale_bar(int xr, int y, int mpp) {   // a round number of NM about 60 px long
    static const int NM[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};
    int nm = 1; for (int i = 0; i < 10; i++) if ((long)NM[i] * 1852 / mpp <= 70) nm = NM[i];
    int len = (int)((long)nm * 1852 / mpp); char b[64]; FMT(b, "%d NM", nm);
    hline(xr - len, y, len, C_TXT); vline(xr - len, y - 2, 3, C_TXT); vline(xr - 1, y - 2, 3, C_TXT);
    text_r(xr - len - 4, y - 4, b, C_TXT, 1);
}
static long epoch_now(void);
static void filter_summary(char *o) {
    static const char *AL[] = { "", "<10K", ">10K" }, *KD[] = { "", "AIRLINERS", "LIGHT/GA", "HELI", "MILITARY" };
    int n = 0; o[0] = 0;
    const char *parts[4] = { filt_emerg ? "EMERG" : "", filt_ground ? "NO GND" : "", AL[filt_alt], KD[filt_kind] };
    for (int i = 0; i < 4; i++) if (parts[i][0]) { if (n) o[n++] = ' '; for (int k = 0; parts[i][k]; k++) o[n++] = parts[i][k]; }
    o[n] = 0;
}
static void top_bar(const char *title) {
    char b[64]; fillrect(0, 0, W, 11, rgb(0, 0, 0));
    { char f[48]; filter_summary(f); if (f[0]) { char t[64]; FMT(t, "FILTER %s", f); text_r(W - 3, 2, t, C_WARN, 1); } }
    text(3, 2, title, C_CY, 1);
    int x = 3 + slen(title) * 6 + 8;
    FMT(b, "%d AC", nvis); x = text(x, 2, b, C_HI, 1) + 8;
    const char *refs = ref_mode == 9 || ref_mode == 0 ? "NO GPS" : ref_mode == 1 ? "HOME" : "GPS";
    x = text(x, 2, refs, ref_mode >= 1 && ref_mode != 9 ? C_DIM : C_WARN, 1) + 8;
    if (src_net) {
        if (net_state == 1) { long age = epoch_now() - net_epoch; FMT(b, "%s %lds", net_feed, age < 0 ? 0 : age); text(x, 2, b, age > 30 ? C_WARN : C_CY, 1); }
        else text(x, 2, net_state == 2 ? "FEEDS UNREACHABLE/BUSY" : net_state == 3 ? "INTERNET: NEEDS GPS/HOME" : "INTERNET...", C_WARN, 1);
        return;
    }
    if (rx_pct >= 0) { FMT(b, "RX %d%%", rx_pct); x = text(x, 2, b, rx_pct < 95 ? C_WARN : C_DIM, 1) + 8; }
    FMT(b, "%d MSG/S", msg_rate); text(x, 2, b, C_DIM, 1);
}
static void emergency_banner(int y) {
    char b[64], b2[64];
    for (int i = 0; i < nvis; i++) if (emergency(vis[i].a->squawk)) {
        int sq = vis[i].a->squawk;
        FMT(b, "%04d %s %s", sq, squawk_meaning(sq), ac_name(vis[i].a, b2));
        if (frame_n & 4) { fillrect(0, y, W, 12, C_RED); text(4, y + 2, b, C_HI, 1); }
        return;
    }
}
static void draw_plane_on(const vp_t *v, ac_t *a, int is_sel, int label) {
    int x, y; vp_xy(v, a->lat, a->lon, &x, &y);
    if (a->ground && !is_sel) { fillrect(x - 1, y - 1, 3, 3, rgb(120, 120, 120)); return; }   // parked / taxiing
    int stale = t_samples - a->pos_t > 20L * FS;
    uint16_t c = stale ? C_DIM : alt_color(a->alt, 1);
    if (emergency(a->squawk) && (frame_n & 4)) c = C_RED;
    type_icon(x, y, a, c, is_sel);
    if (is_sel) box(x - 10, y - 10, 21, 21, C_SEL);
    if (label) {
        char b[64], b2[64]; const char *l = label_mode == 1 && a->alt != NOALT ? (FMT(b, "%d", a->alt / 100), b) : ac_name(a, b2);
        int lx = x + 8; if (lx + slen(l) * 6 > v->x0 + v->w) lx = x - 8 - slen(l) * 6;
        fillrect(lx - 1, y - 4, slen(l) * 6 + 1, 9, C_BG);
        text(lx, y - 3, l, is_sel ? C_SEL : stale ? C_DIM : C_TXT, 1);
    }
}

static void render_map(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) scr[y][x] = rgb(2, 6, 14);
    vp_t v; int32_t cla, clo; map_center(&cla, &clo);
    vp_init(&v, 0, 0, W, H, cla, clo, MPP[mpp_i]);
    nbox = 0; place(0, 0, W, 11); place(0, H - 13, W, 13); place(W - 150, 11, 150, 24);   // keep bars + scale clear
    if (have_ref && ref_mode != 9) { int x, y; vp_xy(&v, ref_lat, ref_lon, &x, &y); place(x - 7, y - 7, 15, 15); }
    static uint8_t lbl_ok[MAXAC];
    int sel0 = sel_index();
    for (int pass = 0; pass < 2; pass++)   // selected plane first, then the rest; labels only where they fit
        for (int i = 0; i < nvis; i++) {
            if ((pass == 0) != (i == sel0)) continue;
            ac_t *a = vis[i].a; lbl_ok[i] = 0; if (!a->has_pos) continue; int x, y; vp_xy(&v, a->lat, a->lon, &x, &y);
            if (!vp_inside(&v, x, y, 0)) continue;
            if (a->ground && i != sel0) continue;
            place(x - 7, y - 7, 15, 15);
            char t[64], t2[64]; const char *l = label_mode == 1 && a->alt != NOALT ? (FMT(t, "%d", a->alt / 100), t) : ac_name(a, t2);
            int lw = slen(l) * 6, lx = x + 8; if (lx + lw > v.x0 + v.w) lx = x - 8 - lw;
            if (i == sel0 || (label_mode < 2 && place(lx > x ? lx : lx - 1, y - 4, lw + 1, 9))) lbl_ok[i] = 1;
        }
    set_clip(0, 0, W, H); draw_map(&v, 1);
    int sel = sel_index();
    // receiver
    if (have_ref && ref_mode != 9) { int x, y; vp_xy(&v, ref_lat, ref_lon, &x, &y);
        hline(x - 4, y, 9, C_HI); vline(x, y - 4, 9, C_HI); circle(x, y, 6, C_HI, 0); }
    if (!map_len) text(W / 2 - 66, H / 2 + 20, "NO MAP.BIN - PLANES ONLY", C_DIM, 1);
    // trails
    for (int i = 0; i < nvis; i++) { ac_t *a = vis[i].a; if (!a->has_pos || a->ground) continue;
        for (int k = 0; k < a->tr_n; k++) { int idx = (a->tr_head - 1 - k + TRAIL) % TRAIL, x, y;
            vp_xy(&v, a->tr_lat[idx], a->tr_lon[idx], &x, &y); px(x, y, alt_color(a->tr_alt[idx], 0)); } }
    // planes: others first, selected on top
    for (int i = nvis - 1; i >= 0; i--) { ac_t *a = vis[i].a; if (!a->has_pos || i == sel) continue;
        int x, y; vp_xy(&v, a->lat, a->lon, &x, &y);
        if (vp_inside(&v, x, y, 0)) draw_plane_on(&v, a, 0, lbl_ok[i]);
        else {   // edge marker pointing toward an off-screen plane
            int ex = x < 4 ? 4 : x > W - 5 ? W - 5 : x, ey = y < 14 ? 14 : y > H - 16 ? H - 16 : y;
            fillrect(ex - 1, ey - 1, 3, 3, alt_color(a->alt, 1));
        }
    }
    if (sel >= 0 && vis[sel].a->has_pos) {
        ac_t *a = vis[sel].a; int x, y; vp_xy(&v, a->lat, a->lon, &x, &y);
        if (vp_inside(&v, x, y, 0)) draw_plane_on(&v, a, 1, 1);
        else { int ex = x < 6 ? 6 : x > W - 7 ? W - 7 : x, ey = y < 16 ? 16 : y > H - 18 ? H - 18 : y; box(ex - 4, ey - 4, 9, 9, C_SEL); }
    }
    top_bar("MAP");
    scale_bar(W - 4, 20, MPP[mpp_i]);
    // bottom strip: selected plane
    fillrect(0, H - 12, W, 12, rgb(0, 0, 0));
    if (sel >= 0) {
        ac_t *a = vis[sel].a; char b[96], n[64], d[64];
        fmt_dist(d, vis[sel].dist);
        int x = text(3, H - 10, ac_name(a, n), C_SEL, 1) + 8;
        if (a->alt != NOALT) { FMT(b, "%d FT", a->alt); x = text(x, H - 10, b, alt_color(a->alt, 1), 1) + 8; }
        if (a->has_vel) { FMT(b, "%d KT  TRK %03d", a->spd, a->hdg); x = text(x, H - 10, b, C_TXT, 1) + 8; }
        if (vis[sel].dist >= 0 && ref_mode != 9) { FMT(b, "%s NM", d); x = text(x, H - 10, b, C_TXT, 1) + 8; }
        else if (!a->has_pos) x = text(x, H - 10, "NO POSITION", C_DIM, 1) + 8;
        text_r(W - 3, H - 10, "GRN INFO", C_DIM, 1);
    } else if (src_net && net_state == 3) text(3, H - 10, "INTERNET MODE: PLUG IN GPS OR ADD HOME.TXT", C_WARN, 1);
    else if (src_net && net_state == 2) text(3, H - 10, "NO DATA: CHECK WI-FI, OR FEEDS ARE RATE LIMITING", C_WARN, 1);
    else if (!have_ref) text(3, H - 10, "NO GPS: MAP CENTRES ON THE FIRST PLANE HEARD", C_WARN, 1);
    else text(3, H - 10, nac ? "LEFT/RIGHT: PICK A PLANE" : "LISTENING ON 1090 MHZ...", C_DIM, 1);
    if (follow) text_r(W - 4, 30, "FOLLOWING", C_SEL, 1);
    emergency_banner(12);
}

static void render_info(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) scr[y][x] = C_PANEL;
    int sel = sel_index(); char b[96], b2[64];
    if (sel < 0) { text(10, 10, "PLANE LOST - RED TO GO BACK", C_DIM, 1); return; }
    view_t *vw = &vis[sel]; ac_t *a = vw->a;
    // ---- left column
    text(4, 3, ac_name(a, b2), emergency(a->squawk) ? C_RED : C_SEL, 3);
    int y = 27;
    char reg[12]; int has_reg = n_number(a->icao, reg);
    if (a->reg[0]) { memcpy(reg, a->reg, 12); has_reg = 1; }
    FMT(b, "%06X", a->icao); int x = text(4, y, b, C_DIM, 1) + 8;
    if (has_reg) {   // registration, dim when the callsign already is the registration
        int same = 1; for (int k = 0; reg[k] || a->call[k]; k++) if (reg[k] != a->call[k]) { same = 0; break; }
        x = text(x, y, reg, same ? C_DIM : C_TXT, 1) + 8;
    }
    if (a->type[0]) x = text(x, y, a->type, C_CY, 1) + 8;
    text(x, y, country(a->icao), C_DIM, 1);
    y += 11;
    if (a->mil) { text(4, y, "MILITARY", C_RED, 1); y += 11; }
    if (!a->desc[0] && cat_name(is_heli(a) ? CAT(1, 7) : a->cat)[0]) { text(4, y, cat_name(is_heli(a) ? CAT(1, 7) : a->cat), C_TXT, 1); y += 11; }
    if (a->desc[0] || a->op[0]) {
        char d[64]; int n = 0;
        for (int k = 0; a->desc[k] && n < 38; k++) d[n++] = a->desc[k];
        if (a->desc[0] && a->op[0] && n < 36) { d[n++] = ' '; d[n++] = '/'; d[n++] = ' '; }
        for (int k = 0; a->op[k] && n < 38; k++) d[n++] = a->op[k];
        d[n] = 0; text(4, y, d, C_TXT, 1); y += 11;
    }
    if (a->squawk >= 0) { FMT(b, "SQUAWK %04d %s", a->squawk, squawk_meaning(a->squawk)); text(4, y, b, emergency(a->squawk) ? C_RED : C_TXT, 1); }
    else text(4, y, "SQUAWK --", C_DIM, 1);
    y += 13;
    if (a->ground) FMT(b, "ON GROUND"); else if (a->alt != NOALT) FMT(b, "%d FT", a->alt); else FMT(b, "-- FT");
    text(4, y, b, a->ground ? C_DIM : alt_color(a->alt, 1), 3);
    y += 25;
    if (a->has_vel && a->vr) { FMT(b, "%s %d FT/MIN", a->vr > 0 ? "CLIMB" : "DESCEND", a->vr < 0 ? -a->vr : a->vr); text(4, y, b, a->vr > 0 ? C_CY : C_WARN, 1); }
    else if (a->has_vel) text(4, y, "LEVEL", C_DIM, 1);
    y += 12;
    if (a->has_vel) { FMT(b, "%d KT", a->spd); text(4, y, b, C_TXT, 2); FMT(b, "TRK %03d", a->hdg); text(110, y, b, C_TXT, 2); }
    else text(4, y, "SPEED --", C_DIM, 2);
    y += 20;
    if (vw->dist >= 0 && ref_mode != 9) { fmt_dist(b2, vw->dist); FMT(b, "%s NM  BRG %03d", b2, vw->brg); text(4, y, b, C_TXT, 2); }
    else text(4, y, a->has_pos ? "NO GPS: DISTANCE UNKNOWN" : "NO POSITION YET", C_DIM, 1);
    y += 20;
    // altitude history (trail samples, oldest left)
    int gx = 4, gy = y, gw = 232, gh = H - 24 - y;
    box(gx, gy, gw, gh, C_GRID);
    int amin = 1 << 30, amax = -(1 << 30), np = 0;
    for (int k = 0; k < a->tr_n; k++) { int al = a->tr_alt[(a->tr_head - a->tr_n + k + TRAIL) % TRAIL]; if (al == NOALT) continue; if (al < amin) amin = al; if (al > amax) amax = al; np++; }
    if (np >= 2) {
        if (amax - amin < 500) { int m = (amax + amin) / 2; amin = m - 250; amax = m + 250; }
        int lx = -1, ly = 0;
        for (int k = 0; k < a->tr_n; k++) { int al = a->tr_alt[(a->tr_head - a->tr_n + k + TRAIL) % TRAIL]; if (al == NOALT) continue;
            int xx = gx + 2 + k * (gw - 4) / (TRAIL - 1), yy = gy + gh - 3 - (int)((long)(al - amin) * (gh - 6) / (amax - amin));
            if (lx >= 0) line(lx, ly, xx, yy, alt_color(al, 1)); lx = xx; ly = yy; }
        FMT(b, "%d", amax); text(gx + gw - 2 - slen(b) * 6, gy + 2, b, C_DIM, 1);
        FMT(b, "%d", amin); text(gx + gw - 2 - slen(b) * 6, gy + gh - 9, b, C_DIM, 1);
        text(gx + 3, gy + 2, "ALTITUDE", C_DIM, 1);
    } else text(gx + 6, gy + gh / 2 - 3, "ALTITUDE HISTORY BUILDS UP", C_DIM, 1);
    // ---- right: mini map centred on the plane
    int mx = 242, my = 3, mw = W - mx - 3, mh = H - 26;
    fillrect(mx, my, mw, mh, rgb(2, 6, 14));
    if (a->has_pos) {
        // zoom so the trail fits, between 50 and 800 m/px
        int64_t far = 0;
        for (int k = 0; k < a->tr_n; k++) { int32_t dla = a->tr_lat[k] - a->lat, dlo = a->tr_lon[k] - a->lon;
            int64_t m = ((int64_t)(dla < 0 ? -dla : dla) * 40007863 >> 32) + ((int64_t)(dlo < 0 ? -dlo : dlo) * 40075016 >> 32); if (m > far) far = m; }
        int mpp = (int)(far / (mh / 2 - 12)); mpp = mpp < 150 ? 150 : mpp > 800 ? 800 : mpp;
        vp_t v; vp_init(&v, mx, my, mw, mh, a->lat, a->lon, mpp);
        set_clip(mx, my, mw, mh); nbox = 0; draw_map(&v, 1);
        if (have_ref && ref_mode != 9) { int rx, ry; vp_xy(&v, ref_lat, ref_lon, &rx, &ry); hline(rx - 3, ry, 7, C_HI); vline(rx, ry - 3, 7, C_HI); }
        for (int k = 0; k < a->tr_n; k++) { int idx = (a->tr_head - 1 - k + TRAIL) % TRAIL, tx, ty;
            vp_xy(&v, a->tr_lat[idx], a->tr_lon[idx], &tx, &ty); fillrect(tx, ty, 2, 2, alt_color(a->tr_alt[idx], 1)); }
        draw_plane_on(&v, a, 0, 0);
        clip_all(); scale_bar(mx + mw - 4, my + mh - 6, mpp);
    } else { clip_all(); text(mx + 40, my + mh / 2, "NO POSITION YET", C_DIM, 1); }
    box(mx, my, mw, mh, C_GRID);
    // ---- footer
    long age = (t_samples - a->seen) / FS, up = (t_samples - a->first) / FS;
    if (src_net) FMT(b, "VIA INTERNET  LAST %lds  TRACKED %ldm%02lds", age, up / 60, up % 60);
    else FMT(b, "SIG %d  MSGS %d  LAST %lds  TRACKED %ldm%02lds", a->sig, a->msgs, age, up / 60, up % 60);
    text(4, H - 19, b, C_DIM, 1);
    FMT(b, "%d/%d", sel + 1, nvis); text_r(W - 3, H - 19, b, C_DIM, 1);
    fillrect(0, H - 10, W, 10, rgb(0, 0, 0));
    text(3, H - 9, "LEFT/RIGHT PREV/NEXT PLANE      RED BACK", C_DIM, 1);
}

#define MENU_N 10
static void render_menu(void) {
    static const char *AL[] = { "ALL", "BELOW 10,000 FT", "ABOVE 10,000 FT" }, *KD[] = { "ALL", "AIRLINERS", "LIGHT & GA", "HELICOPTERS", "MILITARY" };
    char b[8][64];
    FMT(b[0], "VIEW:      %s", view == V_RADAR ? "RADAR" : "MAP");
    FMT(b[1], "LABELS:    %s", label_mode == 0 ? "CALLSIGN" : label_mode == 1 ? "ALTITUDE" : "OFF");
    FMT(b[2], "CENTRE:    %s", follow ? "SELECTED PLANE" : "ME / AUTO");
    FMT(b[3], "GROUND:    %s", filt_ground ? "HIDE" : "SHOW");
    FMT(b[4], "ALTITUDE:  %s", AL[filt_alt]);
    FMT(b[5], "KIND:      %s", KD[filt_kind]);
    FMT(b[6], "EMERGENCY: %s", filt_emerg ? "ONLY" : "ALL PLANES");
    FMT(b[7], "SOURCE:    %s", src_net ? "INTERNET" : "RADIO (RTL-SDR)");
    const char *it[MENU_N] = { b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], "BACK", "EXIT" };
    int w = 236, h = 12 + MENU_N * 13, x = (W - w) / 2, y = (H - h) / 2;
    fillrect(x, y, w, h, rgb(5, 15, 12)); box(x, y, w, h, C_CY);
    for (int i = 0; i < MENU_N; i++) {
        int yy = y + 8 + i * 13;
        if (i == menu_sel) fillrect(x + 4, yy - 3, w - 8, 12, rgb(40, 70, 40));
        int on = (i == 3 && filt_ground) || (i == 4 && filt_alt) || (i == 5 && filt_kind) || (i == 6 && filt_emerg);
        text(x + 10, yy, it[i], i == menu_sel ? C_SEL : on ? C_WARN : C_TXT, 1);
    }
}

static void render(void) {
    clip_all();
    if (view == V_INFO) render_info(); else if (view == V_RADAR) render_radar(); else render_map();
    clip_all();
    if (menu_open) render_menu();
    if (notice_ms > 0 && notice[0]) {
        int w = slen(notice) * 6 + 16, x = (W - w) / 2, y = H / 2 - 10;
        fillrect(x, y, w, 20, C_RED); box(x, y, w, 20, C_HI); text(x + 8, y + 7, notice, C_HI, 1);
    }
}

/* ======================= controls ======================= */
static void save_ui(void) {
    char b[96]; int n = FMT(b, "%d %d %d %d %d %d %d %d %d %d\n", view == V_INFO ? prev_view : view, mpp_i, range_i, label_mode, follow, (int)sel_icao,
                            filt_ground, filt_alt, filt_kind, filt_emerg);
    save_file("/tmp/adsb_ui", b, n);
}
static void load_ui(void) {
    long v[10]; int nv = rd_ints("/tmp/adsb_ui", v, 10); if (nv < 5) return;
    if (nv >= 10) { filt_ground = v[6] != 0; filt_alt = (int)(v[7] % 3); filt_kind = (int)(v[8] % 5); filt_emerg = v[9] != 0; }
    view = (v[0] == V_RADAR) ? V_RADAR : V_MAP; prev_view = view;
    if (v[1] >= 0 && v[1] < NMPP) mpp_i = (int)v[1];
    if (v[2] >= 0 && v[2] < NRANGE) range_i = (int)v[2];
    label_mode = (int)(v[3] % 3); follow = v[4] != 0;
}
static void step_sel(int d) {
    if (!nvis) return;
    int s = sel_index(); s = s < 0 ? 0 : (s + d % nvis + nvis) % nvis;
    sel_icao = vis[s].a->icao;
}
// returns 1 to quit
static int on_keys(int up, int dn, int lf, int rt, int ga, int red) {
    if (menu_open) {
        if (up) menu_sel = (menu_sel + MENU_N - 1) % MENU_N;
        if (dn) menu_sel = (menu_sel + 1) % MENU_N;
        if (red) menu_open = 0;
        if (ga) {
            if (menu_sel == 0) { view = view == V_RADAR ? V_MAP : V_RADAR; menu_open = 0; }
            else if (menu_sel == 1) label_mode = (label_mode + 1) % 3;
            else if (menu_sel == 2) { follow = !follow; menu_open = 0; }
            else if (menu_sel == 3) filt_ground = !filt_ground;      // filters stay open so you can stack them
            else if (menu_sel == 4) filt_alt = (filt_alt + 1) % 3;
            else if (menu_sel == 5) filt_kind = (filt_kind + 1) % 5;
            else if (menu_sel == 6) filt_emerg = !filt_emerg;
            else if (menu_sel == 7) { save_ui(); return 10; }      // payload.sh restarts us on the other source
            else if (menu_sel == 8) menu_open = 0;
            else return 1;
        }
        return 0;
    }
    if (view == V_INFO) {
        if (lf) step_sel(-1);
        if (rt) step_sel(1);
        if (red || ga) view = prev_view;
        return 0;
    }
    if (lf) step_sel(-1);
    if (rt) step_sel(1);
    if (view == V_MAP) {
        if (up && mpp_i > 0) mpp_i--;
        if (dn && mpp_i < NMPP - 1) mpp_i++;
    } else {
        if (up && range_i > 0) range_i--;
        if (dn && range_i < NRANGE - 1) range_i++;
    }
    if (ga && sel_index() >= 0) { prev_view = view; view = V_INFO; }
    if (red) { menu_open = 1; menu_sel = 0; }
    return 0;
}
