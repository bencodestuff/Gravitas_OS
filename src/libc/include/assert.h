#ifndef _ASSERT_H
#define _ASSERT_H 1

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Reports the failed expression and aborts. Called by the assert() macro. */
__attribute__((__noreturn__))
void __assert_fail(const char* expr, const char* file, int line);

__END_DECLS

/* Define NDEBUG to compile assertions out. */
#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__))
#endif

#endif
