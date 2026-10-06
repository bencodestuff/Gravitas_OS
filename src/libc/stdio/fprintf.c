#include <stdio.h>
#include <stdarg.h>
#include "fmt_internal.h"

// fprintf/vfprintf run the same formatter as printf/snprintf; the emit function
// just forwards to the stream, so buffering and the console path stay the FILE
// layer's business.

static void file_emit(void* ctx, const char* s, size_t len) {
    if (len) fwrite(s, 1, len, (FILE*)ctx);
}

int vfprintf(FILE* f, const char* format, va_list parameters) {
    if (f == 0) return -1;
    return __vformat(file_emit, f, format, parameters);
}

int fprintf(FILE* f, const char* format, ...) {
    va_list parameters;
    va_start(parameters, format);
    int n = vfprintf(f, format, parameters);
    va_end(parameters);
    return n;
}
