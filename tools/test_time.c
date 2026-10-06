/* Host validation harness for the calendar code (src/libc/time/gmtime.c and
   strftime.c). Compiles our bodies under my_* names and compares them against
   the host C library over a set of known instants:

       cc -O2 -fno-builtin tools/test_time.c -o /tmp/test_time

   Run by tests/host/run.sh (`ninja check`). time.c/clock.c are not included:
   they call the kernel. */

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <locale.h>

#define gmtime    my_gmtime
#define localtime my_localtime
#define mktime    my_mktime
#define strftime  my_strftime

#include "../src/libc/time/gmtime.c"
#include "../src/libc/time/strftime.c"

#undef gmtime
#undef localtime
#undef mktime
#undef strftime

static int checks = 0, fails = 0;

static void ck(int ok, const char* what, long t) {
    checks++;
    if (!ok) { printf("FAIL %s at t=%ld\n", what, t); fails++; }
}

static const long TIMES[] = {
    0, 1, 59, 86399, 86400, 86401,
    951782400,          /* 2000-02-29 (leap year) */
    1234567890,         /* 2009-02-13 23:31:30 */
    2147483647,         /* 2038-01-19 03:14:07 */
    4102444800L,        /* 2100-01-01 (not a leap year) */
    -1, -86400, -2208988800L   /* 1900-01-01 */
};

static const char* const FORMATS[] = {
    "%Y-%m-%d", "%H:%M:%S", "%a %A %b %B", "%j", "%U %W %w %u",
    "%e|%d", "%I %p", "%r", "%R", "%T", "%D", "%F", "%c", "%x", "%X",
    "%C %y", "%%", "%n%t", "literal", "%Q", "end%"
};

int main(void) {
    setlocale(LC_ALL, "C");

    for (unsigned i = 0; i < sizeof(TIMES) / sizeof(TIMES[0]); i++) {
        long t = TIMES[i];

        struct tm* got = my_gmtime((time_t*)&t);
        struct tm* want = gmtime((time_t*)&t);

        ck(got->tm_sec == want->tm_sec, "tm_sec", t);
        ck(got->tm_min == want->tm_min, "tm_min", t);
        ck(got->tm_hour == want->tm_hour, "tm_hour", t);
        ck(got->tm_mday == want->tm_mday, "tm_mday", t);
        ck(got->tm_mon == want->tm_mon, "tm_mon", t);
        ck(got->tm_year == want->tm_year, "tm_year", t);
        ck(got->tm_wday == want->tm_wday, "tm_wday", t);
        ck(got->tm_yday == want->tm_yday, "tm_yday", t);

        /* mktime round-trip: the struct we produced maps back to the instant */
        struct tm round = *got;
        ck(my_mktime(&round) == (time_t)t, "mktime round-trip", t);
        ck(round.tm_wday == want->tm_wday && round.tm_yday == want->tm_yday,
           "mktime fills wday/yday", t);

        /* localtime is UTC here, so it must agree with the host's gmtime */
        struct tm* lt = my_localtime((time_t*)&t);
        ck(lt->tm_hour == want->tm_hour && lt->tm_mday == want->tm_mday,
           "localtime == UTC", t);

        for (unsigned f = 0; f < sizeof(FORMATS) / sizeof(FORMATS[0]); f++) {
            char a[128], b[128];
            size_t na = my_strftime(a, sizeof a, FORMATS[f], got);
            size_t nb = strftime(b, sizeof b, FORMATS[f], want);
            checks++;
            if (na != nb || strcmp(a, b) != 0) {
                printf("FAIL strftime \"%s\" t=%ld: got '%s' want '%s'\n",
                       FORMATS[f], t, a, b);
                fails++;
            }
        }
    }

    /* truncation: a short buffer returns 0 and still terminates */
    {
        char small[8];
        size_t n = my_strftime(small, sizeof small, "%Y-%m-%d", my_gmtime((time_t*)&TIMES[8]));
        ck(n == 0, "strftime truncation returns 0", TIMES[8]);
        ck(small[sizeof small - 1] == '\0', "strftime truncation terminates", TIMES[8]);
    }

    printf("time host test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
