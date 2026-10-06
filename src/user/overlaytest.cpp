#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

// overlaytest — the writable root. Writes land in a ramfs upper layer over the
// read-only image, the listing is the union of both, and deleting an image file
// leaves a whiteout in the upper rather than modifying the image.

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("overlaytest: FAIL: %s\n", what); fails++; }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    char buf[64];
    struct stat st;

    // 1. Create a file in "/" — this used to be EROFS.
    const char* path = "/overlay-test.txt";
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create a file in /");
    if (fd < 0) { printf("overlaytest: %d check(s) failed\n", fails); return 1; }
    check(write(fd, "written to the root", 19) == 19, "write to the root");
    close(fd);

    fd = open(path, O_RDONLY);
    check(fd >= 0 && read(fd, buf, 19) == 19 &&
          memcmp(buf, "written to the root", 19) == 0, "read it back");
    if (fd >= 0) close(fd);
    check(stat(path, &st) == 0 && st.st_size == 19, "stat it");

    // 2. mkdir in "/", and use the new directory.
    unlink("/overlay-dir/inner.txt");
    unlink("/overlay-dir");
    check(mkdir("/overlay-dir", 0755) == 0, "mkdir /overlay-dir");
    check(stat("/overlay-dir", &st) == 0 && S_ISDIR(st.st_mode), "stat the new dir");
    const char* inner = "/overlay-dir/inner.txt";
    fd = open(inner, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create inside the new dir");
    if (fd >= 0) { write(fd, "x", 1); close(fd); }

    // 3. Overwrite an image file: the bytes go to the upper, so the new content
    //    reads back while the image itself is untouched.
    fd = open("/hello.txt", O_WRONLY | O_TRUNC);
    check(fd >= 0, "open an image file for writing");
    if (fd >= 0) { write(fd, "replaced", 8); close(fd); }
    fd = open("/hello.txt", O_RDONLY);
    check(fd >= 0 && read(fd, buf, 8) == 8 && memcmp(buf, "replaced", 8) == 0,
          "the overwrite reads back");
    if (fd >= 0) close(fd);

    // 4. The listing is the union, with whiteout markers hidden.
    DIR* d = opendir("/");
    check(d != 0, "opendir /");
    int saw_bin = 0, saw_ov = 0, saw_wh = 0;
    struct dirent* e;
    while (d && (e = readdir(d)) != 0) {
        if (strcmp(e->d_name, "bin") == 0) saw_bin = 1;
        if (strcmp(e->d_name, "overlay-test.txt") == 0) saw_ov = 1;
        if (strncmp(e->d_name, ".wh.", 4) == 0) saw_wh = 1;
    }
    if (d) closedir(d);
    check(saw_bin, "the union lists the image's entries");
    check(saw_ov, "the union lists the upper's entries");
    check(!saw_wh, "whiteout markers are hidden");

    // 5. Deleting an image file: gone from stat and the listing, while its
    //    neighbour is untouched. (The font loader already ran, so this is safe.)
    check(unlink("/fonts/README-bizcat.txt") == 0, "unlink an image file");
    check(stat("/fonts/README-bizcat.txt", &st) != 0, "it is gone");
    check(stat("/fonts/bizcat.psf", &st) == 0, "its neighbour is still there");
    d = opendir("/fonts");
    int still = 0;
    while (d && (e = readdir(d)) != 0)
        if (strcmp(e->d_name, "README-bizcat.txt") == 0) still = 1;
    if (d) closedir(d);
    check(!still, "the listing no longer shows it");

    // 6. A file created inside an image directory merges with it rather than
    //    shadowing it: the image's own entries are still there.
    fd = open("/fonts/added.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create a file inside an image directory");
    if (fd >= 0) { write(fd, "y", 1); close(fd); }
    check(stat("/fonts/bizcat.psf", &st) == 0, "the image's files survive the merge");
    check(stat("/fonts/added.txt", &st) == 0 && st.st_size == 1, "the new file is there");
    check(unlink("/fonts/added.txt") == 0, "unlink it again");

    check(unlink(inner) == 0, "unlink inside the new dir");
    check(unlink(path) == 0, "unlink the root file");

    if (fails == 0) { printf("overlaytest: all checks pass\n"); return 0; }
    printf("overlaytest: %d check(s) failed\n", fails);
    return 1;
}
