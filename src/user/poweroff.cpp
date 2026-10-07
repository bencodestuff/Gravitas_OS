#include <stdio.h>
#include <grv/syscall.h>

// poweroff — leave, either way. `poweroff -r` (or `reboot`) resets instead of
// switching off.
//
// Anything that needs flushing first belongs here, before the syscall: by the
// time the machine answers it is already going down.

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char** argv) {
    int reset = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && (streq(argv[i], "-r") || streq(argv[i], "reboot")))
            reset = 1;
    }

    printf(reset ? "poweroff: resetting\n" : "poweroff: shutting down\n");

    if (reset) grv_reboot();
    grv_poweroff();
    return 0; // not reached
}
