#ifndef _SYS_TIME_H
#define _SYS_TIME_H 1

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Whole seconds and a remainder below one second, as POSIX defines it.
   The kernel does all of its waiting in whole milliseconds, so callers
   asking for sub-millisecond precision are rounded up. */
struct timeval {
    long tv_sec;
    long tv_usec;
};

__END_DECLS

#endif
