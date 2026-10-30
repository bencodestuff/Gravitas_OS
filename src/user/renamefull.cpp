#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <grv/syscall.h>

// renamefull: a rename over an existing name that cannot finish must leave the
// name it was replacing where it was.
//
// The interesting failure is the disk running out at the one moment the old
// name has already been taken out of the directory and the new one is not in
// yet. ext2 spends its last block on the *insertion*, which the caller is told
// about (ENOSPC), so the removal has to be undone. This program reaches that
// state on purpose: fill the filesystem, pack one directory's block with short
// names until the next create has nowhere to put an entry, then rename one of
// those names over another. There is no block for the new entry, the rename
// fails, and both names have to still be there with their bytes.
//
// It cleans up the filling file before returning, so the image is left with
// room and the group can check it with e2fsck afterwards.

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("renamefull: FAIL: %s\n", what); fails++; }
}

#define PACKED "/disk/fullpacked"
#define SOURCE PACKED "/a00"
#define DEST   PACKED "/b01"

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    check(mkdir(PACKED, 0755) == 0, "mkdir the directory to pack");

    // The destination holds bytes, so losing the entry loses data and not just
    // a name.
    int fd = open(DEST, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check(fd >= 0, "create the destination");
    if (fd >= 0) {
        check(write(fd, "keep", 4) == 4, "write the destination");
        close(fd);
    }

    // Take every free block. One file is enough: writing until the filesystem
    // reports ENOSPC is also the proof that no block is left for the directory
    // below to grow into.
    fd = open("/disk/fullfill.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check(fd >= 0, "create the filling file");
    struct grv_statfs fs;
    if (grv_statfs("/disk", &fs) == 0)
        printf("renamefull: %lu bytes free before the fill\n", fs.free);
    int full = 0;
    int writes = 0;
    static char chunk[4096];
    memset(chunk, 'F', sizeof(chunk));
    for (int guard = 0; fd >= 0 && guard < 32768; guard++) {
        ssize_t n = write(fd, chunk, sizeof(chunk));
        writes++;
        if ((writes % 1024) == 0)
            printf("renamefull: %d KB written\n", writes * 4);
        if (n < 0) { full = (errno == ENOSPC); break; }
        if (n == 0) break;
    }
    check(full, "writing until the disk is full reports ENOSPC");
    if (fd >= 0) close(fd);
    if (grv_statfs("/disk", &fs) == 0)
        printf("renamefull: %lu bytes free after the fill\n", fs.free);
    printf("renamefull: disk full after %d writes\n", writes);

    // Pack the directory's one block. The create that finds no room for its
    // entry asks for a second block, fails with ENOSPC, and leaves the first
    // exactly full: every record is used and the tail is too short for another
    // header. The loop stops on that failure.
    int made = 0;
    for (int i = 0; i < 200; i++) {
        char name[16];
        int n = 0;
        name[n++] = 'a';
        name[n++] = (char)('0' + (i / 10) % 10);
        name[n++] = (char)('0' + i % 10);
        name[n] = '\0';
        char path[64];
        size_t plen = strlen(PACKED);
        memcpy(path, PACKED, plen);
        path[plen] = '/';
        memcpy(path + plen + 1, name, (size_t)n + 1);

        int f = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (f < 0) break;
        close(f);
        made++;
    }
    check(made > 1, "the directory took entries before it filled");
    printf("renamefull: packed %d entries into one directory block\n", made);

    // The rename the disk cannot finish: the destination entry is removed, its
    // replacement cannot be inserted, and the failure has to put the old name
    // back rather than leave the destination deleted.
    errno = 0;
    int r = rename(SOURCE, DEST);
    check(r == -1 && errno == ENOSPC,
          "a rename with no block for the new entry -> ENOSPC");

    struct stat st;
    check(stat(DEST, &st) == 0 && S_ISREG(st.st_mode),
          "the replaced name is still there after the failed rename");
    check(stat(SOURCE, &st) == 0,
          "the source is still in its old place");
    char back[8];
    fd = open(DEST, O_RDONLY);
    check(fd >= 0 && read(fd, back, 4) == 4 && memcmp(back, "keep", 4) == 0,
          "the replaced file still holds its bytes");
    if (fd >= 0) close(fd);

    // Give the blocks back so the image is not left full for the reboot and the
    // e2fsck run that follow.
    check(unlink("/disk/fullfill.bin") == 0, "unlink the filling file");
    check(stat("/disk/fullfill.bin", &st) == -1, "the filling file is gone");

    if (fails == 0) { printf("renamefull: all checks pass\n"); return 0; }
    printf("renamefull: %d check(s) failed\n", fails);
    return 1;
}
