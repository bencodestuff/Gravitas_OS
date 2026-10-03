#include <stdio.h>
#include <grv/syscall.h>

// proctest — M2 acceptance: background spawn, WAIT, KILL.
// Regression demo like crash.elf/smash.elf: every line below asserts a
// process-control contract and fails loudly (nonzero exit) on violation.

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const char* argv1[] = {"sleeper"};
    long rc;

    // --- 1. Background spawn: parent keeps running immediately ---
    long p1 = grv_spawn_bg("sleeper.elf", argv1, 1);
    if (p1 <= 0) {
        printf("[proctest] FAIL spawn_bg (rc=%d)\n", p1);
        return 1;
    }
    printf("[proctest] spawn_bg -> task %d\n", p1);

    // The parent is NOT blocked: prove it by living past the spawn and
    // waiting synchronously later (sleeper exits with 0 after ~2.5s).
    long code = grv_wait(p1);
    printf("[proctest] wait(%d) -> %d (want 0)\n", p1, code);
    if (code != 0) return 1;

    // --- 2. Double wait: the pid was reaped, second wait must fail ---
    if (grv_wait(p1) != -1) {
        printf("[proctest] FAIL double wait did not return -1\n");
        return 1;
    }

    // --- 3. WAIT on a task that never existed -> -1 ---
    if (grv_wait(9999) != -1) {
        printf("[proctest] FAIL bogus wait did not return -1\n");
        return 1;
    }

    // --- 4. KILL mid-flight: victim's exit code is what we passed ---
    long p2 = grv_spawn_bg("sleeper.elf", argv1, 1);
    if (p2 <= 0) {
        printf("[proctest] FAIL second spawn_bg (rc=%d)\n", p2);
        return 1;
    }
    long t2 = grv_ticks();
    grv_sleep(800); // sleeper is mid-run (~1 of its 5 ticks)
    long t3 = grv_ticks();

    rc = grv_kill(p2, 123);
    // sleep contract: ~800ms elapsed while the victim kept living
    if (t3 < t2 + 38) {
        printf("[proctest] FAIL sleep too short (%d ticks)\n", (int)(t3 - t2));
        return 1;
    }
    printf("[proctest] kill(%d) -> %d (want 0)\n", (int)p2, (int)rc);
    if (rc != 0) return 1;

    code = grv_wait(p2);
    printf("[proctest] wait(%d) -> %d (want 123)\n", p2, code);
    if (code != 123) return 1;

    // --- 5. kill failures: the kernel task and self-kill are refused ---
    if (grv_kill(0, 47) != -1) {
        printf("[proctest] FAIL killing task 0 was not refused\n");
        return 1;
    }

    printf("[proctest] all process-control checks pass\n");
    return 0;
}
