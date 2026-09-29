#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>

// ls — lists the files in the tarfs.
// A standalone userland program: spawned by the shell with argv,
// runs in ring 3, exits when done. Entry via crt0 -> main().

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    char* listbuf = (char*)malloc(2048);
    if (listbuf == NULL) {
        printf("ls: out of memory\n");
        return 1;
    }

    grv_list_files(listbuf, 2048);
    printf("tarfs contents:\n%s\n", listbuf);
    free(listbuf);
    return 0;
}
