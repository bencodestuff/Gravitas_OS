#include <string.h>
#include <stdlib.h>

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (!p) return 0;
    for (size_t i = 0; i < n; i++) p[i] = s[i];
    return p;
}
