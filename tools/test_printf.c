/* Host validation harness for src/libc/stdio/printf.c.
   Builds our formatter under my_* names and compares vsnprintf output with
   the host's. Run on the host:

       cc -O2 -ffunction-sections -Wl,--gc-sections \
          tools/test_printf.c -o /tmp/test_printf
*/

#include <stdio.h>
#include <string.h>

#define printf    my_printf
#define snprintf  my_snprintf
#define vsnprintf my_vsnprintf
#define sprintf   my_sprintf
#define vsprintf  my_vsprintf
#include "../src/libc/stdio/printf.c"
#undef printf
#undef snprintf
#undef vsnprintf
#undef sprintf
#undef vsprintf

static int fails = 0;
static int checks = 0;

static void chk(const char* what, const char* got, const char* want) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("MISMATCH %-18s got='%s' want='%s'\n", what, got, want);
        fails++;
    }
}

#define CMP(fmt, ...) do { \
    char g[128], w[128]; \
    my_snprintf(g, sizeof(g), fmt, __VA_ARGS__); \
    snprintf(w, sizeof(w), fmt, __VA_ARGS__); \
    chk(fmt, g, w); \
} while (0)

int main(void) {
    CMP("%d", 42);
    CMP("%d", -7);
    CMP("%5d", 42);
    CMP("%-5d|", 42);
    CMP("%05d", 42);
    CMP("%05d", -42);
    CMP("%x", 255);
    CMP("%X", 255);
    CMP("%08x", 255);
    CMP("%o", 8);
    CMP("%c", 'A');
    CMP("%s", "hi");
    CMP("%5s|", "hi");
    CMP("%-5s|", "hi");
    CMP("%.2s", "hello");
    CMP("%ld", 123456789L);
    CMP("%lld", 1234567890123LL);
    CMP("%llu", 18446744073709551615ULL);

    CMP("%f", 3.14159);
    CMP("%.2f", 3.14159);
    CMP("%.0f", 2.5);
    CMP("%.0f", 3.5);
    CMP("%f", -3.25);
    CMP("%.3f", 0.0005);
    CMP("%f", 0.0);
    CMP("%.0f", 0.0);
    CMP("%e", 12345.678);
    CMP("%.2e", 12345.678);
    CMP("%E", 12345.678);
    CMP("%.0e", 9.99);
    CMP("%g", 12345.678);
    CMP("%.14g", 1.5);
    CMP("%.14g", 0.1);
    CMP("%.14g", 3.141592653589793);
    CMP("%.3g", 12345.0);
    CMP("%g", 0.0001);
    CMP("%g", 0.00001);
    CMP("%g", 0.0);
    CMP("%.0g", 123.0);
    CMP("%.10g", 2.0);
    CMP("%g", 100000.0);

    char g[128], w[128];
    my_snprintf(g, sizeof(g), "x=%d y=%.2f s=%s %%", 3, 1.5, "ok");
    snprintf(w, sizeof(w), "x=%d y=%.2f s=%s %%", 3, 1.5, "ok");
    chk("mixed", g, w);

    my_snprintf(g, sizeof(g), "%.14g", 1.0 / 3.0);
    snprintf(w, sizeof(w), "%.14g", 1.0 / 3.0);
    chk("1/3 %.14g", g, w);

    // truncation: snprintf must NUL-terminate and report the full length
    int gn = my_snprintf(g, 4, "%s", "abcdef");
    int wn = snprintf(w, 4, "%s", "abcdef");
    chk("truncate text", g, w);
    checks++;
    if (gn != wn) { printf("MISMATCH truncate len %d vs %d\n", gn, wn); fails++; }

    printf("printf host test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}