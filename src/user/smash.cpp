#include <stdio.h>

// smash — deliberately overruns a stack buffer to prove the stack-protector
// wiring detects corruption: __stack_chk_fail -> loud message + exit("SMASH").

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("smash: overrunning a stack buffer...\n");

    volatile char buf[4];
    volatile char* p = (volatile char*)buf;
    // Exactly buf (4 bytes) + the canary (8) that sits right above it in
    // the frame — enough to corrupt the canary, nothing else. Unbounded
    // overruns fault on the stack guard page before the epilogue ever
    // checks, which tests the fault contract instead of this check.
    for (int i = 0; i < 12; i++) p[i] = 'X';

    printf("smash: SURVIVED?! (stack protector failed)\n");
    return 0;
}
