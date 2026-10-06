#include <math.h>

/* --- The standard C math functions, on the x87 FPU ---
   Userland is SSE, so doubles travel in xmm; inside these functions the
   transcendental instructions do the work: fsin/fcos/fptan/fpatan for trig,
   fyl2x for logarithms, f2xm1+fscale for exp/exp2/pow, fprem for fmod.
   That is compact and accurate to a couple of ulp — validated against the
   host libm by tools/test_libm_std.c.

   Every x87 sequence must leave the FPU stack empty: the context switch
   saves/restores the whole 512-byte FPU state, so a leaked register would
   accumulate across calls (and travel between tasks). */

union Dq { double d; unsigned long u; };

static int int_of_double(double x, long long* out) {
    if (x != x) return 0; // NaN
    if (x >= 9223372036854775808.0 || x < -9223372036854775808.0) return 0;
    *out = (long long)x;
    return 1;
}

// True when x is exactly an integer that fits in long long.
static int exact_int(double x, long long* out) {
    if (!int_of_double(x, out)) return 0;
    return (double)*out == x;
}

double fabs(double x) {
    union Dq v;
    v.d = x;
    v.u &= ~(1UL << 63);
    return v.d;
}

double copysign(double x, double y) {
    union Dq a, b;
    a.d = x;
    b.d = y;
    a.u = (a.u & ~(1UL << 63)) | (b.u & (1UL << 63));
    return a.d;
}

double trunc(double x) {
    long long i;
    if (!int_of_double(x, &i)) return x; // NaN, inf, or |x| >= 2^63
    return (double)i;
}

double floor(double x) {
    long long i;
    if (!int_of_double(x, &i)) return x;
    double t = (double)i;
    return (t > x) ? t - 1.0 : t;
}

double ceil(double x) {
    long long i;
    if (!int_of_double(x, &i)) return x;
    double t = (double)i;
    return (t < x) ? t + 1.0 : t;
}

double round(double x) {
    if (x >= 0.0) return floor(x + 0.5); // halves away from zero
    return ceil(x - 0.5);
}

// Non-standard integer conversions (an eLua-style convention). They truncate
// toward the floor/ceiling and wrap like a C cast for out-of-range values.
int ifloor(double x) { return (int)floor(x); }
int iceil(double x)  { return (int)ceil(x); }

double fmin(double a, double b) {
    if (a != a) return b;
    if (b != b) return a;
    return a < b ? a : b;
}

double fmax(double a, double b) {
    if (a != a) return b;
    if (b != b) return a;
    return a > b ? a : b;
}

double nan(const char* tag) {
    (void)tag;
    return __builtin_nan("");
}

double sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double fmod(double x, double y) {
    double r;
    // fprem reduces partially and sets C2 when the quotient did not fit;
    // loop until the full remainder is in. Exact by construction.
    __asm__ volatile(
        "fldl %[y]\n\t"
        "fldl %[x]\n\t"
        "1:\n\t"
        "fprem\n\t"
        "fnstsw %%ax\n\t"
        "testb $0x04, %%ah\n\t"  // C2: partial remainder
        "jnz 1b\n\t"
        "fstpl %[r]\n\t"
        "fstp %%st(0)\n\t"       // drop the divisor
        : [r] "=m"(r)
        : [x] "m"(x), [y] "m"(y)
        : "ax", "memory");
    return r;
}

double log2(double x) {
    double r;
    __asm__ volatile(
        "fld1\n\t"
        "fldl %[x]\n\t"
        "fyl2x\n\t"
        "fstpl %[r]\n\t"
        : [r] "=m"(r) : [x] "m"(x) : "memory");
    return r;
}

double log(double x) {
    double r;
    __asm__ volatile(
        "fldln2\n\t"
        "fldl %[x]\n\t"
        "fyl2x\n\t"
        "fstpl %[r]\n\t"
        : [r] "=m"(r) : [x] "m"(x) : "memory");
    return r;
}

double log10(double x) {
    double r;
    __asm__ volatile(
        "fldlg2\n\t"
        "fldl %[x]\n\t"
        "fyl2x\n\t"
        "fstpl %[r]\n\t"
        : [r] "=m"(r) : [x] "m"(x) : "memory");
    return r;
}

double exp2(double x) {
    double r;
    // st0 = x. Split x = n + f with n = round(x); 2^x = 2^f * 2^n.
    // NOTE: in AT&T x87 syntax `fsubr %st(0), %st(1)` means st1 = st1 - st0
    // (the Intel `fsub`); `fsub` would reverse the sign of the fraction.
    __asm__ volatile(
        "fldl %[x]\n\t"
        "fld %%st(0)\n\t"
        "frndint\n\t"
        "fsubr %%st(0), %%st(1)\n\t" // st1 = x - n  (|f| <= 0.5)
        "fxch %%st(1)\n\t"
        "f2xm1\n\t"                 // 2^f - 1
        "fld1\n\t"
        "faddp\n\t"                 // 2^f
        "fscale\n\t"                // * 2^n
        "fstp %%st(1)\n\t"
        "fstpl %[r]\n\t"
        : [r] "=m"(r)
        : [x] "m"(x)
        : "st", "st(1)", "st(2)", "memory");
    return r;
}

double exp(double x) {
    double r;
    __asm__ volatile(
        "fldl2e\n\t"                // log2(e)
        "fmull %[x]\n\t"            // x * log2(e)
        "fld %%st(0)\n\t"
        "frndint\n\t"
        "fsubr %%st(0), %%st(1)\n\t" // st1 = x*log2(e) - n
        "fxch %%st(1)\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fscale\n\t"
        "fstp %%st(1)\n\t"
        "fstpl %[r]\n\t"
        : [r] "=m"(r)
        : [x] "m"(x)
        : "st", "st(1)", "st(2)", "memory");
    return r;
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;      // includes 0^0
    if (x == 1.0) return 1.0;
    if (x != x || y != y) return __builtin_nan("");

    long long iy;
    if (exact_int(y, &iy)) {
        // Integer exponent: exponentiation by squaring is exact and keeps
        // the sign of a negative base.
        if (iy == 0) return 1.0;
        int neg = iy < 0;
        unsigned long long n = neg ? (unsigned long long)(-(iy + 1)) + 1
                                   : (unsigned long long)iy;
        double r = 1.0, b = x;
        while (n) {
            if (n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        return neg ? 1.0 / r : r;
    }

    if (x == 0.0) return y > 0.0 ? 0.0 : __builtin_huge_val();
    if (x < 0.0) return __builtin_nan(""); // non-integer power of a negative

    // x^y = 2^(y*log2(x)), computed entirely in the x87's 80-bit registers so
    // the multiply into the exponent and the final exponential stay precise
    // (going through a 64-bit exp2(y*log2(x)) loses ~1e-14 relative here).
    double r;
    __asm__ volatile(
        "fldl %[y]\n\t"
        "fldl %[x]\n\t"
        "fyl2x\n\t"                  // st0 = y * log2(x), 80-bit
        "fld %%st(0)\n\t"
        "frndint\n\t"
        "fsubr %%st(0), %%st(1)\n\t" // st1 = exponent - n
        "fxch %%st(1)\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fscale\n\t"
        "fstp %%st(1)\n\t"
        "fstpl %[r]\n\t"
        : [r] "=m"(r)
        : [x] "m"(x), [y] "m"(y)
        : "st", "st(1)", "st(2)", "memory");
    return r;
}

double sin(double x) {
    double r;
    __asm__ volatile("fldl %[x]\n\tfsin\n\tfstpl %[r]\n\t"
                     : [r] "=m"(r) : [x] "m"(x) : "memory");
    return r;
}

double cos(double x) {
    double r;
    __asm__ volatile("fldl %[x]\n\tfcos\n\tfstpl %[r]\n\t"
                     : [r] "=m"(r) : [x] "m"(x) : "memory");
    return r;
}

double tan(double x) {
    double r;
    // fptan pushes 1.0 then the tangent: drop the 1.0.
    __asm__ volatile("fldl %[x]\n\tfptan\n\tfstp %%st(0)\n\tfstpl %[r]\n\t"
                     : [r] "=m"(r) : [x] "m"(x) : "memory");
    return r;
}

double atan2(double y, double x) {
    if (x == 0.0 && y == 0.0) return y; // +-0 preserving, like libm
    double r;
    __asm__ volatile("fldl %[y]\n\tfldl %[x]\n\tfpatan\n\tfstpl %[r]\n\t"
                     : [r] "=m"(r) : [x] "m"(x), [y] "m"(y) : "memory");
    return r;
}

double atan(double x) {
    return atan2(x, 1.0);
}

double asin(double x) {
    if (x != x || x > 1.0 || x < -1.0) return __builtin_nan("");
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x != x || x > 1.0 || x < -1.0) return __builtin_nan("");
    return atan2(sqrt(1.0 - x * x), x);
}

double sinh(double x) {
    double e = exp(x);
    return (e - 1.0 / e) * 0.5;
}

double cosh(double x) {
    double e = exp(fabs(x));
    return (e + 1.0 / e) * 0.5;
}

double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e = exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}

double frexp(double x, int* e) {
    *e = 0;
    if (x == 0.0 || x != x || __builtin_isinf(x)) return x;

    union Dq v;
    v.d = x;
    int exp = (int)((v.u >> 52) & 0x7FF);
    if (exp == 0) {
        // Subnormal: normalise by scaling up (2^53), then fix the exponent.
        v.d = x * 9007199254740992.0;
        exp = (int)((v.u >> 52) & 0x7FF) - 53;
    }
    *e = exp - 1022;
    v.u = (v.u & ~(0x7FFUL << 52)) | (1022UL << 52); // scale to [0.5, 1)
    return v.d;
}

static double scale_pow2(int n) {
    // 2^n for -1022 <= n <= 1023, exactly (exponent field only).
    union Dq v;
    v.u = (unsigned long)(n + 1023) << 52;
    return v.d;
}

double ldexp(double x, int n) {
    if (x == 0.0 || x != x || __builtin_isinf(x)) return x;
    while (n > 1023) {
        x *= scale_pow2(1023);
        if (__builtin_isinf(x)) return x;
        n -= 1023;
    }
    while (n < -1022) {
        x *= scale_pow2(-1022);
        if (x == 0.0) return x;
        n += 1022;
    }
    return x * scale_pow2(n);
}

double modf(double x, double* iptr) {
    double t = trunc(x);
    *iptr = t;
    return x - t;
}