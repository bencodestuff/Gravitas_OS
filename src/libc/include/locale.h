#ifndef _LOCALE_H
#define _LOCALE_H 1

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Gravitas has exactly one locale: "C". The category constants exist so ported
   code compiles; setlocale accepts "" / "C" / "POSIX" and rejects the rest,
   and localeconv reports the C conventions ('.' as the decimal point). */

#define LC_CTYPE    0
#define LC_NUMERIC  1
#define LC_TIME     2
#define LC_COLLATE  3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL      6

struct lconv {
    char* decimal_point;
    char* thousands_sep;
    char* grouping;
    char* int_curr_symbol;
    char* currency_symbol;
    char* mon_decimal_point;
    char* mon_thousands_sep;
    char* mon_grouping;
    char* positive_sign;
    char* negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

/* Query with a NULL locale; returns the current name, or NULL if the requested
   locale is not supported. */
char* setlocale(int category, const char* locale);

/* Returns the C conventions (a shared static struct; do not free). */
struct lconv* localeconv(void);

__END_DECLS

#endif
