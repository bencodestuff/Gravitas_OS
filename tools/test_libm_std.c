/* Host validation harness for src/libc/libm/libm.c.
   Compiles our implementations under my_* names in the same translation unit
   and compares them with the host's libm over dense ranges. Run on the host
   (not in the guest):

       cc -O2 -fno-builtin tools/test_libm_std.c -o /tmp/test_libm_std -lm

   The x87 asm compiles natively on x86_64. */

#include <stdio.h>
#include <math.h>

#define fabs     my_fabs
#define floor    my_floor
#define ceil     my_ceil
#define trunc    my_trunc
#define round    my_round
#define fmod     my_fmod
#define sqrt     my_sqrt
#define pow      my_pow
#define exp      my_exp
#define exp2     my_exp2
#define log      my_log
#define log2     my_log2
#define log10    my_log10
#define sin      my_sin
#define cos      my_cos
#define tan      my_tan
#define asin     my_asin
#define acos     my_acos
#define atan     my_atan
#define atan2    my_atan2
#define sinh     my_sinh
#define cosh     my_cosh
#define tanh     my_tanh
#define frexp    my_frexp
#define ldexp    my_ldexp
#define modf     my_modf
#define copysign my_copysign
#define fmin     my_fmin
#define fmax     my_fmax
#define nan      my_nan

#include "../src/libc/libm/libm.c"

#undef fabs
#undef floor
#undef ceil
#undef trunc
#undef round
#undef fmod
#undef sqrt
#undef pow
#undef exp
#undef exp2
#undef log
#undef log2
#undef log10
#undef sin
#undef cos
#undef tan
#undef asin
#undef acos
#undef atan
#undef atan2
#undef sinh
#undef cosh
#undef tanh
#undef frexp
#undef ldexp
#undef modf
#undef copysign
#undef fmin
#undef fmax
#undef nan

static int fails = 0;
static int checks = 0;

static void chk(const char* what, double x, double got, double want) {
    checks++;
    double d = got - want;
    if (d < 0) d = -d;
    double m = want < 0 ? -want : want;
    double tol = 1e-14 * m + 1e-14;
    if (d > tol) {
        printf("MISMATCH %-10s x=%-24.17g got=%-24.17g want=%-24.17g\n",
               what, x, got, want);
        fails++;
    }
}

int main(void) {
    for (double x = 0.0001; x < 100.0; x *= 1.0007) {
        chk("sqrt", x, my_sqrt(x), sqrt(x));
        chk("log", x, my_log(x), log(x));
        chk("log2", x, my_log2(x), log2(x));
        chk("log10", x, my_log10(x), log10(x));
        chk("exp", x, my_exp(x), exp(x));
        chk("exp2", x, my_exp2(x), exp2(x));
        chk("sin", x, my_sin(x), sin(x));
        chk("cos", x, my_cos(x), cos(x));
        chk("tan", x, my_tan(x), tan(x));
        chk("atan", x, my_atan(x), atan(x));
        chk("asin", x / 101.0, my_asin(x / 101.0), asin(x / 101.0));
        chk("acos", x / 101.0, my_acos(x / 101.0), acos(x / 101.0));
        chk("sinh", x / 10.0, my_sinh(x / 10.0), sinh(x / 10.0));
        chk("cosh", x / 10.0, my_cosh(x / 10.0), cosh(x / 10.0));
        chk("tanh", x / 10.0, my_tanh(x / 10.0), tanh(x / 10.0));

        double y = x * 0.37 + 0.001;
        chk("pow", x, my_pow(x, y), pow(x, y));
        chk("fmod", x, my_fmod(x, y), fmod(x, y));
        chk("atan2", x, my_atan2(x, y), atan2(x, y));
        chk("floor", x, my_floor(-x), floor(-x));
        chk("ceil", x, my_ceil(-x), ceil(-x));
        chk("trunc", x, my_trunc(-x), trunc(-x));
        chk("round", x, my_round(-x), round(-x));
        chk("fabs", -x, my_fabs(-x), fabs(-x));

        int e1 = 0, e2 = 0;
        double f1 = my_frexp(x, &e1);
        double f2 = frexp(x, &e2);
        if (e1 != e2) { printf("MISMATCH frexp exp x=%g %d vs %d\n", x, e1, e2); fails++; }
        chk("frexp", x, f1, f2);
        chk("ldexp", x, my_ldexp(f1, e1), ldexp(f2, e2));

        double i1 = 0, i2 = 0;
        double m1 = my_modf(-x, &i1);
        double m2 = modf(-x, &i2);
        chk("modf", x, m1, m2);
        chk("modf i", x, i1, i2);
    }

    chk("pow int", -2.0, my_pow(-2.0, 3.0), pow(-2.0, 3.0));
    chk("pow int", 2.0, my_pow(2.0, -3.0), pow(2.0, -3.0));
    chk("pow odd", -3.0, my_pow(-3.0, 5.0), pow(-3.0, 5.0));
    chk("pow 0^0", 0.0, my_pow(0.0, 0.0), pow(0.0, 0.0));
    chk("pow 0^2", 0.0, my_pow(0.0, 2.0), pow(0.0, 2.0));

    checks++;
    if (!isnan(my_pow(-2.0, 0.5))) { printf("MISMATCH pow(-2,0.5) not NaN\n"); fails++; }
    checks++;
    if (my_log(-1.0) == my_log(-1.0)) { printf("MISMATCH log(-1) not NaN\n"); fails++; }
    checks++;
    if (my_log(0.0) != -INFINITY) { printf("MISMATCH log(0) != -inf\n"); fails++; }
    checks++;
    if (my_exp(1000.0) != INFINITY) { printf("MISMATCH exp(1000) != inf\n"); fails++; }
    checks++;
    if (my_atan2(0.0, 0.0) != 0.0) { printf("MISMATCH atan2(0,0)\n"); fails++; }
    checks++;
    if (my_floor(3.0) != 3.0 || my_ceil(3.0) != 3.0) {
        printf("MISMATCH floor/ceil integral\n");
        fails++;
    }

    printf("libm host test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}