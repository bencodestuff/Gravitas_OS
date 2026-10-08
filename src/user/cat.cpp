#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>

// cat — prints a file from the VFS, or copies stdin when given no argument.
// Usage: cat [file]   (no file = stdin, so it works in a pipe or with '<')
// Exercises the fd syscalls (open/read/close) from userland.

int main(int argc, char** argv) {
    int fd;
    if (argc >= 2) {
        fd = grv_open(argv[1], GRV_O_RDONLY);
        if (fd < 0) {
            printf("cat: no such file\n");
            return 1;
        }
    } else {
        fd = 0;   // stdin: a pipe, a redirected file, or the console
    }

    char buf[512];
    long n;
    while ((n = grv_read_fd(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) putchar(buf[i]);
    }
    if (argc >= 2) grv_close(fd);

    if (n < 0) {
        printf("\ncat: read error\n");
        return 1;
    }
    return 0;
}
