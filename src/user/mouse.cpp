#include <stdio.h>
#include <grv/syscall.h>

// mouse — polls the PS/2 mouse and prints movement deltas + buttons.
// Exit condition: left-click 3 times (so it terminates cleanly back to
// the shell instead of looping forever).

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("[mouse] move the mouse; left-click 3x to exit\n");

    struct grv_mouse_packet pkt;
    int left_clicks = 0;
    bool prev_left = false;
    int prints = 0;

    while (left_clicks < 3) {
        if (grv_mouse_read(&pkt)) {
            // Only print on actual movement or button change (the mouse
            // streams ~40 packets/sec even when idle-ish)
            if (pkt.dx != 0 || pkt.dy != 0 || pkt.buttons != 0) {
                printf("[mouse] dx=%d dy=%d buttons=%x\n",
                       pkt.dx, pkt.dy, pkt.buttons);
                if (++prints >= 30) {
                    printf("[mouse] (output limit reached)\n");
                    break;
                }
            }

            bool left = (pkt.buttons & 1) != 0;
            if (left && !prev_left) left_clicks++; // rising edge
            prev_left = left;
        } else {
            grv_sleep(20); // one tick — nothing to report
        }
    }

    printf("[mouse] done\n");
    return 0;
}
