#include <stdio.h>
#include <grv/syscall.h>

// hang — sleeps forever. The Ctrl+C test runs it in the foreground and
// interrupts it; without the interrupt the shell would block forever.

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("hang: running\n");
    while (1) grv_sleep(100);
    return 0;
}
