#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <grv/syscall.h>

// mv from to — a rename, which is what a move within one filesystem is.
//
// A cross-device move would need a copy, and the other mount here is read-only,
// so this reports the failure rather than half-doing it. (The kernel's rename
// refuses when the two sides are on different filesystems, which is the same
// answer arrived at honestly.)

int main(int argc, char** argv) {
    if (argc != 3) {
        printf("usage: mv <from> <to>\n");
        return 1;
    }

    if (grv_rename(argv[1], argv[2]) != 0) {
        printf("mv: %s -> %s: %s\n", argv[1], argv[2], strerror(errno));
        return 1;
    }
    return 0;
}
