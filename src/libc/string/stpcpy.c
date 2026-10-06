#include <string.h>

char* stpcpy(char* __restrict dst, const char* __restrict src) {
    while ((*dst = *src) != '\0') { dst++; src++; }
    return dst;   // points at the terminator in dst
}
