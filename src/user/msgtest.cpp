#include <stdio.h>
#include <grv/syscall.h>

// msgtest — M3 acceptance: per-task mailboxes, send, blocking recv, FIFO
// order, mailbox-death semantics. Regression demo like crash.elf/rules:
// every check below fails loudly (nonzero exit) on violation.
// Partner program: src/user/msgecho.cpp (spawned in the background).

static void die(const char* what) {
    printf("[msgtest] FAIL: %s\n", what);
    grv_exit(1);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const char* earg[] = {"msgecho.elf"};

    // --- 1. background-spawn the echo server ---
    long P = grv_spawn_bg("msgecho.elf", earg, 1);
    if (P <= 0) die("spawn_bg msgecho");
    printf("[msgtest] echo server as task %d\n", (int)P);

    struct grv_msg m;

    // --- 2. our own mailbox starts empty; poll recv must say so ---
    if (grv_recv(&m, 0) != -1) die("empty poll recv did not return -1");

    // --- 3. refused destinations ---
    if (grv_send(0, &m) != -1)  die("send to kernel task 0 not refused");
    if (grv_send(9999, &m) != -1) die("send to dead task not refused");

    // --- 4. FIFO + reply round-trips (server answers, we block in recv) ---
    for (long i = 1; i <= 3; i++) {
        m.type = 42; m.a = i; m.b = 0;
        if (grv_send(P, &m) != 0) die("send to live task");
    }
    for (long i = 1; i <= 3; i++) {
        if (grv_recv(&m, 1) != 0) die("blocking recv of reply");
        printf("[msgtest] reply from %d: %d (want %d)\n", (int)m.from, (int)m.a, 100 + (int)i);
        if (m.from != (unsigned long)P) die("reply 'from' is not the server");
        if (m.type != 43) die("reply type wrong");
        if (m.a != 100u + (unsigned long)i) die("FIFO order broken");
    }

    // --- 4. kernel posts exist too (from == -1) — tested when input
    // routing lands; the type namespace is userland policy. ---

    // --- 5. poison: server exits cleanly on message alone ---
    m.type = 0; m.a = 0; m.b = 0;
    if (grv_send(P, &m) != 0) die("poison send");
    long code = grv_wait(P);
    printf("[msgtest] wait(%d) -> %d (want 0)\n", (int)P, (int)code);
    if (code != 0) die("server did not exit 0 on poison");

    // --- 7. dead server's mailbox is gone; send now fails ---
    m.type = 42; m.a = 1;
    if (grv_send(P, &m) != -1) die("send after death not refused");

    printf("[msgtest] all ipc checks pass\n");
    return 0;
}
