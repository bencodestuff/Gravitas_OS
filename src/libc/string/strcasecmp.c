#include <string.h>

// ASCII case folding (the C locale is the only one we have).
static int fold(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

int strcasecmp(const char* a, const char* b) {
    while (*a && fold((unsigned char)*a) == fold((unsigned char)*b)) { a++; b++; }
    return fold((unsigned char)*a) - fold((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    while (n && *a && fold((unsigned char)*a) == fold((unsigned char)*b)) {
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return fold((unsigned char)*a) - fold((unsigned char)*b);
}

// There is one locale, so collation is byte order.
int strcoll(const char* a, const char* b) { return strcmp(a, b); }
