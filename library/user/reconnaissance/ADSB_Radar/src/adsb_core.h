// adsb_core.h - integer-only Mode S / ADS-B decoder (shared by adsb.c CLI and radar.c).
// Input: magnitudes of 2 MS/s RTL-SDR I/Q. No floating point, no libc beyond memset/memmove.
// Angles are 32-bit "units": 2^32 = 360 degrees (signed int32 for lat/lon).
#ifndef ADSB_CORE_H
#define ADSB_CORE_H
#include "nl_table.h"

#define FS 2000000
#define TAIL (16 + 112 * 2 + 8)     // overlap between blocks so no message is cut
#define NOALT (-99999)

static uint16_t maglut[256 * 256];
static void build_lut(void) {
    for (int i = 0; i < 256; i++)
        for (int q = 0; q < 256; q++) {
            int di = i * 2 - 255, dq = q * 2 - 255;
            uint32_t s = (uint32_t)(di * di + dq * dq), r = 0, b = 1u << 30;
            while (b > s) b >>= 2;
            while (b) { if (s >= r + b) { s -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
            maglut[i * 256 + q] = (uint16_t)r;
        }
}

// ---- CRC + single-bit error correction ----
static uint32_t crc_table[256], syn112[112];
static uint32_t modes_crc(const uint8_t *m, int bytes) {
    uint32_t c = 0;
    for (int i = 0; i < bytes - 3; i++) c = ((c << 8) ^ crc_table[((c >> 16) ^ m[i]) & 0xFF]) & 0xFFFFFF;
    return c ^ (((uint32_t)m[bytes - 3] << 16) | (m[bytes - 2] << 8) | m[bytes - 1]);
}
static void build_crc(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i << 16;
        for (int k = 0; k < 8; k++) c = (c & 0x800000) ? ((c << 1) ^ 0xFFF409) : (c << 1);
        crc_table[i] = c & 0xFFFFFF;
    }
    for (int b = 0; b < 112; b++) {
        uint8_t t[14]; memset(t, 0, 14); t[b >> 3] = 0x80 >> (b & 7);
        syn112[b] = modes_crc(t, 14);
    }
}
static int fix1(uint8_t *msg, uint32_t rem) {   // DF17/18 only; never touches the DF field
    for (int b = 5; b < 112; b++)
        if (syn112[b] == rem) { msg[b >> 3] ^= 0x80 >> (b & 7); return b; }
    return -1;
}

// ---- CPR ----
static int cpr_nl(int32_t lat) {
    uint32_t a = lat < 0 ? (uint32_t)(-(int64_t)lat) : (uint32_t)lat;
    for (int k = 0; k < 58; k++) if (a < nl_thr[k]) return 59 - k;
    return 1;
}
static int imod(int a, int b) { int r = a % b; return r < 0 ? r + b : r; }
static int64_t fdiv(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b) < 0) q--; return q; }   // floor, b > 0
// zone index + 17-bit fraction -> angle units: (z*2^17 + frac) * 2^32 / (nz*2^17)
static int64_t zone_units(int64_t z, int frac, int nz) { return (z * 131072 + frac) * 32768 / nz; }

static int cpr_global(int lat0, int lon0, int lat1, int lon1, int odd, int32_t *olat, int32_t *olon) {
    int j = (int)fdiv((int64_t)59 * lat0 - (int64_t)60 * lat1 + 65536, 131072);
    int32_t rlat0 = (int32_t)zone_units(imod(j, 60), lat0, 60);
    int32_t rlat1 = (int32_t)zone_units(imod(j, 59), lat1, 59);
    if (cpr_nl(rlat0) != cpr_nl(rlat1)) return -1;      // straddles a zone boundary: wait
    int32_t rlat = odd ? rlat1 : rlat0;
    if (rlat > (1 << 30) || rlat < -(1 << 30)) return -1;
    int nl = cpr_nl(rlat), ni = nl - odd; if (ni < 1) ni = 1;
    int m = (int)fdiv((int64_t)lon0 * (nl - 1) - (int64_t)lon1 * nl + 65536, 131072);
    *olat = rlat;
    *olon = (int32_t)(uint32_t)zone_units(imod(m, ni), odd ? lon1 : lon0, ni);
    return 0;
}
// local decode relative to a reference within ~180 NM (we use the aircraft's own last fix)
static int cpr_local(int32_t rlat_ref, int32_t rlon_ref, int cl, int cn, int odd, int32_t *olat, int32_t *olon) {
    int nz = 60 - odd;
    // j = floor(ref/dlat) + floor(0.5 + mod(ref,dlat)/dlat - cl/2^17), in units of 1/(nz*2^17) of a circle
    int64_t refz = (int64_t)rlat_ref * nz;                 // ref in units of dlat * 2^32
    int64_t zi = fdiv(refz, 1LL << 32);
    int64_t frac17 = (refz - zi * (1LL << 32)) >> 15;      // mod(ref,dlat)/dlat as 17-bit fraction
    int64_t j = zi + fdiv(frac17 - cl + 65536, 131072);
    int64_t lat = zone_units(j, cl, nz);
    if (lat > (1 << 30) || lat < -(1 << 30)) return -1;
    int ni = cpr_nl((int32_t)lat) - odd; if (ni < 1) ni = 1;
    int64_t refx = (int64_t)rlon_ref * ni;
    int64_t xi = fdiv(refx, 1LL << 32);
    int64_t fx = (refx - xi * (1LL << 32)) >> 15;
    int64_t m = xi + fdiv(fx - cn + 65536, 131072);
    *olat = (int32_t)lat;
    *olon = (int32_t)(uint32_t)(uint64_t)zone_units(m, cn, ni);
    return 0;
}

// ---- aircraft table ----
#define TRAIL 40
typedef struct {
    uint32_t icao; int msgs; char call[9]; int alt;
    int cpr_lat[2], cpr_lon[2]; long cpr_t[2];
    int32_t lat, lon; int has_pos; long pos_t; long seen; long first; int sig;
    int spd, hdg, has_vel; int vr; int squawk;
    int32_t tr_lat[TRAIL], tr_lon[TRAIL]; int tr_alt[TRAIL]; int tr_n, tr_head; long tr_t;
    long log_t;
    char reg[12], type[6];          // from the internet feed (empty for radio)
    char desc[24], op[28];          // aircraft description + operator (adsb.fi)
    int32_t b_lat, b_lon; long b_t; // last reported position, for dead reckoning between downloads
    int ground;                     // on the ground (internet feed says "ground")
    uint8_t cat;                    // ADS-B emitter category: set (1=A 2=B 3=C 4=D) << 3 | number; 0 = unknown
    uint8_t mil;                    // military (internet feed dbFlags bit 0)
} ac_t;
#define MAXAC 256
static ac_t acs[MAXAC]; static int nac;
static long t_samples;   // sample clock (FS per second)
static long st_pre, st_ok, st_fixed, st_pos;
// receiver position (GPS / home), when known: lets one position message place a plane
static int rx_ref_ok; static int32_t rx_lat, rx_lon;

static ac_t *find_ac(uint32_t icao, int create) {
    for (int i = 0; i < nac; i++) if (acs[i].icao == icao) return &acs[i];
    if (!create) return NULL;
    if (nac == MAXAC) {   // evict the stalest
        int old = 0; for (int i = 1; i < nac; i++) if (acs[i].seen < acs[old].seen) old = i;
        acs[old] = acs[--nac];
    }
    ac_t *a = &acs[nac++];
    memset(a, 0, sizeof *a); a->icao = icao; a->alt = NOALT; a->squawk = -1;
    a->cpr_t[0] = a->cpr_t[1] = a->pos_t = a->tr_t = a->log_t = -1000000000L; a->first = t_samples;
    return a;
}
static void prune(long max_age) {   // drop aircraft not heard for max_age samples
    for (int i = 0; i < nac;) if (t_samples - acs[i].seen > max_age) acs[i] = acs[--nac]; else i++;
}
static int alt13(int a) {
    if (a & 0x40) return NOALT;                                     // metric: ignore
    if (!(a & 0x10)) return NOALT;                                  // Gillham: ignore
    return ((((a & 0x1F80) >> 2) | ((a & 0x20) >> 1) | (a & 0xF)) * 25) - 1000;
}
static int alt12(int a) { if (!(a & 0x10)) return NOALT; return (((a & 0xFE0) >> 1) | (a & 0xF)) * 25 - 1000; }
static int isqrt64(uint64_t v) { uint64_t r = 0, b = 1ULL << 62; while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return (int)r; }

// atan2 -> degrees 0..359 (0 = north / +y, clockwise toward +x)
#include "trig_tables.h"
static int atan_deg(int64_t num, int64_t den) {   // 0 <= num <= den, returns 0..45
    if (den == 0) return 0;
    int64_t t = (num << 16) / den;
    int lo = 0; while (lo < 45 && TAN46[lo + 1] <= t) lo++;
    if (lo < 45 && (t - TAN46[lo]) * 2 > (TAN46[lo + 1] - TAN46[lo])) lo++;
    return lo;
}
static int bearing(int64_t east, int64_t north) {
    if (!east && !north) return 0;
    int64_t ax = east < 0 ? -east : east, ay = north < 0 ? -north : north;
    int a = ax <= ay ? atan_deg(ax, ay) : 90 - atan_deg(ay, ax);   // from north toward east
    if (east >= 0 && north >= 0) return a;
    if (east >= 0) return 180 - a;
    if (north < 0) return 180 + a;
    return (360 - a) % 360;
}

static void set_pos(ac_t *ac, int32_t la, int32_t lo) {
    ac->lat = la; ac->lon = lo; ac->has_pos = 1; ac->pos_t = t_samples; st_pos++;
    if (t_samples - ac->tr_t >= 3L * FS) {      // trail point every 3 s
        ac->tr_lat[ac->tr_head] = la; ac->tr_lon[ac->tr_head] = lo; ac->tr_alt[ac->tr_head] = ac->alt;
        ac->tr_head = (ac->tr_head + 1) % TRAIL; if (ac->tr_n < TRAIL) ac->tr_n++;
        ac->tr_t = t_samples;
    }
}

static void handle(uint8_t *msg, int sig) {
    int df = msg[0] >> 3;
    int bytes = (df >= 16) ? 14 : 7;
    uint32_t rem = modes_crc(msg, bytes), icao;
    if (df == 17 || df == 18) {
        if (rem) { if (fix1(msg, rem) < 0) return; st_fixed++; }
        icao = (msg[1] << 16) | (msg[2] << 8) | msg[3];
    } else if (df == 11) {
        if (rem & 0xFFFF80) return;
        icao = (msg[1] << 16) | (msg[2] << 8) | msg[3];
    } else if (df == 0 || df == 4 || df == 5 || df == 16 || df == 20 || df == 21) {
        if (!find_ac(rem, 0)) return;   // address/parity: only trust aircraft already heard
        icao = rem;
    } else return;
    st_ok++;
    ac_t *ac = find_ac(icao, 1);
    ac->msgs++; ac->seen = t_samples; ac->sig = ac->msgs == 1 ? sig : (ac->sig * 3 + sig) / 4;
    if (df == 4 || df == 20) { int a = alt13(((msg[2] & 0x1F) << 8) | msg[3]); if (a != NOALT) ac->alt = a; }
    if (df == 5 || df == 21) {
        int id = ((msg[2] & 0x1F) << 8) | msg[3];
        int a = ((id >> 11) & 1) << 2 | ((id >> 9) & 1) << 1 | ((id >> 7) & 1);
        int b = ((id >> 5) & 1) << 2 | ((id >> 3) & 1) << 1 | ((id >> 1) & 1);
        int c = ((id >> 12) & 1) << 2 | ((id >> 10) & 1) << 1 | ((id >> 8) & 1);
        int d = ((id >> 4) & 1) << 2 | ((id >> 2) & 1) << 1 | (id & 1);
        ac->squawk = a * 1000 + b * 100 + c * 10 + d;
    }
    if (df != 17) return;
    int tc = msg[4] >> 3;
    if (tc >= 1 && tc <= 4) {
        if (msg[4] & 7) ac->cat = (uint8_t)(((5 - tc) << 3) | (msg[4] & 7));   // TC4=A, 3=B, 2=C, 1=D
        static const char cs[] = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";
        uint64_t v = 0; for (int i = 5; i < 11; i++) v = (v << 8) | msg[i];
        for (int i = 0; i < 8; i++) ac->call[i] = cs[(v >> (42 - 6 * i)) & 0x3F];
        ac->call[8] = 0;
        for (int i = 7; i >= 0 && ac->call[i] == ' '; i--) ac->call[i] = 0;
    } else if (tc >= 9 && tc <= 18) {
        int a = alt12((msg[5] << 4) | (msg[6] >> 4)); if (a != NOALT) ac->alt = a;
        int odd = (msg[6] >> 2) & 1;
        int cl = ((msg[6] & 3) << 15) | (msg[7] << 7) | (msg[8] >> 1);
        int cn = ((msg[8] & 1) << 16) | (msg[9] << 8) | msg[10];
        ac->cpr_lat[odd] = cl; ac->cpr_lon[odd] = cn; ac->cpr_t[odd] = t_samples;
        int32_t la, lo;
        if (ac->has_pos && t_samples - ac->pos_t < 60L * FS) {
            if (!cpr_local(ac->lat, ac->lon, cl, cn, odd, &la, &lo)) {
                int32_t dla = la - ac->lat, dlo = lo - ac->lon;
                if (dla < 0) dla = -dla; if (dlo < 0) dlo = -dlo;
                if (dla < 4000000 && dlo < 8000000) { set_pos(ac, la, lo); return; }   // < ~0.33 deg jump
            }
        }
        if (!ac->has_pos && rx_ref_ok && !cpr_local(rx_lat, rx_lon, cl, cn, odd, &la, &lo)) {
            int32_t dla = la - rx_lat; if (dla < 0) dla = -dla;
            if (dla < 30000000) { set_pos(ac, la, lo); return; }   // within ~2.5 deg of the receiver
        }
        long dt = ac->cpr_t[0] - ac->cpr_t[1]; if (dt < 0) dt = -dt;
        if (dt < 10L * FS && !cpr_global(ac->cpr_lat[0], ac->cpr_lon[0], ac->cpr_lat[1], ac->cpr_lon[1], odd, &la, &lo))
            set_pos(ac, la, lo);
    } else if (tc == 19) {
        int st = msg[4] & 7;
        if (st == 1 || st == 2) {
            int ew = ((msg[5] & 3) << 8) | msg[6], ns = ((msg[7] & 0x7F) << 3) | (msg[8] >> 5);
            if (ew && ns) {
                ew--; ns--; if (msg[5] & 4) ew = -ew; if (msg[7] & 0x80) ns = -ns;
                if (st == 2) { ew *= 4; ns *= 4; }
                ac->spd = isqrt64((uint64_t)(ew * ew + ns * ns)); ac->hdg = bearing(ew, ns); ac->has_vel = 1;
            }
            int vr = ((msg[8] & 7) << 6) | (msg[9] >> 2);
            if (vr) ac->vr = ((msg[8] & 8) ? -1 : 1) * (vr - 1) * 64;
        }
    }
}

// scan magnitudes m[0..n) for Mode S preambles; caller keeps TAIL samples of overlap
static void demod(const uint16_t *m, long n) {
    for (long j = 0; j < n - TAIL; j++) {
        const uint16_t *p = m + j;
        if (!(p[0] > p[1] && p[1] < p[2] && p[2] > p[3] && p[3] < p[0] && p[4] < p[0] &&
              p[5] < p[0] && p[6] < p[0] && p[7] > p[8] && p[8] < p[9] && p[9] > p[6])) continue;
        int high = (p[0] + p[2] + p[7] + p[9]) / 6;
        if (p[4] >= high || p[5] >= high) continue;
        if (p[11] >= high || p[12] >= high || p[13] >= high || p[14] >= high) continue;
        st_pre++;
        uint8_t msg[14]; memset(msg, 0, 14);
        for (int b = 0; b < 112; b++)
            if (p[16 + 2 * b] > p[17 + 2 * b]) msg[b >> 3] |= 0x80 >> (b & 7);
        long before = st_ok;
        long save = t_samples; t_samples += j;      // timestamp within block
        handle(msg, (p[0] + p[2] + p[7] + p[9]) / 4);
        t_samples = save;
        if (st_ok != before) j += 16 + (((msg[0] >> 3) >= 16) ? 224 : 112) - 1;
    }
}

// decimal degrees x1e5 from units (for logs / display)
static long units_e5(int32_t u) { return (long)(((int64_t)u * 36000000LL) >> 32); }
static int32_t e6_units(long e6) { return (int32_t)(((int64_t)e6 << 32) / 360000000LL); }
#endif
