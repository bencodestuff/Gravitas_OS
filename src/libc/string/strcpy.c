#include <string.h>

char* strcpy(char* d, const char* s) {
    char* r = d;
    while (*s) *d++ = *s++;
    *d = '\0';
    return r;
}

char* strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}

char* strcat(char* d, const char* s) {
    char* r = d;
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = '\0';
    return r;
}

char* strncat(char* d, const char* s, size_t n) {
    char* r = d;
    while (*d) d++;
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
    return r;
}
