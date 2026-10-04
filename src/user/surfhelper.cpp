#include <stdio.h>
#include <grv/syscall.h>

// surfhelper — owns one surface and dies, so surfacetest can check two
// cross-task contracts: another task's surface cannot be destroyed or
// restacked, and death releases the surface automatically.
// Partner program: src/user/surfacetest.cpp (spawns us in the background,
// holds us alive until its foreign-access checks are done).

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    struct grv_surface_info s;
    int id = grv_surface_create(64, 64, &s);
    if (id < 0) {
        printf("[surfhelper] FAIL: create\n");
        return 1;
    }
    printf("[surfhelper] surface id %d at 0x%x\n", id, (unsigned)s.address);

    // Stay alive while the parent probes us, then exit WITHOUT destroying
    // the surface — the parent then checks it is gone.
    grv_sleep(800);
    printf("[surfhelper] exiting\n");
    return 0;
}
