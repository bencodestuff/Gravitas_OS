#ifndef _MATH_H
#define _MATH_H 1

#include <sys/cdefs.h>

__BEGIN_DECLS

/* --- Fixed-point trig for graphics/effects (no FPU needed) ---
   Angles are in 1/1024-turns units (0..1023 = one full circle).
   Results are sin*512 / cos*512 (range [-512, 512]).
   Accuracy: validated against host libm, max error 1/512.
   Hot path: O(1) table lookups after math_init(). */

void math_init(void);   // build the sine table (call once; idempotent)

int isin(int a);        // sin(a/1024 turns) * 512
int icos(int a);        // cos(a/1024 turns) * 512

/* Floating-point versions. */
double dsin(double radians);
double dcos(double radians);

/* Integer sqrt via Newton's method (rounds down). */
unsigned int isqrt(unsigned int x);

/* atan2 in 1/1024-turns units (0..1023), quadrant-correct. */
int iatan2(int y, int x);

/* Deterministic PRNG (xorshift32). */
void  srand(unsigned int seed);
unsigned int rand(void);

/* --- Standard C math (freestanding, on the x87 FPU) ---
   Userland runs with SSE enabled, so doubles travel in xmm; the
   transcendental functions use the x87 hardware instructions internally
   (fsin/fcos/fptan/fpatan/fyl2x/f2xm1/fscale), which keeps them compact and
   accurate. Results are within a couple of ulp of the host libm (validated
   by tools/test_libm_std.c, run on the host). */

#define HUGE_VAL   (__builtin_huge_val())
#define INFINITY   (__builtin_huge_val())
#define NAN        (__builtin_nan(""))

#define isinf(x)    __builtin_isinf(x)
#define isnan(x)    __builtin_isnan(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

double fabs(double);
double floor(double);
double ceil(double);
double trunc(double);
double round(double);
double fmod(double, double);
double sqrt(double);

double pow(double, double);
double exp(double);
double exp2(double);
double log(double);
double log2(double);
double log10(double);

double sin(double);
double cos(double);
double tan(double);
double asin(double);
double acos(double);
double atan(double);
double atan2(double, double);
double sinh(double);
double cosh(double);
double tanh(double);

double frexp(double, int*);
double ldexp(double, int);
double modf(double, double*);
double copysign(double, double);
double fmin(double, double);
double fmax(double, double);
double nan(const char*);

/* Non-standard integer conversions some embedded Lua builds reference
   (an eLua-style convention); Lua itself does not call them. */
int ifloor(double);
int iceil(double);

__END_DECLS

#endif