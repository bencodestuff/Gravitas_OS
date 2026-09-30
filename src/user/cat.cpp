#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>

// cat — prints a file from the VFS.
// Usage: cat <file>   (argv[1] = file name)
// Exercises the fd syscalls (open/read/close) from userland.

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("cat: missing file name\n");
        return 1;
    }

    int fd = grv_open(argv[1], GRV_O_RDONLY);
    if (fd < 0) {
        printf("cat: no such file\n");
        return 1;
    }

    char buf[512];
    long n;
    while ((n = grv_read_fd(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) putchar(buf[i]);
    }
    grv_close(fd);

    if (n < 0) {
        printf("\ncat: read error\n");
        return 1;
    }
    return 0;
}
