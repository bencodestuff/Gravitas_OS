#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

// pathtest — the path and name limits.
//
// A component or a path that does not fit must be *refused*. It must never be
// silently cut down, because a shortened path names a different file: a path
// three components too deep used to resolve to its own prefix, and a name longer
// than the cap used to match whatever its first 127 bytes spelled. Both were
// real, and neither is visible from userland except as "the wrong file opened".

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("pathtest: FAIL: %s\n", what); fails++; }
}

// The kernel tracks at most 32 components in a canonical path. A file can sit at
// the limit (1 + 30 directories + the file); one component past it is refused.
#define DIRS 30

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // --- 1. the depth limit ------------------------------------------------
    char dir[256];
    int n = 0;
    const char* seed = "/tmp";
    for (const char* p = seed; *p; p++) dir[n++] = *p;
    dir[n] = '\0';

    int built = 1;
    for (int i = 0; i < DIRS; i++) {
        dir[n++] = '/';
        dir[n++] = 'd';
        dir[n] = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) { built = 0; break; }
    }
    check(built, "a directory chain up to the depth limit");

    char at_limit[256];
    int m = 0;
    for (int i = 0; i < n; i++) at_limit[m++] = dir[i];
    const char* tail = "/x.txt";
    for (const char* p = tail; *p; p++) at_limit[m++] = *p;
    at_limit[m] = '\0';

    int fd = open(at_limit, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "a file at the depth limit can be created");
    if (fd >= 0) { write(fd, "shallow", 7); close(fd); }
    fd = open(at_limit, O_RDONLY);
    check(fd >= 0, "and opened again");
    if (fd >= 0) close(fd);

    // The same path with three more components. Truncating back to the limit
    // would land on the file just created, which is exactly the bug.
    char too_deep[256];
    int d = 0;
    for (int i = 0; i < n; i++) too_deep[d++] = dir[i];
    for (int i = 0; i < 3; i++) { too_deep[d++] = '/'; too_deep[d++] = 'd'; }
    for (const char* p = tail; *p; p++) too_deep[d++] = *p;
    too_deep[d] = '\0';

    errno = 0;
    fd = open(too_deep, O_RDONLY);
    check(fd < 0, "a path past the depth limit is refused");
    check(errno == ENAMETOOLONG, "and it says ENAMETOOLONG");
    if (fd >= 0) close(fd);

    // --- 2. a long name that fits -----------------------------------------
    // The ext2 image carries a name longer than 128 bytes. Find it in the
    // listing rather than hardcoding it, and check it opens and reads back.
    DIR* sub = opendir("/disk/sub");
    check(sub != 0, "opendir /disk/sub");

    static char longname[300];
    size_t longest = 0;
    struct dirent* e;
    while (sub && (e = readdir(sub)) != 0) {
        size_t len = strlen(e->d_name);
        if (len > longest) {
            longest = len;
            memcpy(longname, e->d_name, len + 1);
        }
    }
    if (sub) closedir(sub);

    check(longest > 128, "the image has a name longer than 128 bytes");
    check(longest < 256, "and it fits within NAME_MAX");

    char full[512];
    snprintf(full, sizeof(full), "/disk/sub/%s", longname);

    struct stat st;
    check(stat(full, &st) == 0 && (size_t)st.st_size == longest,
          "the long name stats, with the size the inode says");

    fd = open(full, O_RDONLY);
    check(fd >= 0, "the long name opens");
    if (fd >= 0) {
        static char buf[400];
        ssize_t got = read(fd, buf, sizeof(buf) - 1);
        buf[got > 0 ? got : 0] = '\0';
        // The file's contents are its own name, so this checks the whole name
        // travelled and not a prefix of it.
        check(got == (ssize_t)longest && strcmp(buf, longname) == 0,
              "and reads back byte for byte");
        close(fd);
    }

    // --- 3. a name past NAME_MAX ------------------------------------------
    char toolong[512];
    int t = 0;
    const char* pfx = "/disk/sub/";
    for (const char* p = pfx; *p; p++) toolong[t++] = *p;
    for (int i = 0; i < 300; i++) toolong[t++] = 'y';
    toolong[t] = '\0';

    errno = 0;
    fd = open(toolong, O_RDONLY);
    check(fd < 0, "a name past NAME_MAX is refused");
    check(errno == ENAMETOOLONG, "and it says ENAMETOOLONG");
    if (fd >= 0) close(fd);

    errno = 0;
    check(stat(toolong, &st) != 0 && errno == ENAMETOOLONG,
          "stat agrees");

    if (fails == 0) { printf("pathtest: all checks pass\n"); return 0; }
    printf("pathtest: %d check(s) failed\n", fails);
    return 1;
}
