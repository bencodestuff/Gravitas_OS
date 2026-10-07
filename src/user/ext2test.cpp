#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

// ext2test — the read-only ext2 image the kernel mounts at /disk (made on the
// host by tools/mke2fs.py, handed over as a Limine module, copied by a RAM
// disk). It checks the metadata, the directory walk, a small file, and a file
// larger than the twelve direct block pointers — reaching the indirect path is
// the part of an ext2 reader most likely to be quietly wrong.

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("ext2test: FAIL: %s\n", what); fails++; }
}

static const char* const HELLO = "Hello from the ext2 disk\n";
#define BIG_SIZE 20000

// Byte i of big.bin: the pattern the host wrote, so every byte can be checked
// without the guest carrying a copy of the file.
static unsigned char big_byte(size_t i) { return (unsigned char)(33 + (i % 94)); }

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    struct stat st;
    char buf[512];

    // 1. The mount point is a directory with a real inode.
    check(stat("/disk", &st) == 0 && S_ISDIR(st.st_mode), "/disk is a directory");
    check(stat("/disk", &st) == 0 && st.st_ino != 0, "/disk has an inode number");

    // 2. Metadata, straight out of the inode.
    check(stat("/disk/hello.txt", &st) == 0 && S_ISREG(st.st_mode),
          "hello.txt is a regular file");
    check(st.st_size == (off_t)strlen(HELLO), "hello.txt has the size the inode says");
    check((st.st_mode & 0777) == 0644, "hello.txt has its permission bits");
    check(st.st_mtime != 0, "hello.txt has an mtime");

    // 3. Reading it, from the start and from the middle.
    int fd = open("/disk/hello.txt", O_RDONLY);
    check(fd >= 0, "open hello.txt");
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf));
        check(n == (ssize_t)strlen(HELLO) && memcmp(buf, HELLO, (size_t)n) == 0,
              "read hello.txt");
        check(lseek(fd, 6, SEEK_SET) == 6, "seek into hello.txt");
        n = read(fd, buf, 4);
        check(n == 4 && memcmp(buf, "from", 4) == 0, "read from the middle");
        close(fd);
    }

    // 4. The listing. The image has big.bin, hello.txt, sub and the lost+found
    //    mke2fs always makes (a twelve-block directory, so this walks past the
    //    first block of a directory too).
    DIR* d = opendir("/disk");
    check(d != 0, "opendir /disk");
    int saw_hello = 0, saw_sub = 0, saw_big = 0, saw_lost = 0, count = 0;
    struct dirent* e;
    while (d && (e = readdir(d)) != 0) {
        count++;
        if (strcmp(e->d_name, "hello.txt") == 0 && e->d_type == DT_REG) saw_hello = 1;
        if (strcmp(e->d_name, "sub") == 0 && e->d_type == DT_DIR) saw_sub = 1;
        if (strcmp(e->d_name, "big.bin") == 0 && e->d_type == DT_REG) saw_big = 1;
        if (strcmp(e->d_name, "lost+found") == 0 && e->d_type == DT_DIR) saw_lost = 1;
    }
    if (d) closedir(d);
    check(saw_hello && saw_sub && saw_big, "the listing has every entry");
    check(saw_lost, "the listing types lost+found as a directory");
    check(count == 4, "the listing has exactly the four entries");

    // 5. A file reached through a directory.
    fd = open("/disk/sub/nested.txt", O_RDONLY);
    check(fd >= 0, "open sub/nested.txt");
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf));
        check(n == 15 && memcmp(buf, "A nested file.\n", 15) == 0,
              "read sub/nested.txt");
        close(fd);
    }

    // 6. big.bin is 20000 bytes, so it needs the single indirect block. Read it
    //    in chunks and check every byte.
    check(stat("/disk/big.bin", &st) == 0 && st.st_size == BIG_SIZE,
          "big.bin has the size the inode says");
    fd = open("/disk/big.bin", O_RDONLY);
    check(fd >= 0, "open big.bin");
    if (fd >= 0) {
        size_t off = 0;
        int ok = 1;
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if ((unsigned char)buf[i] != big_byte(off + (size_t)i)) {
                    ok = 0;
                    break;
                }
            }
            off += (size_t)n;
            if (!ok || off > BIG_SIZE) break;
        }
        check(ok && off == (size_t)BIG_SIZE, "read all of big.bin byte for byte");

        // The last byte is also reachable by seeking straight to it.
        check(lseek(fd, BIG_SIZE - 1, SEEK_SET) == BIG_SIZE - 1 &&
              read(fd, buf, 1) == 1 &&
              (unsigned char)buf[0] == big_byte(BIG_SIZE - 1),
              "seek to the last byte of big.bin");
        close(fd);
    }

    // 7. A read-only mount refuses everything that would write.
    errno = 0;
    check(open("/disk/hello.txt", O_WRONLY) == -1 && errno == EROFS,
          "a write open fails with EROFS");
    check(unlink("/disk/hello.txt") == -1, "unlink fails");
    check(mkdir("/disk/newdir", 0755) == -1, "mkdir fails");

    if (fails == 0) { printf("ext2test: all checks pass\n"); return 0; }
    printf("ext2test: %d check(s) failed\n", fails);
    return 1;
}
