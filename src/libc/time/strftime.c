#include <time.h>

// strftime. The common conversions are implemented; an unsupported specifier
// is copied literally (its '%' and letter), which is what a minimal libc does.

// Forward declaration: put_fmt (below) recurses through strftime.
size_t strftime(char*, size_t, const char*, const struct tm*);

static const char* const WDAY_ABBR[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char* const WDAY_FULL[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char* const MON_ABBR[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char* const MON_FULL[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static void put_ch(char* s, size_t max, size_t* n, int* fits, char c) {
    if (*n + 1 >= max) *fits = 0;
    else s[*n] = c;
    (*n)++;
}

static void put_str(char* s, size_t max, size_t* n, int* fits, const char* str) {
    while (*str) put_ch(s, max, n, fits, *str++);
}

// Zero-padded unsigned decimal, minimum width.
static void put_num(char* s, size_t max, size_t* n, int* fits, long v, int width) {
    char b[24];
    int i = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) b[i++] = '-';
    for (int p = width - i; p > 0; p--) put_ch(s, max, n, fits, '0');
    while (i > 0) put_ch(s, max, n, fits, b[--i]);
}

// Recursive helper for the composite conversions (%c, %F, ...).
static void put_fmt(char* s, size_t max, size_t* n, int* fits,
                    const char* fmt, const struct tm* tm) {
    char tmp[64];
    if (strftime(tmp, sizeof(tmp), fmt, tm) == 0) { *fits = 0; return; }
    put_str(s, max, n, fits, tmp);
}

size_t strftime(char* s, size_t max, const char* format, const struct tm* tm) {
    size_t n = 0;
    int fits = 1;

    for (const char* p = format; *p; p++) {
        if (*p != '%') { put_ch(s, max, &n, &fits, *p); continue; }

        p++;
        if (*p == '\0') { put_ch(s, max, &n, &fits, '%'); break; }

        switch (*p) {
        case 'a': put_str(s, max, &n, &fits, WDAY_ABBR[tm->tm_wday]); break;
        case 'A': put_str(s, max, &n, &fits, WDAY_FULL[tm->tm_wday]); break;
        case 'b':
        case 'h': put_str(s, max, &n, &fits, MON_ABBR[tm->tm_mon]); break;
        case 'B': put_str(s, max, &n, &fits, MON_FULL[tm->tm_mon]); break;
        case 'c': put_fmt(s, max, &n, &fits, "%a %b %e %H:%M:%S %Y", tm); break;
        case 'C': put_num(s, max, &n, &fits, (tm->tm_year + 1900) / 100, 2); break;
        case 'd': put_num(s, max, &n, &fits, tm->tm_mday, 2); break;
        case 'D': put_fmt(s, max, &n, &fits, "%m/%d/%y", tm); break;
        case 'e':
            if (tm->tm_mday < 10) put_ch(s, max, &n, &fits, ' ');
            put_num(s, max, &n, &fits, tm->tm_mday, 1);
            break;
        case 'F': put_fmt(s, max, &n, &fits, "%Y-%m-%d", tm); break;
        case 'H': put_num(s, max, &n, &fits, tm->tm_hour, 2); break;
        case 'I': {
            int h = tm->tm_hour % 12;
            if (h == 0) h = 12;
            put_num(s, max, &n, &fits, h, 2);
            break;
        }
        case 'j': put_num(s, max, &n, &fits, tm->tm_yday + 1, 3); break;
        case 'm': put_num(s, max, &n, &fits, tm->tm_mon + 1, 2); break;
        case 'M': put_num(s, max, &n, &fits, tm->tm_min, 2); break;
        case 'n': put_ch(s, max, &n, &fits, '\n'); break;
        case 'p': put_str(s, max, &n, &fits, tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'r': put_fmt(s, max, &n, &fits, "%I:%M:%S %p", tm); break;
        case 'R': put_fmt(s, max, &n, &fits, "%H:%M", tm); break;
        case 'S': put_num(s, max, &n, &fits, tm->tm_sec, 2); break;
        case 't': put_ch(s, max, &n, &fits, '\t'); break;
        case 'T': put_fmt(s, max, &n, &fits, "%H:%M:%S", tm); break;
        case 'u': put_num(s, max, &n, &fits, tm->tm_wday == 0 ? 7 : tm->tm_wday, 1); break;
        case 'U': put_num(s, max, &n, &fits, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2); break;
        case 'w': put_num(s, max, &n, &fits, tm->tm_wday, 1); break;
        case 'W': {
            int wd = (tm->tm_wday == 0) ? 6 : tm->tm_wday - 1;
            put_num(s, max, &n, &fits, (tm->tm_yday + 7 - wd) / 7, 2);
            break;
        }
        case 'x': put_fmt(s, max, &n, &fits, "%m/%d/%y", tm); break;
        case 'X': put_fmt(s, max, &n, &fits, "%H:%M:%S", tm); break;
        case 'y': put_num(s, max, &n, &fits, (tm->tm_year + 1900) % 100, 2); break;
        case 'Y': put_num(s, max, &n, &fits, tm->tm_year + 1900, 1); break;
        case 'z': put_str(s, max, &n, &fits, "+0000"); break;
        case 'Z': put_str(s, max, &n, &fits, "UTC"); break;
        case '%': put_ch(s, max, &n, &fits, '%'); break;
        default:
            // Unsupported: copy it literally.
            put_ch(s, max, &n, &fits, '%');
            put_ch(s, max, &n, &fits, *p);
            break;
        }
    }

    if (max) s[n < max ? n : max - 1] = '\0';
    return fits ? n : 0;
}
