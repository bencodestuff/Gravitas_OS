#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <grv/syscall.h>

// mkdir [-p] dir...
//
// The kernel's mkdir makes exactly one directory and needs its parent to exist.
// That is the POSIX contract, and it is why -p lives here: "make the parents
// too" is a loop over the prefixes, not something the filesystem has to know
// about.

static int make_one(const char* path, int parents) {
    if (!parents) {
        if (mkdir(path, 0755) == 0) return 0;
        printf("mkdir: %s: %s\n", path, strerror(errno));
        return 1;
    }

    // Every prefix in turn. An existing one is fine; anything else is not.
    char buf[512];
    size_t n = 0;
    for (const char* p = path; ; p++) {
        if (*p == '/' || *p == '\0') {
            if (n > 0) {
                buf[n] = '\0';
                if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                    printf("mkdir: %s: %s\n", buf, strerror(errno));
                    return 1;
                }
            }
            if (*p == '\0') break;
        }
        if (n + 1 < sizeof(buf)) buf[n++] = *p;
    }
    return 0;
}

int main(int argc, char** argv) {
    int parents = 0, failed = 0, made = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) { parents = 1; continue; }
        if (make_one(argv[i], parents) == 0) made++;
        else failed = 1;
    }
    if (made + failed == 0) {
        printf("usage: mkdir [-p] dir...\n");
        return 1;
    }
    return failed;
}
