#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <grv/syscall.h>

// ext2test — the ext2 image the kernel mounts at /disk (made on the host by
// tools/mkext2.py, handed over as a Limine module, or reached through a real
// disk driver). The first half checks that the reader gets the metadata, the
// directory walk and the indirect block path right. The second half exercises
// the writer: it creates, grows, truncates and removes its own files under
// names that begin with rwtest, and leaves every file the image shipped with
// alone so other tests can keep relying on them.

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("ext2test: FAIL: %s\n", what); fails++; }
}

static const char* const HELLO = "Hello from the ext2 disk\n";
#define BIG_SIZE 20000

// Byte i of big.bin: the pattern the host wrote, so every byte can be checked
// without the guest carrying a copy of the file.
static unsigned char big_byte(size_t i) { return (unsigned char)(33 + (i % 94)); }

// The pattern the writer tests use: printable and a function of the offset.
static unsigned char rw_byte(size_t i, unsigned char base) {
    return (unsigned char)(base + (i % 26));
}

static ssize_t read_all(int fd, void* buf, size_t want) {
    size_t done = 0;
    while (done < want) {
        ssize_t n = read(fd, (char*)buf + done, want - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

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

    // --- the writer ---

    // 7. Create, write, read, and an append that crosses a block boundary
    //    (1000 bytes then 512 more: the second write starts in block 0 and
    //    ends in block 1).
    static unsigned char wbuf[4096];
    static unsigned char rbuf[4096];

    fd = open("/disk/rwtest1.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(fd >= 0, "create rwtest1.txt");
    if (fd >= 0) {
        for (size_t i = 0; i < 1000; i++) wbuf[i] = rw_byte(i, 'a');
        check(write(fd, wbuf, 1000) == 1000, "write 1000 bytes");
        check(fstat(fd, &st) == 0 && st.st_size == 1000,
              "fstat reports the written size");
        check(lseek(fd, 0, SEEK_END) == 1000, "seek to the end");
        for (size_t i = 0; i < 512; i++) wbuf[i] = rw_byte(1000 + i, 'A');
        check(write(fd, wbuf, 512) == 512, "append across a block boundary");
        check(fstat(fd, &st) == 0 && st.st_size == 1512,
              "fstat reports the appended size");

        check(lseek(fd, 0, SEEK_SET) == 0, "seek back to the start");
        check(read_all(fd, rbuf, 1512) == 1512, "read the whole file back");
        int ok = 1;
        for (size_t i = 0; i < 1000; i++)
            if (rbuf[i] != rw_byte(i, 'a')) { ok = 0; break; }
        for (size_t i = 0; i < 512; i++)
            if (rbuf[1000 + i] != rw_byte(1000 + i, 'A')) { ok = 0; break; }
        check(ok, "every byte survived the append");

        // 8. Truncate down: the size shrinks and the kept bytes are untouched.
        check(grv_ftruncate(fd, 700) == 0, "truncate to 700");
        check(fstat(fd, &st) == 0 && st.st_size == 700,
              "fstat reports the truncated size");
        check(lseek(fd, 0, SEEK_SET) == 0 && read_all(fd, rbuf, 700) == 700,
              "read after truncate");
        ok = 1;
        for (size_t i = 0; i < 700; i++)
            if (rbuf[i] != rw_byte(i, 'a')) { ok = 0; break; }
        check(ok, "truncate kept the bytes before the new end");

        // 9. Truncate up: the tail is a hole, so it reads as zeroes.
        check(grv_ftruncate(fd, 3000) == 0, "grow to 3000");
        check(fstat(fd, &st) == 0 && st.st_size == 3000,
              "fstat reports the grown size");
        check(lseek(fd, 700, SEEK_SET) == 700 && read_all(fd, rbuf, 1024) == 1024,
              "read the start of the grown tail");
        ok = 1;
        for (size_t i = 0; i < 1024; i++)
            if (rbuf[i] != 0) { ok = 0; break; }
        check(ok, "the grown tail reads as zeroes");

        // A write into the middle of the hole is still a normal write.
        check(lseek(fd, 1000, SEEK_SET) == 1000, "seek into the hole");
        for (size_t i = 0; i < 100; i++) wbuf[i] = rw_byte(i, '0');
        check(write(fd, wbuf, 100) == 100, "write into the hole");
        check(lseek(fd, 1000, SEEK_SET) == 1000 && read_all(fd, rbuf, 100) == 100,
              "read back from the hole");
        ok = 1;
        for (size_t i = 0; i < 100; i++)
            if (rbuf[i] != rw_byte(i, '0')) { ok = 0; break; }
        check(ok, "the write into the hole landed");

        check(fsync(fd) == 0, "fsync returns 0");
        close(fd);
    }
    check(unlink("/disk/rwtest1.txt") == 0, "unlink rwtest1.txt");
    check(stat("/disk/rwtest1.txt", &st) == -1, "the file is gone");

    // 10. Directories: mkdir, a file inside, the listing, unlink, rmdir.
    check(mkdir("/disk/rwtestdir", 0755) == 0, "mkdir rwtestdir");
    fd = open("/disk/rwtestdir/inner.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check(fd >= 0, "create a file inside the new directory");
    if (fd >= 0) {
        check(write(fd, "inner\n", 6) == 6, "write the inner file");
        close(fd);
    }
    check(stat("/disk/rwtestdir/inner.txt", &st) == 0 && st.st_size == 6,
          "stat the inner file");

    d = opendir("/disk/rwtestdir");
    check(d != 0, "opendir the new directory");
    int inner = 0, dir_count = 0;
    while (d && (e = readdir(d)) != 0) {
        dir_count++;
        if (strcmp(e->d_name, "inner.txt") == 0 && e->d_type == DT_REG) inner = 1;
    }
    if (d) closedir(d);
    check(inner && dir_count == 1, "the listing has exactly the inner file");

    check(unlink("/disk/rwtestdir/inner.txt") == 0, "unlink the inner file");
    check(rmdir("/disk/rwtestdir") == 0, "rmdir the empty directory");
    check(stat("/disk/rwtestdir", &st) == -1, "the directory is gone");

    // 11. Rename: within a directory, replacing a file, and moving a directory
    //     (which has to rewrite its ".." and both parents' link counts).
    fd = open("/disk/rwtest2.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, "rename me", 9); close(fd); }
    check(rename("/disk/rwtest2.txt", "/disk/rwtest3.txt") == 0,
          "rename a file in place");
    check(stat("/disk/rwtest2.txt", &st) == -1, "the old name is gone");
    check(stat("/disk/rwtest3.txt", &st) == 0 && st.st_size == 9,
          "the new name has the file");

    fd = open("/disk/rwtest4.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, "target", 6); close(fd); }
    check(rename("/disk/rwtest3.txt", "/disk/rwtest4.txt") == 0,
          "rename over an existing file");
    check(stat("/disk/rwtest3.txt", &st) == -1, "the replaced name is gone");
    fd = open("/disk/rwtest4.txt", O_RDONLY);
    check(fd >= 0 && read(fd, buf, 9) == 9 && memcmp(buf, "rename me", 9) == 0,
          "the replacement holds the source file's bytes");
    if (fd >= 0) close(fd);

    check(mkdir("/disk/rwtestdir2", 0755) == 0, "mkdir rwtestdir2");
    check(rename("/disk/rwtest4.txt", "/disk/rwtestdir2/moved.txt") == 0,
          "move a file into another directory");
    check(stat("/disk/rwtest4.txt", &st) == -1, "the moved file left the root");
    check(stat("/disk/rwtestdir2/moved.txt", &st) == 0 && st.st_size == 9,
          "the moved file arrived");

    check(mkdir("/disk/rwtestdir3", 0755) == 0, "mkdir rwtestdir3");
    fd = open("/disk/rwtestdir3/inside.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, "inside", 6); close(fd); }
    check(rename("/disk/rwtestdir3", "/disk/rwtestdir2/child") == 0,
          "move a directory into another directory");
    check(stat("/disk/rwtestdir3", &st) == -1, "the old directory name is gone");
    check(stat("/disk/rwtestdir2/child", &st) == 0 && S_ISDIR(st.st_mode),
          "the moved directory is a directory");
    fd = open("/disk/rwtestdir2/child/inside.txt", O_RDONLY);
    check(fd >= 0 && read(fd, buf, 6) == 6 && memcmp(buf, "inside", 6) == 0,
          "a file inside the moved directory is still reachable");
    if (fd >= 0) close(fd);

    check(unlink("/disk/rwtestdir2/child/inside.txt") == 0, "unlink inside.txt");
    check(rmdir("/disk/rwtestdir2/child") == 0, "rmdir the moved directory");
    check(unlink("/disk/rwtestdir2/moved.txt") == 0, "unlink moved.txt");
    check(rmdir("/disk/rwtestdir2") == 0, "rmdir rwtestdir2");

    // 12. A file larger than the twelve direct blocks, so this one is written
    //     through the single indirect block.
    fd = open("/disk/rwtestbig.bin", O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(fd >= 0, "create rwtestbig.bin");
    if (fd >= 0) {
        size_t total = 24000;
        // Fill in chunks so the write path sees more than one call.
        size_t done = 0;
        int ok = 1;
        while (done < total) {
            size_t span = total - done;
            if (span > sizeof(wbuf)) span = sizeof(wbuf);
            for (size_t i = 0; i < span; i++)
                wbuf[i] = big_byte((done + i) % 251);
            if (write(fd, wbuf, span) != (ssize_t)span) { ok = 0; break; }
            done += span;
        }
        check(ok, "write a file through the indirect block");
        check(fstat(fd, &st) == 0 && st.st_size == (off_t)total,
              "fstat reports the indirect file's size");
        check(fsync(fd) == 0, "fsync the indirect file");
        check(lseek(fd, 0, SEEK_SET) == 0, "seek the indirect file to the start");
        ok = 1;
        size_t off = 0;
        ssize_t n;
        while ((n = read(fd, rbuf, sizeof(rbuf))) > 0) {
            for (ssize_t i = 0; i < n; i++)
                if (rbuf[i] != big_byte((off + (size_t)i) % 251)) { ok = 0; break; }
            off += (size_t)n;
            if (!ok) break;
        }
        check(ok && off == total, "read the indirect file byte for byte");
        close(fd);
    }
    check(unlink("/disk/rwtestbig.bin") == 0, "unlink rwtestbig.bin");

    // 13. Unlink while open: the name goes, the open file keeps its inode and
    //     its bytes, and that inode is not handed to a later create until the
    //     last descriptor closes.
    fd = open("/disk/rwtestopen.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(fd >= 0, "create rwtestopen.txt");
    if (fd >= 0) {
        check(write(fd, "orphan", 6) == 6, "write the soon-to-be-unlinked file");
        check(fstat(fd, &st) == 0, "fstat before unlink");
        unsigned long held = (unsigned long)st.st_ino;
        check(unlink("/disk/rwtestopen.txt") == 0, "unlink a file that is still open");
        check(stat("/disk/rwtestopen.txt", &st) == -1, "the unlinked name is gone");

        int fd2 = open("/disk/rwtestother.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
        check(fd2 >= 0 && fstat(fd2, &st) == 0 &&
              (unsigned long)st.st_ino != held,
              "the unlinked inode is not reused while open");
        if (fd2 >= 0) close(fd2);
        check(unlink("/disk/rwtestother.txt") == 0, "unlink rwtestother.txt");

        check(lseek(fd, 0, SEEK_SET) == 0 && read(fd, rbuf, 6) == 6 &&
              memcmp(rbuf, "orphan", 6) == 0,
              "the unlinked open file still reads back");
        check(write(fd, "!", 1) == 1, "the unlinked open file still writes");
        close(fd);
    }
    check(stat("/disk/rwtestopen.txt", &st) == -1, "the unlinked file is gone for good");

    // 14. A rename must not replace a directory that still has entries.
    check(mkdir("/disk/rwtestdir5", 0755) == 0, "mkdir rwtestdir5");
    check(mkdir("/disk/rwtestdir6", 0755) == 0, "mkdir rwtestdir6");
    fd = open("/disk/rwtestdir6/keep.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, "k", 1); close(fd); }
    check(rename("/disk/rwtestdir5", "/disk/rwtestdir6") == -1,
          "rename refuses to replace a non-empty directory");
    check(stat("/disk/rwtestdir5", &st) == 0 && S_ISDIR(st.st_mode) &&
          stat("/disk/rwtestdir6", &st) == 0 && S_ISDIR(st.st_mode),
          "both directories are still there");
    check(unlink("/disk/rwtestdir6/keep.txt") == 0, "unlink keep.txt");
    check(rmdir("/disk/rwtestdir5") == 0, "rmdir rwtestdir5");
    check(rmdir("/disk/rwtestdir6") == 0, "rmdir rwtestdir6");

    // A directory cannot be moved into its own subtree.
    check(mkdir("/disk/rwtestdir7", 0755) == 0, "mkdir rwtestdir7");
    check(mkdir("/disk/rwtestdir7/child", 0755) == 0, "mkdir rwtestdir7/child");
    check(rename("/disk/rwtestdir7", "/disk/rwtestdir7/child/x") == -1,
          "rename refuses to move a directory into its own subtree");
    check(rmdir("/disk/rwtestdir7/child") == 0, "rmdir rwtestdir7/child");
    check(rmdir("/disk/rwtestdir7") == 0, "rmdir rwtestdir7");

    // 15. sync flushes every mount; the ext2 one has a real device to flush.
    check(sync() == 0, "sync returns 0");

    // 16. Everything this test made is gone, and the image's own entries are
    //     still the only ones in the root.
    count = 0;
    saw_hello = saw_sub = saw_big = saw_lost = 0;
    d = opendir("/disk");
    while (d && (e = readdir(d)) != 0) {
        count++;
        if (strcmp(e->d_name, "hello.txt") == 0) saw_hello = 1;
        if (strcmp(e->d_name, "sub") == 0) saw_sub = 1;
        if (strcmp(e->d_name, "big.bin") == 0) saw_big = 1;
        if (strcmp(e->d_name, "lost+found") == 0) saw_lost = 1;
    }
    if (d) closedir(d);
    check(count == 4 && saw_hello && saw_sub && saw_big && saw_lost,
          "the writer cleaned up after itself");

    if (fails == 0) { printf("ext2test: all checks pass\n"); return 0; }
    printf("ext2test: %d check(s) failed\n", fails);
    return 1;
}
