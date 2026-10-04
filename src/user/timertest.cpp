#include <stdio.h>
#include <grv/syscall.h>
#include <grv/input.h>

// timertest — arm a one-shot timer repeatedly and confirm it fires the kernel
// GRV_EV_TIMER event without any external input. Prints PASS/FAIL.

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    unsigned long t0 = grv_ticks();
    int count = 0;

    for (int i = 0; i < 10; i++) {
        grv_timer_set(50); // 50 ms
        struct grv_msg m;
        // Block until a message arrives; with no input or windows, the only
        // message can be our timer event.
        if (grv_recv(&m, 1) == 0 && m.type == GRV_EV_TIMER) count++;
    }

    unsigned long ms = (grv_ticks() - t0) * 20; // 50 Hz ticks -> ms

    if (count == 10 && ms >= 300 && ms <= 2000) {
        printf("timertest: PASS (%d events in %lums)\n", count, ms);
        return 0;
    }
    printf("timertest: FAIL (%d events in %lums)\n", count, ms);
    return 1;
}
