#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <grv/syscall.h>
#include "fmt_internal.h"

// One formatter behind printf and snprintf/sprintf, and (via fmt_internal.h)
// behind fprintf. Each call supplies an emit function; printf's emit batches
// into a chunk and writes it to stdout (fd 1, whatever it is pointed at), so a
// task switch cannot interleave another task's characters into the middle of a
// line — the kernel emits a console write atomically.
//
// Supported: flags '0' and '-', width, precision, the 'l'/'ll' length
// modifiers, and the d/i/u/x/X/o/c/s/p/f/e/E/g/G conversions. Width and
// precision are capped (200 / 30) — a fixed scratch buffer keeps this small.

#define FMT_BUF 256
#define MAXWIDTH 200
#define MAXPREC 30

// --- number to text -----------------------------------------------------

static int utoa(unsigned long long v, unsigned base, int upper, char* out) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

// --- long-double helpers for %f/%e/%g ----------------------------------

static long double ten_pow(int e) {
    long double r = 1.0L, b = 10.0L;
    unsigned int n = (unsigned int)(e < 0 ? -e : e);
    while (n) {
        if (n & 1) r *= b;
        b *= b;
        n >>= 1;
    }
    return e < 0 ? 1.0L / r : r;
}

static long long round_ll(long double v) {
    long long t = (long long)v; // v >= 0 where this is used
    long double frac = v - (long double)t;
    if (frac > 0.5L) return t + 1;
    if (frac < 0.5L) return t;
    return (t & 1) ? t + 1 : t; // exact half rounds to even, like libm
}

// Fixed notation, digits after the point = prec. v >= 0 and finite.
static int fmt_fixed(char* out, long double v, int prec) {
    int n = 0;
    unsigned long long ip = (unsigned long long)v;
    long double frac = v - (long double)ip;

    unsigned long long fd = 0;
    if (prec > 0) {
        fd = (unsigned long long)round_ll(frac * ten_pow(prec));
        if (fd >= (unsigned long long)ten_pow(prec)) { ip++; fd = 0; }
    } else {
        // Round to an integer with ties to even, matching libm.
        if (frac > 0.5L || (frac == 0.5L && (ip & 1))) ip++;
    }

    n += utoa(ip, 10, 0, out + n);
    if (prec > 0) {
        out[n++] = '.';
        char tmp[32];
        int tn = utoa(fd, 10, 0, tmp);
        for (int i = 0; i < prec - tn; i++) out[n++] = '0';
        for (int i = 0; i < tn; i++) out[n++] = tmp[i];
    }
    out[n] = '\0';
    return n;
}

// Scientific notation, one digit before the point, prec after. v >= 0.
static int fmt_sci(char* out, long double v, int prec, char e_char) {
    int n = 0;
    int want = prec + 1;
    if (want > 30) want = 30;

    char digits[40];
    int exp = 0;
    if (v != 0.0L) {
        while (v >= 10.0L) { v /= 10.0L; exp++; }
        while (v < 1.0L) { v *= 10.0L; exp--; }
        unsigned long long mant = (unsigned long long)round_ll(v * ten_pow(prec));
        if (mant >= (unsigned long long)ten_pow(prec + 1)) {
            mant /= 10; // rounding carried into a new digit
            exp++;
        }
        char tmp[40];
        int tn = utoa(mant, 10, 0, tmp);
        for (int i = 0; i < want - tn; i++) digits[i] = '0';
        for (int i = 0; i < tn; i++) digits[want - tn + i] = tmp[i];
    } else {
        for (int i = 0; i < want; i++) digits[i] = '0';
    }

    out[n++] = digits[0];
    if (prec > 0) {
        out[n++] = '.';
        for (int i = 1; i < want; i++) out[n++] = digits[i];
    }
    out[n++] = e_char;
    out[n++] = exp < 0 ? '-' : '+';
    int ae = exp < 0 ? -exp : exp;
    char eb[8];
    int en = utoa((unsigned)ae, 10, 0, eb);
    if (en < 2) out[n++] = '0';
    for (int i = 0; i < en; i++) out[n++] = eb[i];
    out[n] = '\0';
    return n;
}

// Trim trailing fractional zeros (and a trailing point). Does nothing when
// there is no fractional part (the integer part's zeros are significant).
static void strip_zeros(char* s) {
    char* e = s;
    while (*e && *e != 'e' && *e != 'E') e++;
    char* dot = 0;
    for (char* p = s; p < e; p++) if (*p == '.') dot = p;
    if (dot == 0) return; // no fraction: "100000" keeps its zeros

    char* end = e;
    char* p = end;
    while (p > dot && p[-1] == '0') p--;
    if (p > dot && p[-1] == '.') p--;
    if (p != end) {
        int rest = (int)strlen(end);
        for (int i = 0; i <= rest; i++) p[i] = end[i];
    }
}

// --- the formatter ------------------------------------------------------

int __vformat(fmt_emit emit, void* ctx, const char* format,
              va_list parameters) {
    int written = 0;

    while (*format != '\0') {
        if (*format != '%') {
            const char* run = format;
            while (*format && *format != '%') format++;
            size_t n = (size_t)(format - run);
            emit(ctx, run, n);
            written += (int)n;
            continue;
        }
        if (format[1] == '%') {
            emit(ctx, "%", 1);
            written += 1;
            format += 2;
            continue;
        }

        const char* spec_start = format;
        format++;

        int left = 0, zero = 0;
        for (;;) {
            if (*format == '-') left = 1;
            else if (*format == '0') zero = 1;
            else break;
            format++;
        }
        int width = 0;
        while (*format >= '0' && *format <= '9') {
            if (width < MAXWIDTH) width = width * 10 + (*format - '0');
            format++;
        }
        if (width > MAXWIDTH) width = MAXWIDTH;

        int prec = -1;
        if (*format == '.') {
            format++;
            prec = 0;
            while (*format >= '0' && *format <= '9') {
                if (prec < MAXPREC) prec = prec * 10 + (*format - '0');
                format++;
            }
            if (prec > MAXPREC) prec = MAXPREC;
        }

        bool is_long = false;
        while (*format == 'l') { is_long = true; format++; }

        char conv = *format;
        if (conv == '\0') { format = spec_start; break; }
        format++;

        char out[FMT_BUF];
        int n = 0;
        bool numeric = false;

        switch (conv) {
        case 'd':
        case 'i':
        case 'u': {
            long long sv;
            unsigned long long uv;
            if (conv == 'u') {
                uv = is_long ? va_arg(parameters, unsigned long)
                             : (unsigned int)va_arg(parameters, unsigned int);
            } else {
                sv = is_long ? va_arg(parameters, long)
                             : (long long)(int)va_arg(parameters, int);
                if (sv < 0) { out[n++] = '-'; uv = (unsigned long long)(-(sv + 1)) + 1; }
                else uv = (unsigned long long)sv;
            }
            n += utoa(uv, 10, 0, out + n);
            numeric = true;
            break;
        }
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long uv = is_long
                ? va_arg(parameters, unsigned long)
                : (unsigned int)va_arg(parameters, unsigned int);
            int base = (conv == 'o') ? 8 : 16;
            n += utoa(uv, base, conv == 'X', out + n);
            numeric = true;
            break;
        }
        case 'p': {
            n += utoa((unsigned long long)(uintptr_t)va_arg(parameters, void*),
                      16, 0, out + n);
            // right-align the pointer text under a "0x" prefix
            for (int i = n; i > 0; i--) out[i + 1] = out[i - 1];
            out[0] = '0';
            out[1] = 'x';
            n += 2;
            break;
        }
        case 'c': {
            out[n++] = (char)va_arg(parameters, int);
            break;
        }
        case 's': {
            const char* s = va_arg(parameters, const char*);
            if (s == 0) s = "(null)";
            int len = (int)strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            int pad = width > len ? width - len : 0;
            if (!left) for (int i = 0; i < pad; i++) { emit(ctx, " ", 1); written++; }
            emit(ctx, s, (size_t)len);
            written += len;
            if (left) for (int i = 0; i < pad; i++) { emit(ctx, " ", 1); written++; }
            continue;
        }
        case 'f':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            double dv = va_arg(parameters, double);
            int p = prec < 0 ? 6 : prec;
            if (conv == 'g' || conv == 'G') { if (p == 0) p = 1; }

            if (dv != dv) {
                n = 0;
                const char* s = (conv == 'E' || conv == 'G') ? "NAN" : "nan";
                while (s[n]) { out[n] = s[n]; n++; }
            } else if (__builtin_isinf(dv)) {
                n = 0;
                if (dv < 0) out[n++] = '-';
                const char* s = (conv == 'E' || conv == 'G') ? "INF" : "inf";
                while (*s) out[n++] = *s++;
            } else {
                long double v = (long double)dv;
                bool neg = v < 0.0L;
                if (neg) { v = -v; out[n++] = '-'; }
                int start = n; // where the digits begin (after any sign)

                if (conv == 'f') {
                    if (v < 1.0e18L) {
                        n += fmt_fixed(out + n, v, p);
                    } else {
                        // Beyond 2^63 fixed notation would need big-int
                        // printing; fall back to scientific so the digits
                        // are still right.
                        n += fmt_sci(out + n, v, p, 'e');
                    }
                } else if (conv == 'e' || conv == 'E') {
                    n += fmt_sci(out + n, v, p, conv);
                } else { // g/G: choose by exponent, then trim zeros
                    int exp = 0;
                    long double t = v;
                    if (t > 0.0L) {
                        while (t >= 10.0L) { t /= 10.0L; exp++; }
                        while (t < 1.0L) { t *= 10.0L; exp--; }
                    }
                    char e_char = (conv == 'G') ? 'E' : 'e';
                    if (exp < -4 || exp >= p || v >= 1.0e18L) {
                        n += fmt_sci(out + n, v, p - 1, e_char);
                    } else {
                        int fp = p - 1 - exp;
                        if (fp < 0) fp = 0;
                        n += fmt_fixed(out + n, v, fp);
                    }
                    strip_zeros(out + start);
                    n = (int)strlen(out);   // strip_zeros shortened it
                }
            }
            numeric = true;
            break;
        }
        default: {
            // Unknown conversion: echo it literally, like a plain %x.
            size_t len = (size_t)(format - spec_start);
            emit(ctx, spec_start, len);
            written += (int)len;
            continue;
        }
        }

        if (!numeric) {
            int pad = width > n ? width - n : 0;
            if (!left) for (int i = 0; i < pad; i++) { emit(ctx, " ", 1); written++; }
            emit(ctx, out, (size_t)n);
            written += n;
            if (left) for (int i = 0; i < pad; i++) { emit(ctx, " ", 1); written++; }
            continue;
        }

        // Numeric: zero padding goes after a leading sign.
        int sign = (out[0] == '-' || out[0] == ' ') ? 1 : 0;
        int pad = width > n ? width - n : 0;
        if (!left) {
            if (zero) {
                emit(ctx, out, (size_t)sign);
                written += sign;
                for (int i = 0; i < pad; i++) { emit(ctx, "0", 1); written++; }
                emit(ctx, out + sign, (size_t)(n - sign));
                written += n - sign;
            } else {
                for (int i = 0; i < pad; i++) { emit(ctx, " ", 1); written++; }
                emit(ctx, out, (size_t)n);
                written += n;
            }
        } else {
            emit(ctx, out, (size_t)n);
            written += n;
            for (int i = 0; i < pad; i++) { emit(ctx, " ", 1); written++; }
        }
    }

    return written;
}

// --- printf: chunked, atomic ---------------------------------------------

#define PBUF 256

struct put_ctx { char b[PBUF]; int n; };

static void put_flush(struct put_ctx* o) {
    if (o->n == 0) return;
    // printf is fprintf(stdout): the FILE layer writes the descriptor, so a
    // shell redirect (`> file`, `| next`) applies to printf like anything else.
    fwrite(o->b, 1, (size_t)o->n, stdout);
    o->n = 0;
}

static void put_emit(void* ctx, const char* s, size_t len) {
    struct put_ctx* o = (struct put_ctx*)ctx;
    for (size_t i = 0; i < len; i++) {
        if (o->n == PBUF - 1) put_flush(o);
        o->b[o->n++] = s[i];
    }
}

int printf(const char* restrict format, ...) {
    struct put_ctx o;
    o.n = 0;
    va_list parameters;
    va_start(parameters, format);
    int n = __vformat(put_emit, &o, format, parameters);
    va_end(parameters);
    put_flush(&o);
    return n;
}

// --- snprintf / vsnprintf -------------------------------------------------

struct sn_ctx { char* buf; size_t cap; size_t len; };

static void sn_emit(void* ctx, const char* s, size_t len) {
    struct sn_ctx* o = (struct sn_ctx*)ctx;
    for (size_t i = 0; i < len; i++) {
        if (o->cap && o->len + 1 < o->cap) o->buf[o->len] = s[i];
        o->len++;
    }
}

int vsnprintf(char* buf, size_t size, const char* format, va_list parameters) {
    struct sn_ctx o = { buf, size, 0 };
    int n = __vformat(sn_emit, &o, format, parameters);
    if (size) {
        size_t end = (o.len < size) ? o.len : size - 1;
        buf[end] = '\0';
    }
    return n;
}

int snprintf(char* buf, size_t size, const char* format, ...) {
    va_list parameters;
    va_start(parameters, format);
    int n = vsnprintf(buf, size, format, parameters);
    va_end(parameters);
    return n;
}

// --- sprintf / vsprintf (unbounded; the caller owns sizing) --------------

struct sp_ctx { char* buf; size_t len; };

static void sp_emit(void* ctx, const char* s, size_t len) {
    struct sp_ctx* o = (struct sp_ctx*)ctx;
    for (size_t i = 0; i < len; i++) o->buf[o->len++] = s[i];
}

int vsprintf(char* buf, const char* format, va_list parameters) {
    struct sp_ctx o = { buf, 0 };
    int n = __vformat(sp_emit, &o, format, parameters);
    buf[o.len] = '\0';
    return n;
}

int sprintf(char* buf, const char* format, ...) {
    va_list parameters;
    va_start(parameters, format);
    int n = vsprintf(buf, format, parameters);
    va_end(parameters);
    return n;
}