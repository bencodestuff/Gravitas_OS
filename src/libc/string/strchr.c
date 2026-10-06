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

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    for (; s[n]; n++) {
        const char* a = accept;
        while (*a && *a != s[n]) a++;
        if (*a == '\0') break;
    }
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    for (; s[n]; n++) {
        const char* r = reject;
        while (*r && *r != s[n]) r++;
        if (*r) break;
    }
    return n;
}

char* strpbrk(const char* s, const char* accept) {
    for (; *s; s++) {
        const char* a = accept;
        while (*a && *a != *s) a++;
        if (*a) return (char*)s;
    }
    return 0;
}

char* strsep(char** stringp, const char* delim) {
    if (stringp == 0 || *stringp == 0) return 0;
    char* token = *stringp;
    for (char* p = token; ; p++) {
        if (*p == '\0') { *stringp = 0; return token; }
        const char* d = delim;
        while (*d && *d != *p) d++;
        if (*d) { *p = '\0'; *stringp = p + 1; return token; }
    }
}
