#include <stdio.h>
#include <grv/syscall.h>

// echoprog — reads one line of input and prints it back as "echo:<line>".
// Used by consoletest to exercise BOTH directions of the console endpoint
// (owner-injected input, and output captured by the owner).

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    char line[64];
    int n = 0;
    while (n < 63) {
        char c = grv_getc();
        if (c == '\n') break;
        line[n++] = c;
    }
    line[n] = '\0';
    printf("echo:%s\n", line);
    return 0;
}
