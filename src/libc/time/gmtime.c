#include <time.h>

// Calendar conversion, proleptic Gregorian, using Howard Hinnant's
// days_from_civil / civil_from_days (exact for the whole range we care about,
// and branch-free in the interesting parts).

static long days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

static void civil_from_days(long z, int* y, int* m, int* d) {
    z += 719468L;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y = (int)(yy + (*m <= 2));
}

struct tm* gmtime(const time_t* t) {
    static struct tm out;
    long secs = t ? (long)*t : 0;

    long days = secs / 86400;
    long rem = secs % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }   // floor division

    int y, m, d;
    civil_from_days(days, &y, &m, &d);

    out.tm_hour = (int)(rem / 3600);
    out.tm_min  = (int)((rem % 3600) / 60);
    out.tm_sec  = (int)(rem % 60);
    out.tm_year = y - 1900;
    out.tm_mon  = m - 1;
    out.tm_mday = d;

    int wd = (int)((days + 4) % 7);             // 1970-01-01 was a Thursday
    if (wd < 0) wd += 7;
    out.tm_wday = wd;
    out.tm_yday = (int)(days - days_from_civil(y, 1, 1));
    out.tm_isdst = 0;
    return &out;
}

struct tm* localtime(const time_t* t) {
    // There is one timezone, and it is UTC.
    return gmtime(t);
}

time_t mktime(struct tm* tm) {
    long days = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    long secs = days * 86400L + (long)tm->tm_hour * 3600L
              + (long)tm->tm_min * 60L + tm->tm_sec;
    time_t t = (time_t)secs;

    // The standard wants tm_wday/tm_yday filled in on return.
    struct tm* norm = gmtime(&t);
    tm->tm_wday = norm->tm_wday;
    tm->tm_yday = norm->tm_yday;
    tm->tm_isdst = 0;
    return t;
}
