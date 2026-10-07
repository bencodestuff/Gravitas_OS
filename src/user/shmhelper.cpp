#include <stdio.h>
#include <grv/syscall.h>

// shmhelper — shmtest's partner. It waits for the go message (which carries the
// region's id), attaches it, and proves the two tasks see the same pages.
//
// argv[1] is "ok" (we were granted it, so attaching must work) or "deny" (we
// were not, so it must be refused).

#define MSG_GO     0x7001
#define MSG_RESULT 0x7002

#define OK_READ    0x1
#define OK_DENIED  0x2
#define OK_FAILED  0x0

static void reply(long to, unsigned long what) {
    struct grv_msg out;
    out.from = 0;
    out.type = MSG_RESULT;
    out.a = what;
    out.b = 0;
    grv_send(to, &out);
}

int main(int argc, char** argv) {
    int expect_granted = !(argc > 1 && argv[1] && argv[1][0] == 'd');

    struct grv_msg go;
    if (grv_recv(&go, 1) != 0 || go.type != MSG_GO) {
        printf("shmhelper: no go message\n");
        return 1;
    }

    struct grv_map a;
    int r = grv_shm_map((int)go.a, &a);

    if (!expect_granted) {
        printf("shmhelper: attach refused as expected\n");
        reply((long)go.from, r != 0 ? OK_DENIED : OK_FAILED);
        return r != 0 ? 0 : 1;
    }

    if (r != 0 || a.size == 0) {
        printf("shmhelper: attach failed\n");
        reply((long)go.from, OK_FAILED);
        return 1;
    }

    // Check what the parent left, then leave our own mark on every word.
    volatile unsigned int* p = (volatile unsigned int*)a.address;
    int ok = (p[0] == 0x11110000u && p[63] == 0x1111003Fu);
    for (int i = 0; i < 64; i++) p[i] = p[i] + 1;
    printf("shmhelper: pattern %s\n", ok ? "matches" : "MISMATCH");

    reply((long)go.from, ok ? OK_READ : OK_FAILED);
    return ok ? 0 : 1;
}
