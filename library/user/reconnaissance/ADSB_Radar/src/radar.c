/*
 * ADS-B Radar - live aircraft radar for the WiFi Pineapple Pager + RTL-SDR.
 *
 * stdin  : raw unsigned 8-bit I/Q from `rtl_sdr -f 1090000000 -s 2000000 -`
 * stdout : CSV position log (payload.sh redirects it into /root/loot/adsb/)
 * /tmp/adsb_ref : "mode lat_e6 lon_e6" written by payload.sh (0 none, 1 home file, 2/3 GPS fix)
 *
 * Screen (480x222 upright, rotated into the 222x480 framebuffer):
 *   left  - north-up radar scope, range rings, plane icons pointing along their track,
 *           coloured by altitude, with trails; selected plane boxed
 *   right - aircraft list (nearest first) + detail card for the selected plane
 * Controls: Up/Down select plane, Left/Right range, Green label mode, Red exit.
 *
 * Freestanding + integer-only for the FPU-less MT7628 (-DPAGER). The host build
 * (-DHOST_PREVIEW) runs the same code on a .cu8 recording and dumps PPM frames;
 * -DDEMO adds simulated aircraft so the layout can be checked without traffic.
 */
#include <stdint.h>
#ifdef PAGER
#include "minirt.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#endif
#include "adsb_core.h"
#include "font5x7.h"

#define W 480
#define H 222
static uint16_t scr[H][W];

/* ---------------- drawing ---------------- */
static inline uint16_t rgb(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255; if (g < 0) g = 0; if (g > 255) g = 255; if (b < 0) b = 0; if (b > 255) b = 255;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static int clx0 = 0, cly0 = 0, clx1 = W, cly1 = H;      // clip rectangle (x1/y1 exclusive)
static void set_clip(int x, int y, int w, int h) { clx0 = x < 0 ? 0 : x; cly0 = y < 0 ? 0 : y; clx1 = x + w > W ? W : x + w; cly1 = y + h > H ? H : y + h; }
static void clip_all(void) { clx0 = 0; cly0 = 0; clx1 = W; cly1 = H; }
static inline void px(int x, int y, uint16_t c) { if (x >= clx0 && x < clx1 && y >= cly0 && y < cly1) scr[y][x] = c; }
static void hline(int x, int y, int w, uint16_t c) { for (int i = 0; i < w; i++) px(x + i, y, c); }
static void vline(int x, int y, int h, uint16_t c) { for (int i = 0; i < h; i++) px(x, y + i, c); }
static void fillrect(int x, int y, int w, int h, uint16_t c) { for (int j = 0; j < h; j++) hline(x, y + j, w, c); }
static void box(int x, int y, int w, int h, uint16_t c) { hline(x, y, w, c); hline(x, y + h - 1, w, c); vline(x, y, h, c); vline(x + w - 1, y, h, c); }
static void line(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) { px(x0, y0, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
}
static void circle(int cx, int cy, int r, uint16_t c, int dash) {   // midpoint; dash>0 skips points
    int x = r, y = 0, e = 1 - r, k = 0;
    while (x >= y) {
        if (!dash || ((k++ / dash) & 1) == 0) {
            px(cx + x, cy + y, c); px(cx - x, cy + y, c); px(cx + x, cy - y, c); px(cx - x, cy - y, c);
            px(cx + y, cy + x, c); px(cx - y, cy + x, c); px(cx + y, cy - x, c); px(cx - y, cy - x, c);
        }
        y++; if (e < 0) e += 2 * y + 1; else { x--; e += 2 * (y - x) + 1; }
    }
}
static int text(int x, int y, const char *s, uint16_t c, int sc) {
    int cx = x;
    for (; *s; s++) {
        char ch = *s; if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch >= FONT_FIRST && ch < FONT_FIRST + FONT_COUNT) {
            const unsigned char *g = FONT5X7[ch - FONT_FIRST];
            for (int r = 0; r < 7; r++) for (int col = 0; col < 5; col++) if (g[r] & (1 << (4 - col)))
                { if (sc == 1) px(cx + col, y + r, c); else fillrect(cx + col * sc, y + r * sc, sc, sc, c); }
        }
        cx += 6 * sc;
    }
    return cx;
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void text_r(int xr, int y, const char *s, uint16_t c, int sc) { text(xr - slen(s) * 6 * sc + sc, y, s, c, sc); }

/* tiny formatter shared by host + device */
#ifdef PAGER
#define FMT sprintf
#else
#define FMT(b, ...) snprintf(b, 64, __VA_ARGS__)
#endif

/* ---------------- colours ---------------- */
static uint16_t C_BG, C_GRID, C_RING, C_DIM, C_TXT, C_HI, C_CY, C_WARN, C_RED, C_SEL, C_PANEL;
static void init_colors(void) {
    C_BG = rgb(3, 8, 6); C_GRID = rgb(0, 50, 30); C_RING = rgb(0, 95, 55); C_DIM = rgb(70, 110, 90);
    C_TXT = rgb(190, 225, 205); C_HI = rgb(255, 255, 255); C_CY = rgb(60, 230, 200); C_WARN = rgb(255, 190, 40);
    C_RED = rgb(255, 50, 40); C_SEL = rgb(255, 255, 120); C_PANEL = rgb(8, 20, 16);
}
static uint16_t alt_color(int alt, int bright) {   // low = orange, mid = green, high = cyan/blue, top = violet
    int r, g, b;
    if (alt == NOALT) { r = 160; g = 160; b = 160; }
    else if (alt < 2000) { r = 255; g = 140; b = 30; }
    else if (alt < 10000) { int t = (alt - 2000) * 255 / 8000; r = 255 - t; g = 140 + t * 100 / 255; b = 30; }
    else if (alt < 25000) { int t = (alt - 10000) * 255 / 15000; r = 0; g = 240 - t / 4; b = 30 + t * 200 / 255; }
    else if (alt < 40000) { int t = (alt - 25000) * 255 / 15000; r = t * 180 / 255; g = 180 - t / 2; b = 255; }
    else { r = 200; g = 90; b = 255; }
    if (!bright) { r = r * 2 / 5; g = g * 2 / 5; b = b * 2 / 5; }
    return rgb(r, g, b);
}

/* ---------------- geometry ---------------- */
static int ref_mode;              // 0 none, 1 home file, 2/3 GPS fix, 9 auto (first plane)
static int32_t ref_lat, ref_lon;
static int have_ref;
static int coslat_q14 = 16384;
static void set_ref(int32_t la, int32_t lo) {
    ref_lat = la; ref_lon = lo; have_ref = 1;
    int d = (int)(((int64_t)(la < 0 ? -(int64_t)la : la) * 360) >> 32);   // whole degrees
    coslat_q14 = SIN360[(90 - d + 360) % 360];
}
// offsets in metres east / north of the reference
static void enu(int32_t la, int32_t lo, int64_t *e, int64_t *n) {
    int32_t dla = la - ref_lat, dlo = lo - ref_lon;           // int32 wrap handles the antimeridian
    *n = ((int64_t)dla * 40007863) >> 32;
    *e = ((((int64_t)dlo * 40075016) >> 32) * coslat_q14) >> 14;
}

/* ---------------- state ---------------- */
static const int RANGE_NM[] = {5, 10, 25, 50, 100, 200};
#define NRANGE 6
static int range_i = 2;
static int label_mode = 0;        // 0 callsign, 1 altitude, 2 none
static int filt_ground, filt_alt, filt_kind, filt_emerg;   // menu filters (see views.h)
static uint32_t sel_icao;
static int frame_n;
static int src_net;
static long rate_prev_ok, rate_prev_t; static int msg_rate; static int rx_pct = -1;
#define SCX 111
#define SCY 111
#define SCR 106

typedef struct { ac_t *a; int64_t dist; int brg; int sx, sy, on; } view_t;
static int passes_filters(const ac_t *a);
static view_t vis[MAXAC]; static int nvis;

static void build_view(void) {
    nvis = 0;
    long range_m = (long)RANGE_NM[range_i] * 1852;
    for (int i = 0; i < nac; i++) {
        ac_t *a = &acs[i];
        if (t_samples - a->seen > 60L * FS) continue;
        if (a->msgs < 2 && !a->has_pos) continue;
        if (!passes_filters(a)) continue;
        view_t *v = &vis[nvis++]; v->a = a; v->dist = -1; v->on = 0; v->brg = 0;
        if (a->has_pos && have_ref) {
            int64_t e, n; enu(a->lat, a->lon, &e, &n);
            v->dist = isqrt64((uint64_t)(e * e + n * n));
            v->brg = bearing(e, n);
            v->sx = SCX + (int)(e * SCR / range_m); v->sy = SCY - (int)(n * SCR / range_m);
            int64_t dx = v->sx - SCX, dy = v->sy - SCY;
            v->on = dx * dx + dy * dy <= (int64_t)(SCR - 2) * (SCR - 2);
        }
    }
    // sort: positioned (nearest first), then the rest by message count
    for (int i = 1; i < nvis; i++) {
        view_t t = vis[i]; int j = i - 1;
        while (j >= 0) {
            int64_t da = vis[j].dist < 0 ? (1LL << 60) - vis[j].a->msgs : vis[j].dist + (vis[j].a->ground ? (1LL << 50) : 0);
            int64_t db = t.dist < 0 ? (1LL << 60) - t.a->msgs : t.dist + (t.a->ground ? (1LL << 50) : 0);
            if (da <= db) break; vis[j + 1] = vis[j]; j--;
        }
        vis[j + 1] = t;
    }
}
static int sel_index(void) { for (int i = 0; i < nvis; i++) if (vis[i].a->icao == sel_icao) return i; return -1; }

static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0); if (!area) return;
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        int w0 = (x1 - x) * (y2 - y) - (x2 - x) * (y1 - y), w1 = (x2 - x) * (y0 - y) - (x0 - x) * (y2 - y), w2 = (x0 - x) * (y1 - y) - (x1 - x) * (y0 - y);
        if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0)) px(x, y, c);
    }
}
// arrowhead pointing along the track: nose, right wing, notch, left wing (nose-up frame, y- = forward)
static void plane_icon(int x, int y, int hdg, uint16_t c, int big) {
    static const int8_t P[4][2] = { {0, -8}, {4, 6}, {0, 3}, {-4, 6} };
    int s = SIN360[hdg % 360], co = SIN360[(hdg + 90) % 360], k = big ? 4 : 3, X[4], Y[4];
    for (int i = 0; i < 4; i++) {
        int ax = P[i][0] * k, ay = P[i][1] * k;   // scale x3 (or x4) then /3 below
        X[i] = x + (((ax * co - ay * s) >> 14) / 3); Y[i] = y + (((ax * s + ay * co) >> 14) / 3);
    }
    fill_tri(X[0], Y[0], X[1], Y[1], X[2], Y[2], c);
    fill_tri(X[0], Y[0], X[2], Y[2], X[3], Y[3], c);
}
static void blip(int x, int y, uint16_t c) { fillrect(x - 2, y - 2, 5, 5, c); }

/* ---- aircraft silhouettes by emitter category (nose up, units ~1 px, rotated by track) ---- */
static const int8_t SH_AIRLINER[] = { 0,-8, 1,-6, 1,-2, 7,2, 7,3, 1,1, 1,5, 3,7, 3,8, 0,7, -3,8, -3,7, -1,5, -1,1, -7,3, -7,2, -1,-2, -1,-6 };
static const int8_t SH_HEAVY[]    = { 0,-10, 1,-8, 2,-3, 10,3, 10,4, 2,1, 1,6, 4,8, 4,9, 0,8, -4,9, -4,8, -1,6, -2,1, -10,4, -10,3, -2,-3, -1,-8 };
static const int8_t SH_LIGHT[]    = { 0,-7, 1,-6, 1,-3, 8,-3, 8,-1, 1,-1, 1,4, 3,5, 3,6, -3,6, -3,5, -1,4, -1,-1, -8,-1, -8,-3, -1,-3, -1,-6 };
static const int8_t SH_GLIDER[]   = { 0,-6, 1,-5, 1,-2, 10,-2, 10,-1, 1,-1, 1,5, 3,6, 3,7, -3,7, -3,6, -1,5, -1,-1, -10,-1, -10,-2, -1,-2, -1,-5 };
static const int8_t SH_FIGHTER[]  = { 0,-9, 1,-5, 6,3, 6,5, 2,5, 2,7, -2,7, -2,5, -6,5, -6,3, -1,-5 };
static const int8_t SH_HELI[]     = { 0,-4, 2,-2, 2,2, 1,3, 1,8, -1,8, -1,3, -2,2, -2,-2 };
static void fill_poly(const int *vx, const int *vy, int n, uint16_t c) {   // even-odd scanline fill, Q4 coords
    int miny = 1 << 30, maxy = -(1 << 30);
    for (int i = 0; i < n; i++) { if (vy[i] < miny) miny = vy[i]; if (vy[i] > maxy) maxy = vy[i]; }
    for (int y = (miny >> 4); y <= (maxy >> 4) + 1; y++) {
        int yc = y * 16 + 8, xs[24], k = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            if ((vy[i] > yc) != (vy[j] > yc) && k < 24)
                xs[k++] = vx[j] + (int)((int64_t)(vx[i] - vx[j]) * (yc - vy[j]) / (vy[i] - vy[j]));
        }
        for (int a = 1; a < k; a++) { int v = xs[a], b = a - 1; while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; } xs[b + 1] = v; }
        for (int a = 0; a + 1 < k; a += 2) for (int x = (xs[a] + 8) >> 4; x <= (xs[a + 1] - 8) >> 4; x++) px(x, y, c);
    }
}
static void shape(int x, int y, int hdg, const int8_t *pts, int n, int q, uint16_t c) {   // q: Q4 px per unit
    int X[24], Y[24], s = SIN360[hdg % 360], co = SIN360[(hdg + 90) % 360];
    for (int i = 0; i < n; i++) {
        int ax = pts[2 * i] * q, ay = pts[2 * i + 1] * q;
        X[i] = x * 16 + 8 + ((ax * co - ay * s) >> 14); Y[i] = y * 16 + 8 + ((ax * s + ay * co) >> 14);
    }
    fill_poly(X, Y, n, c);
}
static int heli_type(const char *t) {   // common helicopter ICAO type codes (internet feed, when no category)
    static const char *HELI[] = { "R22", "R44", "R66", "B06", "B407", "B412", "B429", "B505", "EC20", "EC30", "EC35", "EC45", "EC55", "EC75",
                               "H125", "H130", "H135", "H145", "H160", "AS50", "AS55", "AS65", "A109", "A119", "A139", "A169", "S76", "S92", "H60", "UH1", "BK17", 0 };
    for (int i = 0; HELI[i]; i++) { int k = 0; while (HELI[i][k] && HELI[i][k] == t[k]) k++; if (!HELI[i][k] && !t[k]) return 1; }
    return 0;
}
#define CAT(set, num) (((set) << 3) | (num))
static void type_icon(int x, int y, const ac_t *a, uint16_t c, int big) {
    int q = big ? 18 : 13, cat = a->cat;
    if (!cat && a->type[0] && heli_type(a->type)) cat = CAT(1, 7);
    if (cat == CAT(1, 7)) {                                   // rotorcraft: rotor disk + body
        circle(x, y, big ? 8 : 6, c, 1);
        shape(x, y, a->has_vel ? a->hdg : 0, SH_HELI, 9, q, c); return;
    }
    if (cat == CAT(2, 2)) { int r = big ? 5 : 4; for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) if (dx * dx + dy * dy <= r * r) px(x + dx, y - 2 + dy, c); fillrect(x - 1, y + r - 1, 3, 3, c); return; }   // balloon
    if (cat == CAT(2, 6)) { line(x - 5, y - 5, x + 5, y + 5, c); line(x - 5, y + 5, x + 5, y - 5, c);                       // drone
        for (int i = 0; i < 4; i++) circle(x + (i & 1 ? 5 : -5), y + (i & 2 ? 5 : -5), 2, c, 0); return; }
    if (cat == CAT(2, 3)) { fillrect(x - 1, y - 1, 3, 3, c); circle(x, y, 4, c, 0); return; }                              // parachutist
    if ((cat >> 3) == 3) { fillrect(x - 3, y - 2, 7, 5, c); return; }                                                    // surface vehicle
    if (!a->has_vel) { blip(x, y, c); return; }
    int h = a->hdg;
    switch (cat) {
        case CAT(1, 1): shape(x, y, h, SH_LIGHT, 17, q, c); return;                    // light
        case CAT(1, 2): shape(x, y, h, SH_AIRLINER, 18, q * 13 / 16, c); return;       // small (regional / bizjet)
        case CAT(1, 3): case CAT(1, 4): shape(x, y, h, SH_AIRLINER, 18, q, c); return;   // large
        case CAT(1, 5): shape(x, y, h, SH_HEAVY, 18, q, c); return;                    // heavy
        case CAT(1, 6): case CAT(2, 7): shape(x, y, h, SH_FIGHTER, 11, q, c); return;   // high performance / space
        case CAT(2, 1): case CAT(2, 4): shape(x, y, h, SH_GLIDER, 17, q, c); return;   // glider / ultralight
        default: plane_icon(x, y, h, c, big); return;                                 // unknown: arrow
    }
}
static int is_heli(const ac_t *a) { return a->cat == CAT(1, 7) || (!a->cat && a->type[0] && heli_type(a->type)); }
static int emergency(int sq);
static int passes_filters(const ac_t *a) {
    if (filt_emerg && !emergency(a->squawk)) return 0;
    if (filt_ground && a->ground) return 0;
    if (filt_alt == 1 && (a->ground || a->alt == NOALT || a->alt >= 10000)) return 0;
    if (filt_alt == 2 && (a->ground || a->alt == NOALT || a->alt < 10000)) return 0;
    if (filt_kind == 1 && !(a->cat >= CAT(1, 3) && a->cat <= CAT(1, 5))) return 0;                              // airliners
    if (filt_kind == 2 && !(a->cat == CAT(1, 1) || a->cat == CAT(1, 2) || a->cat == CAT(2, 1) || a->cat == CAT(2, 4))) return 0;   // light & GA
    if (filt_kind == 3 && !is_heli(a)) return 0;
    if (filt_kind == 4 && !(a->mil || a->cat == CAT(1, 6))) return 0;                                          // military
    return 1;
}
static const char *cat_name(int cat) {
    switch (cat) {
        case CAT(1, 1): return "LIGHT  <15,500 LB"; case CAT(1, 2): return "SMALL  15,500-75,000 LB";
        case CAT(1, 3): return "LARGE  75,000-300,000 LB"; case CAT(1, 4): return "LARGE  HIGH VORTEX (B757)";
        case CAT(1, 5): return "HEAVY  >300,000 LB"; case CAT(1, 6): return "HIGH PERFORMANCE"; case CAT(1, 7): return "ROTORCRAFT";
        case CAT(2, 1): return "GLIDER"; case CAT(2, 2): return "BALLOON / AIRSHIP"; case CAT(2, 3): return "PARACHUTIST";
        case CAT(2, 4): return "ULTRALIGHT"; case CAT(2, 6): return "DRONE (UAV)"; case CAT(2, 7): return "SPACE VEHICLE";
        case CAT(3, 1): return "EMERGENCY VEHICLE"; case CAT(3, 2): return "SERVICE VEHICLE"; default: return "";
    }
}

static void fmt_dist(char *b, int64_t m) {   // nautical miles, 1 decimal under 100
    if (m < 0) { b[0] = '-'; b[1] = '-'; b[2] = 0; return; }
    long t = (long)(m * 10 / 1852);
    if (t < 1000) FMT(b, "%ld.%ld", t / 10, t % 10); else FMT(b, "%ld", t / 10);
}
static const char *ac_name(ac_t *a, char *buf) { if (a->call[0]) return a->call; FMT(buf, "%06X", a->icao); return buf; }
static int emergency(int sq) { return sq == 7500 || sq == 7600 || sq == 7700; }

static void render_radar(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) scr[y][x] = C_BG;
    char b[64], b2[64];
    long range_m = (long)RANGE_NM[range_i] * 1852;
    int sel = sel_index();

    /* ---- scope ---- */
    for (int r = 1; r <= 3; r++) circle(SCX, SCY, SCR * r / 3, r == 3 ? C_RING : C_GRID, r == 3 ? 0 : 2);
    for (int d = 0; d < 360; d += 30) {   // bearing ticks
        int s = SIN360[d], c = SIN360[(d + 90) % 360];
        line(SCX + (s * (SCR - 4) >> 14), SCY - (c * (SCR - 4) >> 14), SCX + (s * SCR >> 14), SCY - (c * SCR >> 14), C_RING);
    }
    hline(SCX - 3, SCY, 7, C_DIM); vline(SCX, SCY - 3, 7, C_DIM);
    text(SCX - 2, SCY - SCR - 1 + 3, "N", C_CY, 1);
    FMT(b, "%dNM", RANGE_NM[range_i]); text(SCX + SCR * 7 / 10 + 2, SCY - SCR * 7 / 10 - 8, b, C_DIM, 1);
    FMT(b, "%d", RANGE_NM[range_i] / 3 ? RANGE_NM[range_i] / 3 : 1); (void)b2;

    if (!have_ref) {
        text(SCX - 45, SCY - 18, "WAITING FOR", C_DIM, 1);
        text(SCX - 45, SCY - 8, "A POSITION", C_DIM, 1);
        text(SCX - 45, SCY + 6, nac ? "(LISTENING)" : "(NO PLANES YET)", C_DIM, 1);
    }
    // trails, then icons, then labels
    for (int i = 0; i < nvis; i++) {
        ac_t *a = vis[i].a; if (!a->has_pos || !have_ref) continue;
        uint16_t c = alt_color(a->alt, 0);
        for (int k = 0; k < a->tr_n; k++) {
            int idx = (a->tr_head - 1 - k + TRAIL) % TRAIL; int64_t e, n; enu(a->tr_lat[idx], a->tr_lon[idx], &e, &n);
            int x = SCX + (int)(e * SCR / range_m), y = SCY - (int)(n * SCR / range_m);
            int64_t dx = x - SCX, dy = y - SCY; if (dx * dx + dy * dy < (int64_t)SCR * SCR) px(x, y, c);
        }
    }
    for (int i = nvis - 1; i >= 0; i--) {
        view_t *v = &vis[i]; ac_t *a = v->a; if (!v->on) continue;
        int stale = t_samples - a->pos_t > 20L * FS;
        int is_sel = (i == sel);
        uint16_t c = stale ? C_DIM : alt_color(a->alt, 1);
        if (emergency(a->squawk) && (frame_n & 4)) c = C_RED;
        if (a->ground && !is_sel) { fillrect(v->sx - 1, v->sy - 1, 3, 3, rgb(110, 110, 110)); continue; }
        type_icon(v->sx, v->sy, a, c, is_sel);
        if (is_sel) box(v->sx - 10, v->sy - 10, 21, 21, C_SEL);
        if (label_mode < 2 || is_sel) {
            const char *l = label_mode == 1 && a->alt != NOALT ? (FMT(b, "%d", a->alt / 100), b) : ac_name(a, b2);
            int lx = v->sx + 8; if (lx + slen(l) * 6 > SCX + SCR) lx = v->sx - 8 - slen(l) * 6;
            text(lx, v->sy - 3, l, is_sel ? C_SEL : (stale ? C_DIM : C_TXT), 1);
        }
    }
    // planes outside the range: a tick on the rim pointing at them
    for (int i = 0; i < nvis; i++) {
        view_t *v = &vis[i]; if (v->on || v->dist < 0) continue;
        int s = SIN360[v->brg], c = SIN360[(v->brg + 90) % 360];
        line(SCX + (s * (SCR + 1) >> 14), SCY - (c * (SCR + 1) >> 14), SCX + (s * (SCR + 5) >> 14), SCY - (c * (SCR + 5) >> 14), alt_color(v->a->alt, 1));
    }

    /* ---- right panel ---- */
    int PX = 228;
    fillrect(PX - 4, 0, W - PX + 4, H, C_PANEL); vline(PX - 4, 0, H, C_GRID);
    text(PX, 3, "ADS-B RADAR", C_CY, 2);
    FMT(b, "%d AC", nvis); text_r(W - 3, 3, b, C_HI, 1);
    if (src_net) FMT(b, "VIA INTERNET");
    else if (rx_pct >= 0) FMT(b, "RX %d%%  %d MSG/S", rx_pct, msg_rate); else FMT(b, "%d MSG/S", msg_rate);
    text_r(W - 3, 12, b, !src_net && rx_pct >= 0 && rx_pct < 95 ? C_WARN : C_DIM, 1);
    const char *refs = ref_mode == 9 ? "NO GPS: AUTO" : ref_mode == 0 ? "NO GPS" : ref_mode == 1 ? "HOME POS" : "GPS FIX";
    int rx_ = text(PX, 22, refs, ref_mode >= 1 && ref_mode != 9 ? C_TXT : C_WARN, 1);
    if (filt_ground || filt_alt || filt_kind || filt_emerg) text(rx_ + 6, 22, "FILTER", C_WARN, 1);
    FMT(b, "RANGE %dNM", RANGE_NM[range_i]); text_r(W - 3, 22, b, C_TXT, 1);

    // list
    int ly = 34;
    text(PX, ly, "FLIGHT       ALT   NM  KT", C_DIM, 1); hline(PX, ly + 9, W - PX - 3, C_GRID);
    int rows = 7, first = 0;
    if (sel >= rows) first = sel - rows + 1;
    for (int r = 0; r < rows && first + r < nvis; r++) {
        view_t *v = &vis[first + r]; ac_t *a = v->a; int y = ly + 12 + r * 10;
        int is_sel = (first + r == sel);
        if (is_sel) fillrect(PX - 2, y - 1, W - PX, 9, rgb(40, 60, 30));
        uint16_t c = emergency(a->squawk) ? C_RED : is_sel ? C_SEL : C_TXT;
        text(PX, y, ac_name(a, b2), c, 1);
        if (a->alt != NOALT) { FMT(b, "%d", a->alt); text_r(PX + 108, y, b, alt_color(a->alt, 1), 1); }
        fmt_dist(b, v->dist); text_r(PX + 150, y, b, c, 1);
        if (a->has_vel) { FMT(b, "%d", a->spd); text_r(PX + 174, y, b, c, 1); }
        if (a->has_vel && a->vr) text(PX + 180, y, a->vr > 0 ? "+" : "-", a->vr > 0 ? C_CY : C_WARN, 1);
    }
    if (nvis > rows) { FMT(b, "%d/%d", (sel < 0 ? 0 : sel) + 1, nvis); text_r(W - 3, ly, b, C_DIM, 1); }

    // detail card
    int cy = 120;
    hline(PX, cy - 3, W - PX - 3, C_GRID);
    if (sel >= 0) {
        view_t *v = &vis[sel]; ac_t *a = v->a;
        text(PX, cy, ac_name(a, b2), emergency(a->squawk) ? C_RED : C_SEL, 2);
        FMT(b, "%06X", a->icao); text_r(W - 3, cy, b, C_DIM, 1);
        if (a->squawk >= 0) { FMT(b, "SQ %04d", a->squawk); text_r(W - 3, cy + 9, b, emergency(a->squawk) ? C_RED : C_DIM, 1); }
        int y = cy + 22;
        if (a->alt != NOALT) FMT(b, "%d FT", a->alt); else FMT(b, "-- FT");
        text(PX, y, b, alt_color(a->alt, 1), 2);
        if (a->has_vel && a->vr) { FMT(b, "%s%d FPM", a->vr > 0 ? "+" : "", a->vr); text_r(W - 3, y + 4, b, a->vr > 0 ? C_CY : C_WARN, 1); }
        y += 20;
        if (a->has_vel) { FMT(b, "%d KT  TRK %03d", a->spd, a->hdg); text(PX, y, b, C_TXT, 1); }
        y += 11;
        if (v->dist >= 0) { fmt_dist(b2, v->dist); FMT(b, "%s NM  BRG %03d", b2, v->brg); text(PX, y, b, C_TXT, 1); }
        else text(PX, y, a->has_pos ? "POSITION, NO REF" : "NO POSITION YET", C_DIM, 1);
        y += 11;
        long age = (t_samples - a->seen) / FS;
        FMT(b, "SIG %d  MSGS %d  %lds", a->sig, a->msgs, age); text(PX, y, b, C_DIM, 1);
    } else text(PX, cy + 4, nac ? "LEFT/RIGHT: PICK A PLANE" : "LISTENING ON 1090 MHZ", C_DIM, 1);

    // emergency banner over the scope
    for (int i = 0; i < nvis; i++) if (emergency(vis[i].a->squawk)) {
        int sq = vis[i].a->squawk;
        const char *what = sq == 7700 ? "EMERGENCY" : sq == 7600 ? "RADIO FAIL" : "HIJACK";
        FMT(b, "%04d %s %s", sq, what, ac_name(vis[i].a, b2));
        if (frame_n & 4) { fillrect(2, H - 14, 220, 12, C_RED); text(5, H - 12, b, C_HI, 1); }
        break;
    }
    text(PX, H - 9, "LR PICK UD ZOOM GRN INFO RED MENU", rgb(60, 90, 75), 1);
}

#include "views.h"

/* ---------------- CSV log ---------------- */
static long epoch_now(void);
static void log_positions(void) {
    char b[160], la[64], lo[64];
    for (int i = 0; i < nac; i++) {
        ac_t *a = &acs[i];
        if (!a->has_pos || a->pos_t <= a->log_t || t_samples - a->log_t < 5L * FS) continue;
        a->log_t = t_samples;
        long y = units_e5(a->lat), x = units_e5(a->lon);
        FMT(la, "%s%ld.%05ld", y < 0 ? "-" : "", (y < 0 ? -y : y) / 100000, (y < 0 ? -y : y) % 100000);
        FMT(lo, "%s%ld.%05ld", x < 0 ? "-" : "", (x < 0 ? -x : x) / 100000, (x < 0 ? -x : x) % 100000);
#ifdef PAGER
        sprintf(b, "%ld,%06X,%s,%d,%s,%s,%d,%d,%d,%d\n", epoch_now(), a->icao, a->call,
                a->alt == NOALT ? -1 : a->alt, la, lo, a->has_vel ? a->spd : -1, a->has_vel ? a->hdg : -1, a->vr, a->squawk);
        printf("%s", b);
#else
        snprintf(b, sizeof b, "%ld,%06X,%s,%d,%s,%s,%d,%d,%d,%d\n", epoch_now(), a->icao, a->call,
                 a->alt == NOALT ? -1 : a->alt, la, lo, a->has_vel ? a->spd : -1, a->has_vel ? a->hdg : -1, a->vr, a->squawk);
        fputs(b, stdout);
#endif
    }
    fflush(stdout);
}

/* reference position: GPS / home file, else centre on the first plane */
static void apply_ref(int mode, long la_e6, long lo_e6) {
    if (mode >= 1) { ref_mode = mode; set_ref(e6_units(la_e6), e6_units(lo_e6)); rx_ref_ok = 1; rx_lat = ref_lat; rx_lon = ref_lon; return; }
    rx_ref_ok = 0;
    if (ref_mode == 9 && have_ref) return;
    ref_mode = 0; have_ref = 0;
    for (int i = 0; i < nac; i++) if (acs[i].has_pos) { ref_mode = 9; set_ref(acs[i].lat, acs[i].lon); break; }
}
static int parse_ref(const char *s, int *mode, long *la, long *lo) {
    long v[3]; int k = 0;
    while (*s && k < 3) {
        while (*s == ' ' || *s == '\n' || *s == '\t') s++;
        if (!*s) break;
        int neg = 0; if (*s == '-') { neg = 1; s++; }
        long n = 0; int any = 0; while (*s >= '0' && *s <= '9') { n = n * 10 + (*s++ - '0'); any = 1; }
        if (!any) return 0;
        v[k++] = neg ? -n : n;
    }
    if (k < 3) return 0;
    *mode = (int)v[0]; *la = v[1]; *lo = v[2]; return 1;
}

#ifdef DEMO
/* simulated traffic around the reference, for layout checks */
static void demo_tick(void) {
    static int init; static int32_t blat, blon;
    if (!have_ref && !init) return;
    if (!init) { init = 1; blat = ref_lat; blon = ref_lon; }
    static const struct { uint32_t icao; const char *call; int alt, spd, hdg, vr, sq; int e_km, n_km; } D[] = {
        {0xA1B2C3, "UAL1532", 36000, 470, 75, 0, 3412, -60, -20}, {0xA0FFEE, "AAL2210", 11800, 290, 300, 1600, 4721, 25, -15},
        {0xACE123, "N512TX", 2500, 110, 40, -300, 1200, -8, 6}, {0xA44444, "SWA3920", 23000, 410, 180, -2000, 6631, 10, 40},
        {0xA77777, "", 6000, 150, 250, 0, -1, 30, 25}, {0xAB0B0B, "DAL880", 41000, 480, 120, 0, 7700, -35, 45},
    };
    long tsec = t_samples / FS;
    for (unsigned i = 0; i < sizeof D / sizeof D[0]; i++) {
        ac_t *a = find_ac(D[i].icao, 1);
        a->msgs++; a->seen = t_samples; a->sig = 40 + (int)i * 20;
        int k = 0; while (D[i].call[k] && k < 8) { a->call[k] = D[i].call[k]; k++; } a->call[k] = 0;
        a->alt = D[i].alt + D[i].vr * (int)tsec / 60; a->spd = D[i].spd; a->hdg = D[i].hdg; a->vr = D[i].vr; a->has_vel = 1; a->squawk = D[i].sq;
        long mps10 = D[i].spd * 5144 / 1000;   // knots -> dm/s
        long e_m = D[i].e_km * 1000L + mps10 * tsec * SIN360[D[i].hdg] / 16384 / 10;
        long n_m = D[i].n_km * 1000L + mps10 * tsec * SIN360[(D[i].hdg + 90) % 360] / 16384 / 10;
        int32_t la = blat + (int32_t)(((int64_t)n_m << 32) / 40007863);
        int32_t lo = blon + (int32_t)((((int64_t)e_m << 32) / 40075016) * 16384 / coslat_q14);
        set_pos(a, la, lo);
    }
}
#endif

static void update_rate(void) {
    if (t_samples - rate_prev_t >= FS) {
        msg_rate = (int)((st_ok - rate_prev_ok) * FS / (t_samples - rate_prev_t));
        rate_prev_ok = st_ok; rate_prev_t = t_samples;
    }
}

/* ======================= platform: Pager ======================= */
#ifdef PAGER
static long sys5(long n, long a, long b, long c, long d, long e) {
    register long r2 asm("$2") = n, r4 asm("$4") = a, r5 asm("$5") = b, r6 asm("$6") = c, r7 asm("$7") = d;
    asm volatile("addiu $sp,$sp,-32\n sw %5,16($sp)\n syscall\n addiu $sp,$sp,32"
                 : "+r"(r2), "+r"(r7) : "r"(r4), "r"(r5), "r"(r6), "r"(e)
                 : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "memory");
    return r7 ? -r2 : r2;
}
#define SYS_open 4005
#define SYS_close 4006
#define SYS_lseek 4019
#define SYS_fcntl 4055
#define SYS_poll 4188
#define SYS_clock_gettime 4263
#define O_RDONLY 0
#define O_RDWR 2
#define O_NONBLOCK 0x80
#define K_UP 103
#define K_DOWN 108
#define K_LEFT 105
#define K_RIGHT 106
#define K_GREEN 305
#define K_RED 304
struct ts { long sec, nsec; };
static long epoch_now(void) { struct ts t; sys3(SYS_clock_gettime, 0, (long)&t, 0); return t.sec; }
static int now_ms(void) { struct ts t; sys3(SYS_clock_gettime, 1, (long)&t, 0); return (int)(t.sec * 1000 + t.nsec / 1000000); }
static long load_file(const char *path, uint8_t *buf, long cap) {
    int fd = (int)sys3(SYS_open, (long)path, O_RDONLY, 0); if (fd < 0) return 0;
    long n = rt_readfull(fd, buf, cap); sys3(SYS_close, fd, 0, 0); return n < 0 ? 0 : n;
}
static int save_file(const char *path, const char *buf, int n) {
    int fd = (int)sys3(SYS_open, (long)path, 1 | 0x100 | 0x200, 0644); if (fd < 0) return -1;   // O_WRONLY|O_CREAT|O_TRUNC
    long w = sys3(SYS_write, fd, (long)buf, n); sys3(SYS_close, fd, 0, 0); return (int)w;
}
static uint16_t fbuf[W * H];
static void blit(int fb) {   // upright 480x222 -> native 222x480 portrait
    for (int y = 0; y < H; y++) { const uint16_t *row = scr[y]; for (int x = 0; x < W; x++) fbuf[x * H + (H - 1 - y)] = row[x]; }
    sys3(SYS_lseek, fb, 0, 0); sys3(SYS_write, fb, (long)fbuf, sizeof fbuf);
}
static void read_ref(void) {
    char b[96]; int fd = (int)sys3(SYS_open, (long)"/tmp/adsb_ref", O_RDONLY, 0);
    int mode = 0; long la = 0, lo = 0;
    if (fd >= 0) { long n = sys3(SYS_read, fd, (long)b, sizeof b - 1); sys3(SYS_close, fd, 0, 0);
        if (n > 0) { b[n] = 0; if (!parse_ref(b, &mode, &la, &lo)) mode = 0; } }
    apply_ref(mode, la, lo);
}
#define CHUNK (64 * 1024)                 /* samples per decode step (32 ms) */
static uint8_t raw[CHUNK * 2 + 2];
static uint16_t mag[CHUNK + TAIL];
int main(void) {
    int fb = (int)sys3(SYS_open, (long)"/dev/fb0", O_RDWR, 0); if (fb < 0) return 1;
    int ev = (int)sys3(SYS_open, (long)"/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    long fl = sys3(SYS_fcntl, 0, 3, 0); sys3(SYS_fcntl, 0, 4, fl | O_NONBLOCK);
    init_colors(); build_lut(); build_crc(); map_load("/tmp/adsb_map.bin");
    { uint8_t sb[8]; long n = load_file("/tmp/adsb_source", sb, sizeof sb); src_net = n >= 3 && sb[0] == 'n'; }
    load_ui();
    { long n = load_file("/tmp/adsb_notice", (uint8_t *)notice, sizeof notice - 1);
      if (n > 0) { notice[n] = 0; for (long i = 0; i < n; i++) if (notice[i] == '\n') notice[i] = 0; notice_ms = 6000; }
      sys3(4010 /* unlink */, (long)"/tmp/adsb_notice", 0, 0); }
    int rawn = 0, have = 0, eof = 0, last_draw = 0, last_ref = -10000;
    int rx_t0 = now_ms(); long rx_s0 = 0, t_start = now_ms(), last_net = -10000;
    if (src_net) eof = 1;                           // internet: no I/Q on stdin
    int rc = 0;
    for (;;) {
        struct { int fd; short ev, rev; } p[2] = { {ev, 1, 0}, {0, 1, 0} };
        sys3(SYS_poll, (long)p, eof ? 1 : 2, eof ? 100 : 30);
        if (src_net) {                              // clock from the wall, data from the downloaded file
            int nw = now_ms(); t_samples = (long)(nw - t_start) * (FS / 1000);
            if (nw - last_net >= 1000) { net_poll("/tmp/adsb_net.json"); read_net_status(); last_net = nw; }
            net_extrapolate();
        }
        // samples
        while (!eof) {
            long r = sys3(SYS_read, 0, (long)(raw + rawn), CHUNK * 2 - rawn);
            if (r == 0) { eof = 1; break; }
            if (r < 0) break;                          // EAGAIN
            rawn += (int)r;
            if (rawn < CHUNK * 2) continue;
            for (int i = 0; i < CHUNK; i++) mag[have + i] = maglut[raw[2 * i] * 256 + raw[2 * i + 1]];
            long n = have + CHUNK;
            demod(mag, n);
            t_samples += CHUNK;
            memmove(mag, mag + n - TAIL, TAIL * 2); have = TAIL;
            rawn = 0;
            break;                                     // one chunk per pass keeps the UI responsive
        }
        // keys
        int up = 0, dn = 0, lf = 0, rt = 0, ga = 0, red = 0, gh = 0, rh = 0, quit = 0;
        int now = now_ms();
        unsigned char e[16];
        // D-pad acts on press. Green/Red act on release (tap) or after HOLD_MS while still down (hold).
        static int kdn[6], kt0[6], kfired[6], krep[6];   // up down left right green red
        while (ev >= 0 && sys3(SYS_read, ev, (long)e, 16) == 16) {
            int type = e[8] | (e[9] << 8), code = e[10] | (e[11] << 8), val = e[12] | (e[13] << 8);
            if (type != 1 || val == 2) continue;
            int k = code == K_UP ? 0 : code == K_DOWN ? 1 : code == K_LEFT ? 2 : code == K_RIGHT ? 3 : code == K_GREEN ? 4 : code == K_RED ? 5 : -1;
            if (k < 0) continue;
            if (val == 1) {
                kdn[k] = 1; kt0[k] = now; kfired[k] = 0; krep[k] = now;
                if (k == 0) up++; else if (k == 1) dn++; else if (k == 2) lf++; else if (k == 3) rt++;
            } else {
                if (kdn[k] && k >= 4 && !kfired[k]) { if (k == 4) ga++; else red++; }
                kdn[k] = 0;
            }
        }
        if (kdn[4] && !kfired[4] && now - kt0[4] >= 600) { kfired[4] = 1; gh++; }
        if (kdn[5] && !kfired[5] && now - kt0[5] >= 600) { kfired[5] = 1; rh++; }
        if (pan_mode) for (int k = 0; k < 4; k++)       // held D-pad keeps the map gliding
            if (kdn[k] && now - kt0[k] >= 350 && now - krep[k] >= 90) {
                krep[k] = now; pan_by(k == 2 ? -W / 10 : k == 3 ? W / 10 : 0, k == 0 ? -H / 10 : k == 1 ? H / 10 : 0);
                last_draw = now - 1000;                  // redraw now
            }
        if (now - last_ref > 1000) { read_ref(); last_ref = now; }
        if (!src_net && now - rx_t0 >= 5000) {   // share of the 2 MS/s stream that reached us (drops show up here)
            rx_pct = (int)((t_samples - rx_s0) * 100 / ((long)(now - rx_t0) * (FS / 1000)));
            if (rx_pct > 100) rx_pct = 100;
            rx_t0 = now; rx_s0 = t_samples;
        }
        if (up || dn || lf || rt || ga || red || gh || rh || now - last_draw >= 250) {
            build_view(); quit = on_keys(up, dn, lf, rt, ga, red, gh, rh); build_view();
            if (quit) { rc = quit == 10 ? 10 : 0; break; }
            if (sel_index() < 0 && nvis) sel_icao = vis[0].a->icao;
            update_rate(); prune(300L * FS); log_positions();
            if (notice_ms > 0) notice_ms -= now - last_draw > 1000 ? 250 : now - last_draw;
            render(); if (eof && !src_net) text(4, H - 10, "SDR STOPPED", C_RED, 1);
            blit(fb); frame_n++; last_draw = now;
        }
    }
    fflush(stdout);
    return rc;
}

/* ======================= platform: host preview ======================= */
#else
static long epoch_now(void) { return 1795000000L + t_samples / FS; }
static long load_file(const char *path, uint8_t *buf, long cap) {
    FILE *f = fopen(path, "rb"); if (!f) return 0; long n = (long)fread(buf, 1, cap, f); fclose(f); return n;
}
static int save_file(const char *path, const char *buf, int n) {
    FILE *f = fopen(path, "wb"); if (!f) return -1; int w = (int)fwrite(buf, 1, n, f); fclose(f); return w;
}
static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint16_t c = scr[y][x]; unsigned char p[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
        fwrite(p, 1, 3, f);
    }
    fclose(f);
}
// usage: radar_host file.cu8 outdir [ref_mode lat_e6 lon_e6] [keys]
//   keys: string applied one per frame: u d l r g(green) b(red) G/B (green/red held), '.' = nothing
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s file.cu8 outdir [mode lat_e6 lon_e6] [keys]\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    int mode = 0; long la = 0, lo = 0; const char *keys = "";
    if (argc >= 6) { mode = atoi(argv[3]); la = atol(argv[4]); lo = atol(argv[5]); }
    if (argc >= 7) keys = argv[6];
    init_colors(); build_lut(); build_crc();
    if (getenv("ADSB_MAP")) map_load(getenv("ADSB_MAP"));
    const char *netjson = getenv("ADSB_NET");      // host test of the internet source: a saved feed file
    if (netjson) { src_net = 1; net_state = 1; }
    static uint8_t raw[200000 * 2]; static uint16_t mag[200000 + TAIL];
    int have = 0, fr = 0; size_t got;
    while ((got = fread(raw, 2, 200000, f)) > 0 || fr < 60) {   // 0.1 s per frame; keep going past EOF
        for (size_t i = 0; i < got; i++) mag[have + i] = maglut[raw[2 * i] * 256 + raw[2 * i + 1]];
        long n = have + (long)got;
        if (got) { demod(mag, n); memmove(mag, mag + n - TAIL, TAIL * 2); have = TAIL; }
        t_samples += 200000;
        apply_ref(mode, la, lo);
        if (netjson && fr == 0) { net_poll(netjson); net_epoch = epoch_now() - 2; }
        if (netjson) net_extrapolate();
#ifdef DEMO
        demo_tick();
#endif
        char k = fr < (int)strlen(keys) ? keys[fr] : 0;
        build_view();
        on_keys(k == 'u', k == 'd', k == 'l', k == 'r', k == 'g', k == 'b', k == 'G', k == 'B');
        build_view(); if (sel_index() < 0 && nvis) sel_icao = vis[0].a->icao;
        update_rate(); log_positions(); render(); frame_n++;
        char p[512]; snprintf(p, sizeof p, "%s/f%03d.ppm", argv[2], fr); write_ppm(p);
        fr++;
        if (!got && fr >= 60) break;
    }
    fprintf(stderr, "frames %d  ok %ld  pos %ld  aircraft %d\n", fr, st_ok, st_pos, nac);
    if (getenv("ADSB_DEBUG")) for (int i = 0; i < nac; i++) fprintf(stderr, "  %06X %-8s pos=%d vel=%d spd=%d hdg=%d\n", acs[i].icao, acs[i].call, acs[i].has_pos, acs[i].has_vel, acs[i].spd, acs[i].hdg);
    return 0;
}
#endif
