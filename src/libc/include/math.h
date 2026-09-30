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

/* Floating-point versions (FPU context save/restore is per-task, so
   these are safe in any task). */
double dsin(double radians);
double dcos(double radians);

/* Integer sqrt via Newton's method (rounds down). */
unsigned int isqrt(unsigned int x);

/* atan2 in 1/1024-turns units (0..1023), quadrant-correct. */
int iatan2(int y, int x);

/* Deterministic PRNG (xorshift32). */
void  srand(unsigned int seed);
unsigned int rand(void);

__END_DECLS

#endif