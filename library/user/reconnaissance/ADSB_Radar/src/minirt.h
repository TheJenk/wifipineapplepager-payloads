// minirt.h - tiny freestanding runtime for the Pager (mipsel o32, no libc, no FP).
// Provides just what adsb.c needs: read/write/exit, mem*, and a small printf.
#ifndef MINIRT_H
#define MINIRT_H
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

static long sys3(long n, long a, long b, long c) {
    register long r2 asm("$2") = n, r4 asm("$4") = a, r5 asm("$5") = b, r6 asm("$6") = c, r7 asm("$7") = 0;
    asm volatile("syscall" : "+r"(r2), "+r"(r7) : "r"(r4), "r"(r5), "r"(r6)
                 : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "memory");
    return r7 ? -r2 : r2;
}
#define SYS_exit 4001
#define SYS_read 4003
#define SYS_write 4004
asm(".text\n.globl _start\n.ent _start\n_start:\n jal main\n nop\n move $4,$2\n li $2,4246\n syscall\n nop\n.end _start\n");

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *p = d; const uint8_t *q = s;
    if (p < q) while (n--) *p++ = *q++; else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}

// read exactly n bytes unless EOF/error; returns bytes read
static long rt_readfull(int fd, void *buf, long n) {
    long got = 0;
    while (got < n) { long r = sys3(SYS_read, fd, (long)((char *)buf + got), n - got); if (r <= 0) break; got += r; }
    return got;
}

// ---- tiny printf: %d %ld %u %x %X %s %c %%, flags '-' '0', width ----
static char obuf[8192]; static int olen;
static void rt_flush(void) { if (olen) sys3(SYS_write, 1, (long)obuf, olen); olen = 0; }
static void rt_putc(char c) { if (olen == sizeof obuf) rt_flush(); obuf[olen++] = c; }
static int rt_vfmt(char *dst, int cap, const char *f, va_list ap) {
    int n = 0;
#define EMIT(ch) do { if (dst) { if (n < cap - 1) dst[n] = (ch); } else rt_putc(ch); n++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { EMIT(*f); continue; }
        f++;
        int left = 0, zero = 0, width = 0, lng = 0;
        for (;; f++) { if (*f == '-') left = 1; else if (*f == '0') zero = 1; else break; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 'l') { lng = 1; f++; }
        char tmp[24]; const char *s = tmp; int len = 0;
        if (*f == 'd' || *f == 'u' || *f == 'x' || *f == 'X') {
            long v = lng ? va_arg(ap, long) : (*f == 'd' ? va_arg(ap, int) : (long)va_arg(ap, unsigned));
            unsigned long u; int neg = 0, base = (*f == 'x' || *f == 'X') ? 16 : 10;
            if (*f == 'd' && v < 0) { neg = 1; u = (unsigned long)(-v); } else u = (unsigned long)v;
            const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char r[24]; int k = 0; do { r[k++] = dig[u % base]; u /= base; } while (u);
            if (neg) tmp[len++] = '-';
            if (zero && !left) while (len + k < width) tmp[len++] = '0';
            while (k) tmp[len++] = r[--k];
        } else if (*f == 's') { s = va_arg(ap, const char *); while (s[len]) len++; }
        else if (*f == 'c') { tmp[0] = (char)va_arg(ap, int); len = 1; }
        else { tmp[0] = *f; len = 1; }
        if (!left) for (int i = len; i < width; i++) EMIT(' ');
        for (int i = 0; i < len; i++) EMIT(s[i]);
        if (left) for (int i = len; i < width; i++) EMIT(' ');
    }
    if (dst && cap) dst[n < cap ? n : cap - 1] = 0;
#undef EMIT
    return n;
}
static int printf(const char *f, ...) { va_list ap; va_start(ap, f); int n = rt_vfmt(0, 0, f, ap); va_end(ap); return n; }
static int sprintf(char *d, const char *f, ...) { va_list ap; va_start(ap, f); int n = rt_vfmt(d, 4096, f, ap); va_end(ap); return n; }
#define fflush(x) rt_flush()
#endif
