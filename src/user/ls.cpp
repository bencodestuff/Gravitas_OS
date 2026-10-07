#include <stdio.h>
#include <grv/syscall.h>
#include <sys/stat.h>

// ls [-l] [path] — lists a directory.
// A standalone userland program: spawned by the shell with argv, runs in ring
// 3, exits when done. Entry via crt0 -> main().

#define LS_MAX 128

// "-rwxr-xr-x" from a struct stat's st_mode.
static void mode_string(unsigned long m, char* out) {
    out[0] = S_ISDIR(m) ? 'd' : '-';
    const char rwx[3] = { 'r', 'w', 'x' };
    for (int i = 0; i < 9; i++)
        out[1 + i] = (m & (1u << (8 - i))) ? rwx[i % 3] : '-';
    out[10] = '\0';
}

int main(int argc, char** argv) {
    int long_form = 0;
    const char* path = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'l' && argv[i][2] == '\0') {
            long_form = 1;
        } else {
            path = argv[i];
        }
    }

    static char cwd[64];
    if (path == 0) {
        grv_getcwd(cwd, sizeof(cwd));
        path = cwd;
    }

    static struct grv_dirent ents[LS_MAX];
    long n = grv_readdir(path, ents, LS_MAX);
    if (n < 0) {
        printf("%s: not a directory\n", path);
        return 1;
    }

    printf("%s:\n", path);
    for (long i = 0; i < n; i++) {
        int isdir = ents[i].type == GRV_DT_DIR;
        if (!long_form) {
            printf("%s%s\n", ents[i].name, isdir ? "/" : "");
            continue;
        }

        char full[192];
        snprintf(full, sizeof(full), "%s/%s", path, ents[i].name);

        struct stat st;
        char mode[11] = "??????????";
        unsigned long ino = 0, size = 0;
        if (stat(full, &st) == 0) {
            mode_string((unsigned long)st.st_mode, mode);
            ino = (unsigned long)st.st_ino;
            size = (unsigned long)st.st_size;
        }
        printf("%s %4lu %8lu  %s%s\n", mode, ino, size, ents[i].name,
               isdir ? "/" : "");
    }
    return 0;
}
