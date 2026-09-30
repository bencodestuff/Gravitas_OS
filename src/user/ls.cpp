#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>

// ls — lists the files in the tarfs.
// A standalone userland program: spawned by the shell with argv,
// runs in ring 3, exits when done. Entry via crt0 -> main().

int main(int argc, char** argv) {
    (void)argc;

    // ls [path] — defaults to the process CWD
    const char* path;
    if (argc >= 2) {
        path = argv[1];
    } else {
        static char cwd[64];
        grv_getcwd(cwd, sizeof(cwd));
        path = cwd;
    }

    char* listbuf = (char*)malloc(2048);
    if (listbuf == NULL) {
        printf("ls: out of memory\n");
        return 1;
    }

    grv_readdir(path, listbuf, 2048);
    if (listbuf[0] == '\0') {
        printf("%s: not a directory\n", path);
        free(listbuf);
        return 1;
    }
    printf("%s:\n%s\n", path, listbuf);
    free(listbuf);
    return 0;
}
