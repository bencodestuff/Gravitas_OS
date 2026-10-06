#ifndef _STDIO_H
#define _STDIO_H 1

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)

#ifdef __cplusplus
extern "C" {
#endif

int printf(const char* __restrict, ...);

/* Formatted output into a buffer. snprintf never writes more than 'size'
   bytes (including the terminator) and returns the length it would have
   produced. sprintf assumes the buffer is large enough. */
int snprintf(char* __restrict, size_t, const char* __restrict, ...);
int vsnprintf(char* __restrict, size_t, const char* __restrict, va_list);
int sprintf(char* __restrict, const char* __restrict, ...);
int vsprintf(char* __restrict, const char* __restrict, va_list);

int putchar(int);
int puts(const char*);

/* Filesystem operations, in their stdio spelling. remove() unlinks a file or
   empty directory; rename() moves within one filesystem. */
int remove(const char* path);
int rename(const char* from, const char* to);

#ifdef __cplusplus
}
#endif

#endif
