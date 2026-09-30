#include <stdio.h>
#include <grv/syscall.h>

// sleeper — prints a message every 500ms, five times.
// The whole point: grv_sleep BLOCKS the task (no CPU spin), the timer IRQ
// wakes it, and the shell stays responsive the whole time. Proves sleep +
// scheduler wake + multi-process coexistence in one demo.

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    for (int i = 1; i <= 5; i++) {
        printf("[sleeper] tick %d\n", i);
        grv_sleep(500); // 500ms blocked
    }
    printf("[sleeper] done\n");
    return 0;
}
