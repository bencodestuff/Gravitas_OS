/* Host validation harness for the libc string/ctype/stdlib helpers added for
   the Lua port. Compiles our implementations under my_* names in this
   translation unit and compares them against the host's C library:

       cc -O2 -fno-builtin -Isrc/libc/include tools/test_string.c -o /tmp/test_string

   Run by tests/host/run.sh (`ninja check`). */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>

/* glibc defines several of these as (function-like) macros; clear them so the
   my_* renames below do not collide. The same names are cleared again after
   the includes, which also restores the host's real functions for the test. */
#undef memcmp
#undef memchr
#undef strchr
#undef strrchr
#undef strstr
#undef strnlen
#undef strspn
#undef strcspn
#undef strpbrk
#undef strsep
#undef stpcpy
#undef strcasecmp
#undef strncasecmp
#undef strcoll
#undef strtol
#undef strtoul
#undef atoi
#undef abs
#undef isdigit
#undef isspace
#undef isalpha
#undef isalnum
#undef isupper
#undef islower
#undef isxdigit
#undef isprint
#undef isgraph
#undef ispunct
#undef iscntrl
#undef toupper
#undef tolower

#define memcmp      my_memcmp
#define memchr      my_memchr
#define strchr      my_strchr
#define strrchr     my_strrchr
#define strstr      my_strstr
#define strnlen     my_strnlen
#define strspn      my_strspn
#define strcspn     my_strcspn
#define strpbrk     my_strpbrk
#define strsep      my_strsep
#define stpcpy      my_stpcpy
#define strcasecmp  my_strcasecmp
#define strncasecmp my_strncasecmp
#define strcoll     my_strcoll
#define strtol      my_strtol
#define strtoul     my_strtoul
#define atoi        my_atoi
#define abs         my_abs
#define isdigit     my_isdigit
#define isspace     my_isspace
#define isalpha     my_isalpha
#define isalnum     my_isalnum
#define isupper     my_isupper
#define islower     my_islower
#define isxdigit    my_isxdigit
#define isprint     my_isprint
#define isgraph     my_isgraph
#define ispunct     my_ispunct
#define iscntrl     my_iscntrl
#define toupper     my_toupper
#define tolower     my_tolower

#include "../src/libc/string/memcmp.c"
#include "../src/libc/string/strchr.c"
#include "../src/libc/string/stpcpy.c"
#include "../src/libc/string/strcasecmp.c"
#include "../src/libc/stdlib/strtol.c"
#include "../src/libc/stdlib/abs.c"
#include "../src/libc/ctype/ctype.c"

#undef memcmp
#undef memchr
#undef strchr
#undef strrchr
#undef strstr
#undef strnlen
#undef strspn
#undef strcspn
#undef strpbrk
#undef strsep
#undef stpcpy
#undef strcasecmp
#undef strncasecmp
#undef strcoll
#undef strtol
#undef strtoul
#undef atoi
#undef abs
#undef isdigit
#undef isspace
#undef isalpha
#undef isalnum
#undef isupper
#undef islower
#undef isxdigit
#undef isprint
#undef isgraph
#undef ispunct
#undef iscntrl
#undef toupper
#undef tolower

static int checks = 0;
static int fails = 0;

static void ck(const char* what, int ok) {
    checks++;
    if (!ok) { printf("FAIL %s\n", what); fails++; }
}

static void cks(const char* what, const char* got, const char* want) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got '%s' want '%s'\n", what, got, want);
        fails++;
    }
}

int main(void) {
    // memchr
    const char* s = "hello world";
    ck("memchr found",   my_memchr(s, 'w', 11) == s + 6);
    ck("memchr missing", my_memchr(s, 'z', 11) == NULL);
    ck("memchr empty",   my_memchr(s, 'h', 0) == NULL);
    static const char nulbuf[6] = {'a', 'b', '\0', 'c', 'd', '\0'};
    ck("memchr NUL",     my_memchr(nulbuf, '\0', 5) == nulbuf + 2);

    // stpcpy
    char buf[16];
    char* end = my_stpcpy(buf, "abc");
    ck("stpcpy return", end == buf + 3 && *end == '\0');
    cks("stpcpy text", buf, "abc");

    // strspn / strcspn / strpbrk
    ck("strspn",       my_strspn("abcXYZ", "abc") == 3);
    ck("strspn none",  my_strspn("xyz", "abc") == 0);
    ck("strspn all",   my_strspn("aaa", "a") == 3);
    ck("strcspn",      my_strcspn("abc:def", ":") == 3);
    ck("strcspn none", my_strcspn("abc", ":") == 3);
    ck("strpbrk",      my_strpbrk("hello", "xyzl") == strchr("hello", 'l'));
    ck("strpbrk none", my_strpbrk("hello", "xyz") == NULL);

    // strsep (keeps empty fields; ends by clearing the cursor)
    char toks[] = "a,b,,c";
    char* p = toks;
    cks("strsep 1", my_strsep(&p, ","), "a");
    cks("strsep 2", my_strsep(&p, ","), "b");
    cks("strsep 3", my_strsep(&p, ","), "");
    cks("strsep 4", my_strsep(&p, ","), "c");
    ck("strsep end", my_strsep(&p, ",") == NULL);

    // strcasecmp / strncasecmp / strcoll, also against the host
    static const char* pairs[][2] = {
        {"", ""}, {"a", "A"}, {"AbC", "aBc"}, {"abc", "abd"},
        {"Z", "a"}, {"aa", "a"}, {"HELLO", "hello"}, {"abc", "ABC"}
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        int got = my_strcasecmp(pairs[i][0], pairs[i][1]);
        int want = strcasecmp(pairs[i][0], pairs[i][1]);
        ck("strcasecmp vs host", (got == 0) == (want == 0) && (got < 0) == (want < 0));
        int gn = my_strncasecmp(pairs[i][0], pairs[i][1], 2);
        int wn = strncasecmp(pairs[i][0], pairs[i][1], 2);
        ck("strncasecmp vs host", (gn == 0) == (wn == 0) && (gn < 0) == (wn < 0));
    }
    ck("strncasecmp n=0", my_strncasecmp("x", "y", 0) == 0);
    ck("strcoll", my_strcoll("abc", "abd") < 0);

    // ctype
    ck("isgraph space", !my_isgraph(' '));
    ck("isgraph bang",  my_isgraph('!') && my_isgraph('0') && my_isgraph('~'));
    ck("isgraph DEL",   !my_isgraph(127) && !my_isgraph('\n'));
    ck("ispunct",       my_ispunct('!') && !my_ispunct('a') && !my_ispunct(' '));
    ck("toupper",       my_toupper('q') == 'Q' && my_toupper('Q') == 'Q');
    ck("tolower",       my_tolower('Q') == 'q' && my_tolower('q') == 'q');

    // abs
    ck("abs neg",  my_abs(-5) == 5);
    ck("abs pos",  my_abs(7) == 7);
    ck("abs zero", my_abs(0) == 0);
    ck("abs max",  my_abs(-2147483647) == 2147483647);

    // strtoul, compared with the host for in-range literals
    static const char* nums[] = {
        "0", "42", "4294967295", "0x10", "0XfF", "007", "12345", "-1", "-42",
        "  +17", "999999999999"
    };
    for (unsigned i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
        char* e1; char* e2;
        unsigned long got = my_strtoul(nums[i], &e1, 0);
        unsigned long want = strtoul(nums[i], &e2, 0);
        ck("strtoul value vs host", got == want);
        ck("strtoul end vs host", e1 - nums[i] == e2 - nums[i]);
    }
    ck("strtoul base10", my_strtoul("ff", NULL, 16) == 255);
    ck("strtoul huge",   my_strtoul("18446744073709551615", NULL, 10) == ~0UL);

    // strtol regression (end pointer past the digits)
    char* e;
    long v = my_strtol(" -42abc", &e, 10);
    ck("strtol value", v == -42);
    cks("strtol end", e, "abc");
    ck("atoi", my_atoi("  99") == 99);

    printf("string host test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
