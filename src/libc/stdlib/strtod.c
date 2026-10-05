#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/* strtod — decimal to double, accumulated in 80-bit long double so a full
   precision digit string still rounds correctly into a double. */

static long double pow10l(int e) {
    long double r = 1.0L, b = 10.0L;
    unsigned int n = (unsigned int)(e < 0 ? -(long)e : e);
    while (n) {
        if (n & 1) r *= b;
        b *= b;
        n >>= 1;
    }
    return e < 0 ? 1.0L / r : r;
}

static int match_ci(const char* s, const char* word) {
    int i = 0;
    for (; word[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != word[i]) return 0;
    }
    return i;
}

double strtod(const char* s, char** end) {
    const char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\v' || *p == '\f')
        p++;

    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    int n = match_ci(p, "inf");
    if (n) {
        p += n;
        if (match_ci(p, "inity")) p += 5;
        if (end) *end = (char*)p;
        return neg ? -INFINITY : INFINITY;
    }
    if (match_ci(p, "nan")) {
        p += 3;
        if (*p == '(') { // nan(chars)
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
        }
        if (end) *end = (char*)p;
        return NAN;
    }

    long double mant = 0.0L;
    int digits = 0;
    int exp_adj = 0;
    while (isdigit((unsigned char)*p)) {
        mant = mant * 10.0L + (*p - '0');
        p++;
        digits++;
    }
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) {
            mant = mant * 10.0L + (*p - '0');
            p++;
            digits++;
            exp_adj--;
        }
    }
    if (digits == 0) {
        if (end) *end = (char*)s; // nothing consumed
        return 0.0;
    }

    int exp = 0;
    if (*p == 'e' || *p == 'E') {
        const char* e = p + 1;
        int esign = 0;
        if (*e == '+' || *e == '-') { esign = (*e == '-'); e++; }
        if (isdigit((unsigned char)*e)) {
            while (isdigit((unsigned char)*e)) {
                if (exp < 100000) exp = exp * 10 + (*e - '0');
                e++;
            }
            if (esign) exp = -exp;
            p = e;
        }
    }
    exp += exp_adj;

    long double val = mant;
    if (exp > 0) {
        while (exp > 1000) { val *= pow10l(1000); exp -= 1000; }
        val *= pow10l(exp);
    } else if (exp < 0) {
        while (exp < -1000) { val /= pow10l(1000); exp += 1000; }
        val /= pow10l(-exp);
    }

    if (end) *end = (char*)p;
    return neg ? (double)-val : (double)val;
}

double atof(const char* s) {
    return strtod(s, 0);
}