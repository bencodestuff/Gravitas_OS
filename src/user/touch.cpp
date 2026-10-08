#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <grv/syscall.h>

// touch — create the file if it is not there, and otherwise leave it alone.
//
// There is no utime in this system, so "update the timestamps" is not on offer
// and this does not pretend to: what it is good for is making an empty file.
// That is why there is no O_TRUNC — an existing file keeps its contents.

int main(int argc, char** argv) {
    int failed = 0;
    if (argc < 2) {
        printf("usage: touch file...\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            printf("touch: %s: %s\n", argv[i], strerror(errno));
            failed = 1;
            continue;
        }
        close(fd);
    }
    return failed;
}
