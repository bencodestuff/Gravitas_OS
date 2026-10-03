#include <stdio.h>
#include <grv/syscall.h>

// msgecho — IPC test server (M3 regression pair with msgtest).
// Quiet by design: the CLIENT prints the verdicts.
//   * receives (type==42, a=i)  -> replies (type=43, a=100+i, b=sender pid)
//   * receives type 0 (poison)  -> exits 0
//   * anything else             -> exits 2 (protocol violation)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    while (1) {
        struct grv_msg m;
        // Blocks until a message exists — exercises the wake path.
        if (grv_recv(&m, 1) != 0) break;

        if (m.type == 0) { // poison: client asks us to die cleanly
            grv_exit(0);
        }
        if (m.type == 42) {
            struct grv_msg r;
            r.type = 43;
            r.a = 100 + m.a;
            r.b = m.from; // tell the client who answered
            long rc = grv_send(m.from, &r);
            if (rc != 0) return 2;
            continue;
        }
        return 2; // unknown type: fastest way to fail the client
    }
    return 3; // recv failed: also fails the client, with a distinct code
}
