#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <grv/syscall.h>

// cp from to — one file at a time, in chunks, so nothing has to fit in memory.
//
// The destination is opened truncating, which is the hazard: `cp a a` would
// empty the source before reading it. Refusing the identical argument catches
// the case people actually type; two different names for one file would need an
// inode identity check (fstat now offers the pieces), which cp does not do yet.

int main(int argc, char** argv) {
    if (argc != 3) {
        printf("usage: cp <from> <to>\n");
        return 1;
    }
    const char* from = argv[1];
    const char* to = argv[2];

    if (strcmp(from, to) == 0) {
        printf("cp: %s and %s are the same file\n", from, to);
        return 1;
    }

    int in = open(from, O_RDONLY);
    if (in < 0) {
        printf("cp: %s: %s\n", from, strerror(errno));
        return 1;
    }

    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        printf("cp: %s: %s\n", to, strerror(errno));
        close(in);
        return 1;
    }

    char buf[1024];
    long n;
    int failed = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            long w = write(out, buf + off, (unsigned long)(n - off));
            if (w <= 0) {
                printf("cp: %s: write failed\n", to);
                failed = 1;
                break;
            }
            off += w;
        }
        if (failed) break;
    }
    if (n < 0 && !failed) {
        printf("cp: %s: read failed\n", from);
        failed = 1;
    }

    close(in);
    close(out);
    return failed;
}
