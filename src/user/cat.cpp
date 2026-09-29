#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>

// cat — prints a file from the tarfs.
// Usage: cat <file>   (argv[1] = file name)

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("cat: missing file name\n");
        return 1;
    }

    char* filebuf = (char*)malloc(4096);
    if (filebuf == NULL) {
        printf("cat: out of memory\n");
        return 1;
    }

    long n = grv_read_file(argv[1], filebuf, 4096);
    if (n == -1) {
        printf("cat: no such file\n");
        free(filebuf);
        return 1;
    }

    // fwrite the bytes: file data isn't NUL-terminated, so print per-byte
    for (long i = 0; i < n; i++) putchar(filebuf[i]);
    putchar('\n');

    free(filebuf);
    return 0;
}
