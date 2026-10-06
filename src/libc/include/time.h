#ifndef _TIME_H
#define _TIME_H 1

#include <sys/cdefs.h>
#include <stddef.h>

__BEGIN_DECLS

typedef long time_t;    /* seconds since the Unix epoch */
typedef long clock_t;   /* processor time in CLOCKS_PER_SEC units */

/* The timer runs at 50 Hz, so this is the resolution of clock(). */
#define CLOCKS_PER_SEC 1000000L

struct tm {
    int tm_sec;    /* 0..60 */
    int tm_min;    /* 0..59 */
    int tm_hour;   /* 0..23 */
    int tm_mday;   /* 1..31 */
    int tm_mon;    /* 0..11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0 = Sunday */
    int tm_yday;   /* 0..365 */
    int tm_isdst;  /* always 0: there is one timezone */
};

time_t time(time_t* tloc);
clock_t clock(void);
double difftime(time_t t1, time_t t2);

/* UTC, into a shared static struct (not thread-safe, as in C). localtime is
   the same thing: there is one timezone and it is UTC. */
struct tm* gmtime(const time_t* t);
struct tm* localtime(const time_t* t);

/* Fills in tm_wday/tm_yday as the standard requires and returns the epoch
   seconds. tm_isdst is ignored. */
time_t mktime(struct tm* tm);

/* Returns the number of bytes written (not counting the terminator), or 0 if
   the result would not fit. Unsupported specifiers are copied literally. */
size_t strftime(char* __restrict, size_t, const char* __restrict,
                const struct tm* __restrict);

__END_DECLS

#endif
