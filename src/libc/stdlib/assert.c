#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// The failure path of assert(): say what and where, then die. Goes to stderr
// so it is not interleaved into a program's normal output.
void __assert_fail(const char* expr, const char* file, int line) {
    fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expr);
    abort();
}
