#include <string.h>

char* strchr(const char* s, int c) {
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char*)s;
        if (*s == '\0') return 0;
    }
}

char* strrchr(const char* s, int c) {
    char ch = (char)c;
    const char* last = 0;
    for (;; s++) {
        if (*s == ch) last = s;
        if (*s == '\0') break;
    }
    return (char*)last;
}

char* strstr(const char* hay, const char* needle) {
    if (*needle == '\0') return (char*)hay;
    for (; *hay; hay++) {
        const char* a = hay;
        const char* b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b == '\0') return (char*)hay;
    }
    return 0;
}

size_t strnlen(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}
