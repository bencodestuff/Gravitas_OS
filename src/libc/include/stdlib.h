#ifndef _STDLIB_H
#define _STDLIB_H 1

#include <sys/cdefs.h>
#include <stddef.h>

__BEGIN_DECLS

__attribute__((__noreturn__))
void abort(void);

/* Memory allocation — backed by the grv_brk syscall (kernel grows the
   program's data segment on demand). */
void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);
void  free(void* ptr);

/* Number parsing. */
long strtol(const char* __restrict, char** __restrict, int base);
unsigned long strtoul(const char* __restrict, char** __restrict, int base);
int  atoi(const char*);
double strtod(const char* __restrict, char** __restrict);
double atof(const char*);

int abs(int);

/* Environment lookup. There is no environment block yet, so this always
   returns NULL (a miss). Lua's os.getenv therefore returns nil. */
char* getenv(const char* name);

/* Run a command through a shell. There is no shell to hand it to, so this
   always fails (-1); a NULL argument would be the "is one available" query. */
int system(const char* command);

/* Sort 'count' elements of 'size' bytes by 'compare'. */
void qsort(void* base, size_t count, size_t size,
           int (*compare)(const void*, const void*));

__attribute__((__noreturn__))
void exit(int code);

__END_DECLS

#ifdef __cplusplus
/* C++ runtime support: operator new/delete.
   Implemented in libc/cxx/cxxrt.cpp. These are called by compiled C++
   code and must NOT be inside extern "C" (they're C++ mangled symbols). */
void* operator new(size_t size);
void* operator new[](size_t size);
void operator delete(void* p) noexcept;
void operator delete[](void* p) noexcept;
void operator delete(void* p, unsigned long) noexcept;
void operator delete[](void* p, unsigned long) noexcept;
#endif

#endif
