#include <stdlib.h>

long strtol(const char* s, char** end, int base) {
    const char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') { base = 8; p++; }
        else base = 10;
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    unsigned long v = 0;
    for (;; p++) {
        int d;
        char c = *p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }

    if (end) *end = (char*)p;
    return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char* s, char** end, int base) {
    long v = strtol(s, end, base);
    return (unsigned long)v;
}

int atoi(const char* s) { return (int)strtol(s, 0, 10); }
