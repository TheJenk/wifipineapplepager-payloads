/*
 * fieldtag - the on-screen tagging view for the Field Marker payload.
 *
 * Shows four labels laid out like the D-pad (up / down / left / right). Press a
 * direction and it records that observation: it appends "<epoch>,<slot>" to the
 * queue file and bumps that label's on-screen counter. A bash helper reads the
 * queue, grabs the GPS fix, and writes the waypoint - so this binary stays tiny
 * and never touches GPS or file formats itself. Red exits.
 *
 * Freestanding (no libc, raw MIPS o32 syscalls, integer-only - the MT7628 has no
 * FPU). Draws upright 480x222 into the 222x480 framebuffer. -DHOST_PREVIEW builds
 * a desktop version that renders frames to .raw for tuning the UI off-device.
 *
 *   Slots file  /tmp/fieldtag.slots   4 lines: UP, DOWN, LEFT, RIGHT labels
 *   Queue file  /tmp/fieldtag.q       appended "<epoch>,<slot 0..3>" per press
 *   Status file /tmp/fieldtag.status  optional 1 line from the recorder (e.g. "FIX")
 */
#include <stdint.h>
#include "font5x7.h"

#define W 480
#define H 222
#define FB_W 222
#define NPIX (W * H)
#define FRAME_BYTES (NPIX * 2)
#define LABELMAX 24

static uint16_t fb[NPIX];

/* ------------------------------------------------------------------ drawing */
static inline uint16_t rgb(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255; if (g < 0) g = 0; if (g > 255) g = 255; if (b < 0) b = 0; if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline void px(int x, int y, uint16_t c) { if ((unsigned)x < W && (unsigned)y < H) fb[x * FB_W + (H - 1 - y)] = c; }
static void fill(int x, int y, int w, int h, uint16_t c) { for (int i = 0; i < w; i++) for (int j = 0; j < h; j++) px(x + i, y + j, c); }
static void frame_rect(int x, int y, int w, int h, uint16_t c) { fill(x, y, w, 1, c); fill(x, y + h - 1, w, 1, c); fill(x, y, 1, h, c); fill(x + w - 1, y, 1, h, c); }
static void frame2(int x, int y, int w, int h, uint16_t c) { frame_rect(x, y, w, h, c); frame_rect(x + 1, y + 1, w - 2, h - 2, c); }
static int draw_textn(int x, int y, const char *s, int maxc, uint16_t c, int sc) {
    int n = 0;
    for (; *s && n < maxc; s++, n++) { char ch = *s; if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch >= FONT_FIRST && ch < FONT_FIRST + FONT_COUNT) { const unsigned char *g = FONT5X7[ch - FONT_FIRST];
            for (int row = 0; row < 7; row++) { unsigned bits = g[row];
                for (int col = 0; col < 5; col++) if (bits & (1u << (4 - col))) fill(x + col * sc, y + row * sc, sc, sc, c); } }
        x += 6 * sc; }
    return n;
}
static void draw_text(int x, int y, const char *s, uint16_t c, int sc) { draw_textn(x, y, s, 100000, c, sc); }
static int slen_(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int text_w(const char *s, int sc) { int n = slen_(s); return n ? n * 6 * sc - sc : 0; }
static int text_wn(const char *s, int maxc, int sc) { int n = slen_(s); if (n > maxc) n = maxc; return n ? n * 6 * sc - sc : 0; }
static void text_at_c(int cx, int y, const char *s, uint16_t c, int sc) { draw_text(cx - text_w(s, sc) / 2, y, s, c, sc); }

/* small filled direction triangles */
static void tri_up(int cx, int y, int s, uint16_t c)    { for (int i = 0; i < s; i++) fill(cx - i, y + i, 2 * i + 1, 1, c); }
static void tri_down(int cx, int y, int s, uint16_t c)  { for (int i = 0; i < s; i++) fill(cx - (s - 1 - i), y + i, 2 * (s - 1 - i) + 1, 1, c); }
static void tri_left(int x, int cy, int s, uint16_t c)  { for (int i = 0; i < s; i++) fill(x + i, cy - i, 1, 2 * i + 1, c); }
static void tri_right(int x, int cy, int s, uint16_t c) { for (int i = 0; i < s; i++) fill(x + i, cy - (s - 1 - i), 1, 2 * (s - 1 - i) + 1, c); }

/* tiny string builder */
static char sb[64]; static int sbn;
static void sb_reset(void) { sbn = 0; sb[0] = 0; }
static void sb_c(char c) { if (sbn < 62) { sb[sbn++] = c; sb[sbn] = 0; } }
static void sb_s(const char *s) { while (*s && sbn < 62) sb[sbn++] = *s++; sb[sbn] = 0; }
static void sb_i(long v) { char t[16]; int j = 0, i = 0; if (v < 0) { sb_c('-'); v = -v; } if (!v) t[j++] = '0'; while (v) { t[j++] = (char)('0' + v % 10); v /= 10; } char o[16]; while (j) o[i++] = t[--j]; o[i] = 0; sb_s(o); }

/* ------------------------------------------------------------------ colors */
static uint16_t C_BG, C_BAR, C_TITLE, C_DIM, C_BOX, C_BOXSEL, C_TEXT, C_ARROW, C_FLASH, C_FIX, C_NOFIX;
static void init_colors(void) {
    C_BG = rgb(14, 16, 22); C_BAR = rgb(24, 28, 38); C_TITLE = rgb(120, 200, 255); C_DIM = rgb(110, 120, 135);
    C_BOX = rgb(34, 40, 54); C_BOXSEL = rgb(40, 120, 90); C_TEXT = rgb(235, 238, 245); C_ARROW = rgb(90, 160, 220);
    C_FLASH = rgb(90, 220, 140); C_FIX = rgb(90, 220, 140); C_NOFIX = rgb(235, 170, 70);
}

/* ------------------------------------------------------------------ state */
enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_GREEN, A_RED, NACT };
static unsigned char press[NACT];
static char label[4][LABELMAX + 1];
static long count[4];
static int flash[4];            /* frames left to highlight a just-pressed box */
static long total;
static int quit_req;
static char statusline[32];     /* from the recorder: "", "FIX", "NO FIX" */

static int slot_of_action(int a) {
    if (a == A_UP) return 0; if (a == A_DOWN) return 1; if (a == A_LEFT) return 2; if (a == A_RIGHT) return 3; return -1;
}

/* ------------------------------------------------------------------ render */
static void box(int x, int y, int w, int h, int slot, int arrow) {
    int sel = flash[slot] > 0;
    fill(x, y, w, h, sel ? C_BOXSEL : C_BOX);
    frame2(x, y, w, h, sel ? C_FLASH : C_DIM);
    /* arrow */
    int ax = x + 14, ay = y + h / 2;
    if (arrow == 0) tri_up(ax, ay - 6, 7, C_ARROW);
    else if (arrow == 1) tri_down(ax, ay - 6, 7, C_ARROW);
    else if (arrow == 2) tri_left(x + 8, ay, 7, C_ARROW);
    else tri_right(x + 8, ay, 7, C_ARROW);
    /* count on the right (always scale 2) */
    sb_reset(); sb_c('x'); sb_i(count[slot]);
    int cw = text_w(sb, 2);
    draw_text(x + w - 8 - cw, y + (h - 14) / 2, sb, sel ? C_FLASH : C_DIM, 2);
    /* label: scale 2 if it fits, else scale 1, truncated to the space left */
    const char *lb = label[slot][0] ? label[slot] : "(unset)";
    int lx = x + 28, avail = (x + w - 10 - cw) - lx;
    uint16_t lc = sel ? C_FLASH : C_TEXT;
    if (text_w(lb, 2) <= avail - 8) draw_text(lx, y + (h - 14) / 2, lb, lc, 2);
    else { int maxc = (avail - 4) / 6; if (maxc < 1) maxc = 1; draw_textn(lx, y + (h - 8) / 2, lb, maxc, lc, 1); }
}

static void render(void) {
    for (int i = 0; i < NPIX; i++) fb[i] = C_BG;
    /* title bar */
    fill(0, 0, W, 22, C_BAR);
    draw_text(8, 4, "FIELD MARKER", C_TITLE, 2);
    sb_reset(); sb_s("TOTAL "); sb_i(total);
    draw_text(W - 8 - text_w(sb, 1), 8, sb, C_DIM, 1);
    /* gps status chip */
    if (statusline[0]) {
        int fixv = (statusline[0] == 'F');
        const char *t = fixv ? "GPS FIX" : "NO FIX";
        int tw = text_w(t, 1);
        fill(W - 90 - tw, 6, tw + 10, 11, C_BAR);
        draw_text(W - 85 - tw, 8, t, fixv ? C_FIX : C_NOFIX, 1);
    }

    /* four boxes in a cross */
    int bw = 240, bh = 40;
    box((W - bw) / 2, 28, bw, bh, 0, 0);           /* UP    top-center */
    box((W - bw) / 2, H - 28 - bh, bw, bh, 1, 1);  /* DOWN  bottom-center */
    box(10, (H - bh) / 2, 210, bh, 2, 2);          /* LEFT  */
    box(W - 10 - 210, (H - bh) / 2, 210, bh, 3, 3);/* RIGHT */

    /* center dot between the left/right boxes */
    fill(W / 2 - 2, H / 2 - 2, 4, 4, C_DIM);
    draw_text(8, H - 11, "RED  EXIT", C_DIM, 1);
    text_at_c(W / 2, H - 11, "TAP A DIRECTION TO DROP A PIN", C_DIM, 1);
}

/* ------------------------------------------------------------------ device */
#ifndef HOST_PREVIEW
#define SYS_read 4003
#define SYS_write 4004
#define SYS_open 4005
#define SYS_close 4006
#define SYS_lseek 4019
#define SYS_nanosleep 4166
#define SYS_clock_gettime 4263
#define O_RDONLY 0
#define O_WRONLY 1
#define O_NONBLOCK 0x80
#define O_RDWR 2
#define O_CREAT 0x100
#define O_APPEND 0x8
static long sys3(long n, long a, long b, long c) {
    register long r2 asm("$2") = n, r4 asm("$4") = a, r5 asm("$5") = b, r6 asm("$6") = c, r7 asm("$7") = 0;
    asm volatile("syscall" : "+r"(r2), "+r"(r7) : "r"(r4), "r"(r5), "r"(r6)
                 : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "memory");
    return r7 ? -r2 : r2;
}
asm(".text\n.globl _start\n.ent _start\n_start:\n jal main\n nop\n move $4,$2\n li $2,4246\n syscall\n nop\n.end _start\n");
void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
struct ts { long sec, nsec; };
static void sleep_ms(long ms) { struct ts t; t.sec = ms / 1000; t.nsec = (ms % 1000) * 1000000; sys3(SYS_nanosleep, (long)&t, 0, 0); }
static long now_epoch(void) { struct ts t; t.sec = 0; sys3(SYS_clock_gettime, 0, (long)&t, 0); return t.sec; }

static int slen2(const char *s){int n=0;while(s[n])n++;return n;}

/* read the 4 slot labels from /tmp/fieldtag.slots (one per line) */
static void load_slots(void) {
    for (int i = 0; i < 4; i++) label[i][0] = 0;
    char buf[512]; int fd = (int)sys3(SYS_open, (long)"/tmp/fieldtag.slots", O_RDONLY, 0);
    if (fd < 0) return;
    long n = sys3(SYS_read, fd, (long)buf, sizeof buf - 1); sys3(SYS_close, fd, 0, 0);
    if (n <= 0) return; buf[n] = 0;
    int row = 0, k = 0;
    for (long i = 0; i <= n && row < 4; i++) {
        char c = buf[i];
        if (c == '\n' || c == 0) { label[row][k] = 0; row++; k = 0; if (c == 0) break; }
        else if (k < LABELMAX) label[row][k++] = c;
    }
}
/* read optional recorder status */
static void load_status(void) {
    statusline[0] = 0;
    int fd = (int)sys3(SYS_open, (long)"/tmp/fieldtag.status", O_RDONLY, 0);
    if (fd < 0) return;
    long n = sys3(SYS_read, fd, (long)statusline, sizeof statusline - 1); sys3(SYS_close, fd, 0, 0);
    if (n < 0) n = 0; statusline[n] = 0;
    for (int i = 0; statusline[i]; i++) if (statusline[i] == '\n') { statusline[i] = 0; break; }
}
/* append "<epoch>,<slot>\n" to the queue */
static void queue_tag(int slot) {
    char line[32]; int k = 0;
    long e = now_epoch(); char t[16]; int j = 0; if (!e) t[j++]='0'; while (e) { t[j++] = (char)('0'+e%10); e/=10; }
    while (j) line[k++] = t[--j];
    line[k++] = ','; line[k++] = (char)('0' + slot); line[k++] = '\n';
    int fd = (int)sys3(SYS_open, (long)"/tmp/fieldtag.q", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { sys3(SYS_write, fd, (long)line, k); sys3(SYS_close, fd, 0, 0); }
}
static void poll_input(int fd) {
    for (int i = 0; i < NACT; i++) press[i] = 0;
    if (fd < 0) return;
    unsigned char e[16];
    while (sys3(SYS_read, fd, (long)e, 16) == 16) {
        int type = e[8] | (e[9] << 8), code = e[10] | (e[11] << 8), val = e[12] | (e[13] << 8);
        if (type != 1 || val != 1) continue;
        if (code == 103) press[A_UP] = 1; else if (code == 108) press[A_DOWN] = 1;
        else if (code == 105) press[A_LEFT] = 1; else if (code == 106) press[A_RIGHT] = 1;
        else if (code == 305) press[A_GREEN] = 1; else if (code == 304) press[A_RED] = 1;
    }
}
static void tick_logic(void) {
    if (press[A_RED]) { quit_req = 1; return; }
    for (int a = A_UP; a <= A_RIGHT; a++) if (press[a]) {
        int s = slot_of_action(a);
        if (s >= 0) { count[s]++; total++; flash[s] = 6; queue_tag(s); }
    }
    for (int i = 0; i < 4; i++) if (flash[i] > 0) flash[i]--;
}
int main(void) {
    init_colors();
    int fb_fd = (int)sys3(SYS_open, (long)"/dev/fb0", O_RDWR, 0); if (fb_fd < 0) return 1;
    int ev = (int)sys3(SYS_open, (long)"/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    load_slots();
    /* drain any input buffered before we started (the keypress that launched us,
       or a back/select left over from the menu) so it isn't read as a tap/exit */
    { unsigned char junk[16]; while (ev >= 0 && sys3(SYS_read, ev, (long)junk, 16) == 16) {} }
    int frame = 0, grace = 8;   /* ignore input for the first ~0.4s */
    while (!quit_req) {
        poll_input(ev);
        if (grace > 0) { grace--; for (int i = 0; i < NACT; i++) press[i] = 0; }
        tick_logic();
        if ((frame++ % 6) == 0) load_status();   /* refresh gps chip ~3x/sec */
        render();
        sys3(SYS_lseek, fb_fd, 0, 0); sys3(SYS_write, fb_fd, (long)fb, FRAME_BYTES);
        sleep_ms(50);
    }
    if (ev >= 0) sys3(SYS_close, ev, 0, 0);
    sys3(SYS_close, fb_fd, 0, 0);
    (void)slen2;
    return 0;
}

/* ================================================================== desktop preview */
#else
#include <stdio.h>
#include <string.h>
static const char *outdir;
static void snap(const char *name) { char p[512]; snprintf(p, sizeof p, "%s/%s.raw", outdir, name); FILE *f = fopen(p, "wb"); if (f) { fwrite(fb, 2, NPIX, f); fclose(f); } }
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s OUTDIR\n", argv[0]); return 2; }
    outdir = argv[1];
    init_colors();
    strcpy(label[0], "SECURITY CAM"); strcpy(label[1], "OPEN DOOR");
    strcpy(label[2], "OPEN WINDOW"); strcpy(label[3], "OPEN GATE");
    count[0] = 3; count[1] = 1; count[2] = 7; count[3] = 0; total = 11;
    strcpy(statusline, "FIX");
    render(); snap("tag");
    flash[0] = 6; strcpy(statusline, "NO FIX"); render(); snap("tag_flash");
    return 0;
}
#endif
