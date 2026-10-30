#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

// overlaytest — the writable root. Writes land in a ramfs upper layer over the
// read-only image, the listing is the union of both, and deleting an image file
// leaves a whiteout in the upper rather than modifying the image. A name that
// exists in either layer leaves a marker when it is deleted or moved, so the
// lower entry cannot reappear behind the upper's back.

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

    // 1b. Two descriptors on one overlay file agree about the live size. The
    //     node each one holds asks its upper layer rather than trusting the
    //     length copied at lookup, so an old reader sees the new bytes and an
    //     old appender lands at the real end.
    const char* live = "/overlay-live.txt";
    unlink(live);
    int lfd = open(live, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int lrd = open(live, O_RDONLY);
    int lap = open(live, O_WRONLY | O_APPEND);
    check(lfd >= 0 && lrd >= 0 && lap >= 0, "open one overlay file three times");
    if (lfd >= 0 && lrd >= 0 && lap >= 0) {
        check(write(lfd, "abcdef", 6) == 6, "write through the first descriptor");
        check(read(lrd, buf, sizeof(buf)) == 6 && memcmp(buf, "abcdef", 6) == 0,
              "a reader opened before the write sees the new bytes");
        check(fstat(lrd, &st) == 0 && st.st_size == 6,
              "fstat through the older descriptor");
        check(lseek(lrd, 0, SEEK_END) == 6, "SEEK_END through the older descriptor");
        check(write(lap, "Z", 1) == 1, "an append opened at 0 lands at the live end");
        check(stat(live, &st) == 0 && st.st_size == 7,
              "the overlay file grew to 7 bytes");
    }
    if (lap >= 0) close(lap);
    if (lrd >= 0) close(lrd);
    if (lfd >= 0) close(lfd);
    check(unlink(live) == 0, "clean up the live-size file");

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

    // 7. Deleting a file that was copied up must not let the image's original
    //    reappear: the upper entry goes and a marker stays. /hello.txt was
    //    copied up in step 3, so it exists in both layers now.
    check(stat("/hello.txt", &st) == 0, "the copied-up file is there");
    check(unlink("/hello.txt") == 0, "unlink a copied-up file");
    check(stat("/hello.txt", &st) != 0, "it is gone, not resurrected");
    fd = open("/hello.txt", O_RDONLY);
    check(fd < 0, "and it cannot be reopened");
    if (fd >= 0) close(fd);
    // Put the overwritten file back, so the rest of the suite sees the state it
    // expects (file_commands explains why it copies /disk/hello.txt instead).
    fd = open("/hello.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "recreate the copied-up file");
    if (fd >= 0) { write(fd, "replaced", 8); close(fd); }

    // 8. Renaming a file that exists only in the lower layer copies it up,
    //    moves the copy and hides the old name. The old path must be gone and
    //    the new one must carry the original bytes.
    char original[512];
    int have = 0;
    fd = open("/init.lua", O_RDONLY);
    check(fd >= 0, "open an image file to rename");
    if (fd >= 0) { have = (int)read(fd, original, sizeof(original)); close(fd); }
    check(have > 0, "read the image file");
    check(rename("/init.lua", "/init-renamed.lua") == 0, "rename a lower-only file");
    check(stat("/init.lua", &st) != 0, "the old name is gone");
    fd = open("/init-renamed.lua", O_RDONLY);
    check(fd >= 0, "the new name exists");
    if (fd >= 0) {
        int want = have < (int)sizeof(buf) ? have : (int)sizeof(buf);
        int got = (int)read(fd, buf, want);
        check(got == want && memcmp(buf, original, want) == 0,
              "the renamed file keeps its contents");
        close(fd);
    }
    check(unlink("/init-renamed.lua") == 0, "unlink the renamed file");

    // 9. The same rule when the source exists in both layers: write it (copy
    //    up), then rename. The old name must not fall back to the image copy.
    //    This leaves the old name hidden for the rest of the boot, so it uses
    //    the image's own /overlay-both.txt, which exists for this one test:
    //    shmtest maps /lua/hello.lua straight out of the image later, and other
    //    image files here are either already deleted or needed by a test.
    fd = open("/overlay-both.txt", O_WRONLY | O_TRUNC);
    check(fd >= 0, "open a lower file that will exist in both layers");
    if (fd >= 0) { write(fd, "both layers", 11); close(fd); }
    check(rename("/overlay-both.txt", "/overlay-both-moved.txt") == 0,
          "rename a file present in both layers");
    check(stat("/overlay-both.txt", &st) != 0,
          "the lower original stays hidden");
    fd = open("/overlay-both-moved.txt", O_RDONLY);
    check(fd >= 0 && read(fd, buf, 11) == 11 && memcmp(buf, "both layers", 11) == 0,
          "the moved file has the upper's contents");
    if (fd >= 0) close(fd);
    check(unlink("/overlay-both-moved.txt") == 0, "unlink the moved file");

    // 10. unlink doubles as rmdir, but a directory the image owns is never
    //     hidden: its subtree would become unreachable.
    check(unlink("/fonts") != 0, "unlink a non-empty lower directory is refused");
    check(stat("/fonts", &st) == 0 && S_ISDIR(st.st_mode), "the directory survives");
    check(stat("/fonts/bizcat.psf", &st) == 0, "and so does its subtree");

    // 11. A path longer than the overlay's old 256-byte field: it must be
    //     carried whole rather than truncated into a different name. The
    //     components stay short so this tests the path, not a backend's name.
    char deep[512];
    int lens[8];
    deep[0] = '\0';
    for (int level = 0; level < 6; level++) {
        char part[64];
        int n = snprintf(part, sizeof(part), "/long-overlay-path-level-%d-", level);
        while (n < 44) part[n++] = (char)('a' + level);
        part[n] = '\0';
        strcat(deep, part);
        lens[level] = (int)strlen(deep);
        check(mkdir(deep, 0755) == 0, "mkdir a long path component");
    }
    char leaf[512];
    snprintf(leaf, sizeof(leaf), "%s/leaf.txt", deep);
    check(strlen(leaf) > 255, "the test path is longer than the old cap");
    fd = open(leaf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create a file on a long path");
    if (fd >= 0) { check(write(fd, "long overlay path", 17) == 17, "write it"); close(fd); }
    fd = open(leaf, O_RDONLY);
    check(fd >= 0 && read(fd, buf, 17) == 17 &&
          memcmp(buf, "long overlay path", 17) == 0,
          "reopen the long path by its real name");
    if (fd >= 0) close(fd);
    check(stat(leaf, &st) == 0 && st.st_size == 17, "stat the long path");

    // 12. ".wh." marks a deletion, so a user name may not start with it: such
    //     a file could never be opened, and a rename onto one would hide data.
    errno = 0;
    fd = open("/.wh.user", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int create_errno = errno;
    check(fd < 0, "a .wh. file is refused");
    check(create_errno == EINVAL, "with EINVAL");
    if (fd >= 0) close(fd);
    check(mkdir("/.wh.userdir", 0755) != 0, "a .wh. directory is refused");
    check(rename(leaf, "/.wh.moved") != 0, "a .wh. rename destination is refused");
    check(stat(leaf, &st) == 0, "the refused rename left its source alone");
    check(unlink("/fonts/.wh.README-bizcat.txt") != 0,
          "a marker cannot be removed as a user file");
    check(stat("/fonts/README-bizcat.txt", &st) != 0,
          "and the marker still hides its lower entry");

    // 13. Clean up the long path, deepest component first.
    check(unlink(leaf) == 0, "unlink the long-path file");
    check(stat(leaf, &st) != 0, "the long-path file is gone");
    for (int level = 5; level >= 0; level--) {
        deep[lens[level]] = '\0';
        check(unlink(deep) == 0, "rmdir the long path component");
    }

    check(unlink(inner) == 0, "unlink inside the new dir");
    check(unlink(path) == 0, "unlink the root file");

    if (fails == 0) { printf("overlaytest: all checks pass\n"); return 0; }
    printf("overlaytest: %d check(s) failed\n", fails);
    return 1;
}
