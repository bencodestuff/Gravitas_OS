#include <time.h>
#include <grv/syscall.h>

time_t time(time_t* tloc) {
    time_t s = (time_t)grv_time();
    if (tloc) *tloc = s;
    return s;
}

clock_t clock(void) {
    // The PIT ticks at 50 Hz; scale into CLOCKS_PER_SEC units so callers get a
    // conventional base (20 ms resolution).
    unsigned long ticks = grv_ticks();
    return (clock_t)(ticks * (CLOCKS_PER_SEC / 50));
}

double difftime(time_t t1, time_t t2) {
    return (double)(t1 - t2);
}
