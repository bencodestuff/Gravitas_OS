#include <stdio.h>
#include <grv/syscall.h>

// rawwin — a client with a window that never reads its mailbox, so it ignores
// the WM's close request. It exists to prove the WM force-kills a client that
// will not cooperate.

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    struct grv_surface_info si;
    int sid = grv_surface_create(360, 200, &si);
    if (sid < 0) { printf("rawwin: no surface\n"); return 1; }
    printf("rawwin: up as #%d\n", sid);

    for (;;) grv_sleep(1000); // alive, but deaf to IPC
}