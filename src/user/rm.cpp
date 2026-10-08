#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <grv/syscall.h>

// rm [-r] path...
//
// A directory only goes if it is empty, and the kernel's unlink enforces that.
// So "recursive" is a walk in here rather than a mode in there: deciding what a
// tree means, and in what order it comes apart, is policy.

static int remove_path(const char* path, int recursive, int* failed);

static int remove_tree(const char* path, int* failed) {
    DIR* d = opendir(path);
    if (d == 0) {
        printf("rm: %s: %s\n", path, strerror(errno));
        *failed = 1;
        return -1;
    }

    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        remove_path(child, 1, failed);
    }
    closedir(d);

    if (unlink(path) != 0) {
        printf("rm: %s: %s\n", path, strerror(errno));
        *failed = 1;
        return -1;
    }
    return 0;
}

static int remove_path(const char* path, int recursive, int* failed) {
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("rm: %s: %s\n", path, strerror(errno));
        *failed = 1;
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            printf("rm: %s: is a directory (use -r)\n", path);
            *failed = 1;
            return -1;
        }
        return remove_tree(path, failed);
    }

    if (unlink(path) != 0) {
        printf("rm: %s: %s\n", path, strerror(errno));
        *failed = 1;
        return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int recursive = 0, failed = 0, done = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-rf") == 0) {
            recursive = 1;
            continue;
        }
        remove_path(argv[i], recursive, &failed);
        done++;
    }
    if (done == 0) {
        printf("usage: rm [-r] path...\n");
        return 1;
    }
    return failed;
}
