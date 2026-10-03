#include <stdio.h>

// crash — deliberately faults to prove the fault contract.
// Writes through a null pointer: ring-3 page fault -> the kernel dumps
// registers + kills this task, and the shell (and OS) must survive.

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("crash: about to dereference NULL...\n");

    volatile int* p = nullptr;
    *p = 42;   // ring-3 #PF — this process should die, nothing else

    printf("crash: SURVIVED?! (fault contract failed)\n");
    return 0;
}
