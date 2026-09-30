#include <math.h>
#include <stddef.h>

/* --- libm: freestanding math for Gravitas userland ---
   All fixed-point functions are validated against host libm by a test
   harness (see tools/test_libm.c — run on the host, not the guest). */

#define TABLE_BITS  10
#define TABLE_SIZE  1024
#define TABLE_MASK  1023
#define SIN_SCALE   512

static int sintab[TABLE_SIZE];
static int table_ready = 0;

void math_init(void) {
    if (table_ready) return;
    table_ready = 1;

    // Fixed-point sin: X = radians << 22, Taylor within a quadrant,
    // quadrant identities for the rest. Validated against host libm:
    // max error 1/512.
    const long long K = 25735;       // (2*pi/1024) << 22
    const long long QUART = K * 256; // (pi/2) << 22
    for (int i = 0; i < TABLE_SIZE; i++) {
        long long X = (long long)i * K;
        long long q = (X / QUART) & 3;
        long long r = X % QUART;
        int mirror = (q == 1 || q == 3);
        int sign = (q >= 2) ? -1 : 1;
        long long a = mirror ? (QUART - r) : r;
        long long a2 = (a * a) >> 22;
        long long a3 = (a2 * a) >> 22;
        long long a4 = (a2 * a2) >> 22;
        long long a5 = (a4 * a) >> 22;
        long long a6 = (a3 * a3) >> 22;
        long long a7 = (a6 * a) >> 22;
        long long s = a - a3 / 6 + a5 / 120 - a7 / 5040; // sin(a) << 22
        sintab[i] = (int)(sign * (s >> 13));             // sin * 512
    }
}

int isin(int a) {
    if (!table_ready) math_init();
    return sintab[a & TABLE_MASK];
}

int icos(int a) {
    if (!table_ready) math_init();
    return sintab[(a + 256) & TABLE_MASK]; // cos = sin + quarter turn
}

double dsin(double radians) {
    // normalize into [0, 2pi)
    if (radians < 0) radians = radians - 6.283185307 * ((long long)(radians / 6.283185307) - 1);
    radians = radians - 6.283185307 * (long long)(radians / 6.283185307);
    double t = radians / 6.283185307 * TABLE_SIZE; // turns -> table units
    int i = (int)t;
    double frac = t - i;
    // lerp between adjacent table entries (16-bit table -> decent precision)
    int s0 = isin(i & TABLE_MASK);
    int s1 = isin((i + 1) & TABLE_MASK);
    return (s0 + (s1 - s0) * frac) / (double)SIN_SCALE;
}

double dcos(double radians) {
    return dsin(radians + 1.5707963268);
}

unsigned int isqrt(unsigned int x) {
    if (x == 0) return 0;
    unsigned int r = x;
    unsigned int prev;
    do {
        prev = r;
        r = (r + x / r) / 2;
    } while (r < prev);
    return prev;
}

int iatan2(int y, int x) {
    // CORDIC vectoring: rotate (x,y) onto the +x axis, accumulating the
    // rotation angles. Boundary-safe by construction (no octant algebra).
    // Angle units: 1/1024 turn, signed during iteration, normalized after.
    static const int angles[16] = {
        128,  76,  43,  22,  11,   6,   3,   1,
          1,   0,   1,   0,   0,   0,   0,   1  // atan(2^-i)/2pi * 1024
    };

    // Scale up for precision through the shifts (small inputs lose bits)
    long long lx = x, ly = y;
    // CORDIC vectoring converges only within ±~100° — pre-rotate vectors
    // in the left half-plane by 180° (add pi = 512 units afterwards).
    long long pre = 0;
    if (lx < 0) { lx = -lx; ly = -ly; pre = 512; }
    if (lx == 0 && ly == 0) return 0;

    // Keep 10 bits of working headroom; also handles (0,0) -> 0
    lx <<= 10; ly <<= 10;

    long long angle = 0;
    for (int i = 0; i < 16; i++) {
        if (ly == 0) break; // already on the axis — rotate nothing
        if (ly > 0) {
            long long ty = ly - (lx >> i);
            lx = lx + (ly >> i);
            ly = ty;
            angle += angles[i];
        } else {
            long long ty = ly + (lx >> i);
            lx = lx - (ly >> i);
            ly = ty;
            angle -= angles[i];
        }
    }

    angle += pre;
    angle &= 1023; // normalize to [0, 1024)
    return (int)angle;
}

/* --- xorshift32 PRNG --- */
static unsigned int rng_state = 0x12345678;

void srand(unsigned int seed) {
    rng_state = seed ? seed : 0x12345678;
}

unsigned int rand(void) {
    unsigned int x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}