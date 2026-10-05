#include <stdio.h>

// stackoverflow — recurse until the user stack guard page is hit. The kernel
// must report a stack overflow (not a random wild fault) and kill only this
// task, leaving the system running.

static volatile int stop = 0; // never set: the descent is meant to run away
static int depth = 0;

__attribute__((noinline))
static void plunge(void) {
    char pad[512];
    pad[0] = (char)depth;
    depth++;
    if (stop == 0) plunge(); // not a tail call: pad stays live across it
    (void)pad[0];
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    plunge();
    printf("stackoverflow: returned (no guard hit)\n");
    return 1;
}