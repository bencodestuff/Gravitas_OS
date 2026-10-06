#include <locale.h>
#include <limits.h>
#include <stddef.h>

// The C locale is the only one there is. setlocale(NULL, ...) queries the
// current locale; "C", "POSIX" and "" all select it. Anything else is
// unsupported — the standard lets us answer NULL.
char* setlocale(int category, const char* locale) {
    (void)category;
    if (locale == NULL) return (char*)"C";
    if (locale[0] == '\0') return (char*)"C";
    if (locale[0] == 'C' && locale[1] == '\0') return (char*)"C";
    if (locale[0] == 'P' && locale[1] == 'O' && locale[2] == 'S' &&
        locale[3] == 'I' && locale[4] == 'X' && locale[5] == '\0')
        return (char*)"C";
    return NULL;
}

// The C conventions. The fields Lua reads (decimal_point above all) are exact;
// the monetary ones are "unspecified" (CHAR_MAX) as the standard prescribes.
struct lconv* localeconv(void) {
    static struct lconv lc = {
        .decimal_point     = (char*)".",
        .thousands_sep     = (char*)"",
        .grouping          = (char*)"",
        .int_curr_symbol   = (char*)"",
        .currency_symbol   = (char*)"",
        .mon_decimal_point = (char*)"",
        .mon_thousands_sep = (char*)"",
        .mon_grouping      = (char*)"",
        .positive_sign     = (char*)"",
        .negative_sign     = (char*)"",
        .int_frac_digits   = CHAR_MAX,
        .frac_digits       = CHAR_MAX,
        .p_cs_precedes     = CHAR_MAX,
        .p_sep_by_space    = CHAR_MAX,
        .n_cs_precedes     = CHAR_MAX,
        .n_sep_by_space    = CHAR_MAX,
        .p_sign_posn       = CHAR_MAX,
        .n_sign_posn       = CHAR_MAX,
        .int_p_cs_precedes = CHAR_MAX,
        .int_p_sep_by_space = CHAR_MAX,
        .int_n_cs_precedes = CHAR_MAX,
        .int_n_sep_by_space = CHAR_MAX,
        .int_p_sign_posn   = CHAR_MAX,
        .int_n_sign_posn   = CHAR_MAX,
    };
    return &lc;
}
