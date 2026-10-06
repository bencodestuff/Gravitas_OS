#pragma once

#include <stddef.h>
#include <stdarg.h>

/* The formatter shared by printf/snprintf (printf.c) and the FILE* variants
   (fprintf.c). Each piece of output is handed to the caller's emit function,
   so the destination — a chunk printed atomically, a fixed buffer, or a stream
   — is the caller's choice. Not a public header. */

typedef void (*fmt_emit)(void* ctx, const char* s, size_t len);

int __vformat(fmt_emit emit, void* ctx, const char* format, va_list parameters);
