#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <grv/syscall.h>

// filetest — exercises seek/stat/truncate/create/remove/rename on the writable
// filesystem, the bare-name creation rule, and unlink-while-open. Prints
// PASS/FAIL for the test harness.

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("filetest: FAIL: %s\n", what); fails++; }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    const char* dir  = "/tmp/ftdir";
    const char* file = "/tmp/ftdir/f.txt";

    grv_unlink(file); // clean slate if a previous run left it (may fail)
    grv_mkdir(dir);

    struct grv_stat st;
    check(grv_stat(dir, &st) == 0 && st.is_dir, "mkdir + stat dir");

    int fd = grv_open(file, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    check(fd >= 0, "open(O_CREATE|O_TRUNC)");
    check(grv_write_fd(fd, "hello world", 11) == 11, "write");
    grv_close(fd);

    check(grv_stat(file, &st) == 0 && st.size == 11 && !st.is_dir, "stat size");

    char buf[64];
    fd = grv_open(file, GRV_O_RDONLY);
    check(fd >= 0, "open read");
    long n = grv_read_fd(fd, buf, sizeof(buf));
    check(n == 11 && memcmp(buf, "hello world", 11) == 0, "read back");

    check(grv_lseek(fd, 6, 0) == 6, "lseek SET");
    n = grv_read_fd(fd, buf, 5);
    check(n == 5 && memcmp(buf, "world", 5) == 0, "read after SET");

    check(grv_lseek(fd, -5, 2) == 6, "lseek END-5");
    n = grv_read_fd(fd, buf, sizeof(buf));
    check(n == 5 && memcmp(buf, "world", 5) == 0, "read after END");

    check(grv_ftruncate(fd, 5) == 0, "ftruncate to 5");
    grv_close(fd);
    check(grv_stat(file, &st) == 0 && st.size == 5, "size after truncate");

    fd = grv_open(file, GRV_O_RDONLY);
    n = grv_read_fd(fd, buf, sizeof(buf));
    check(n == 5 && memcmp(buf, "hello", 5) == 0, "content after truncate");
    grv_close(fd);

    fd = grv_open(file, GRV_O_WRONLY | GRV_O_TRUNC); // existing file: empties
    check(fd >= 0, "reopen O_TRUNC");
    grv_close(fd);
    check(grv_stat(file, &st) == 0 && st.size == 0, "O_TRUNC emptied");

    // --- O_APPEND: every write lands at the end ---
    // The classic use is a log: a writer seeks to the end at open and writes
    // there. O_APPEND does it per write, so a seek in between cannot make a
    // later write land in the middle of the file.
    fd = grv_open(file, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    check(fd >= 0, "open for append");
    grv_write_fd(fd, "one", 3);
    grv_close(fd);

    fd = grv_open(file, GRV_O_WRONLY | GRV_O_APPEND);
    check(fd >= 0, "open O_APPEND");
    check(grv_write_fd(fd, "two", 3) == 3, "append write");
    grv_lseek(fd, 0, 0);                       // a seek must not defeat the append
    check(grv_write_fd(fd, "three", 5) == 5, "append write after a seek");
    grv_close(fd);

    check(grv_stat(file, &st) == 0 && st.size == 11, "append grew the file");
    fd = grv_open(file, GRV_O_RDONLY);
    n = grv_read_fd(fd, buf, sizeof(buf));
    check(n == 11 && memcmp(buf, "onetwothree", 11) == 0, "appended contents");
    grv_close(fd);

    // --- O_EXCL: a create that refuses to clobber ---
    errno = 0;
    fd = grv_open(file, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_EXCL);
    check(fd == -1 && errno == EEXIST, "O_CREATE|O_EXCL on an existing file -> EEXIST");
    check(grv_stat(file, &st) == 0 && st.size == 11, "the refused create left it alone");

    const char* excl = "/tmp/ftdir/excl.txt";
    grv_unlink(excl);
    fd = grv_open(excl, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_EXCL);
    check(fd >= 0, "O_CREATE|O_EXCL creates a missing file");
    check(grv_write_fd(fd, "x", 1) == 1, "write the exclusive create");
    grv_close(fd);

    // --- fstat: the same record, read from the descriptor ---
    fd = grv_open(excl, GRV_O_RDONLY);
    check(fd >= 0, "open for fstat");
    check(grv_fstat(fd, &st) == 0 && st.size == 1 && !st.is_dir, "fstat a file");
    errno = 0;
    check(grv_fstat(99, &st) == -1 && errno == EBADF, "fstat bad fd -> EBADF");
    grv_close(fd);
    check(grv_unlink(excl) == 0, "unlink the exclusive file");

    check(grv_unlink(file) == 0, "unlink");
    check(grv_stat(file, &st) != 0, "stat after unlink fails");

    // --- rename + remove (the stdio spellings of the VFS operations) ---
    const char* from = "/tmp/ftdir/g.txt";
    const char* moved = "/tmp/ftdir/moved.txt";
    remove(moved);   // clean slate if a previous run left it (may fail)

    fd = grv_open(from, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    check(fd >= 0, "create for rename");
    grv_write_fd(fd, "payload", 7);
    grv_close(fd);

    check(rename(from, moved) == 0, "rename");
    check(grv_stat(from, &st) != 0, "rename dropped the old name");
    check(grv_stat(moved, &st) == 0 && st.size == 7, "rename lands the new name");
    fd = grv_open(moved, GRV_O_RDONLY);
    n = grv_read_fd(fd, buf, sizeof(buf));
    check(n == 7 && memcmp(buf, "payload", 7) == 0, "rename keeps contents");
    grv_close(fd);

    // Renaming onto an existing file replaces it (contents and size).
    fd = grv_open(from, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    grv_write_fd(fd, "new", 3);
    grv_close(fd);
    check(rename(from, moved) == 0, "rename replaces the destination");
    check(grv_stat(moved, &st) == 0 && st.size == 3, "replaced size");

    check(remove(moved) == 0, "remove");
    check(grv_stat(moved, &st) != 0, "stat after remove fails");
    errno = 0;
    check(rename("/tmp/ftdir/nope", "/tmp/ftdir/x") == -1 && errno == ENOENT,
          "rename missing source -> ENOENT");

    // --- a bare name is created in the CWD, never on the search path ---
    // A lookup searches the CWD then /bin; a *creation* must not, or
    // open("notes.txt", O_CREATE) would land in /bin/notes.txt.
    const char* bare = "ft-bare.txt";
    grv_unlink(bare);
    fd = grv_open(bare, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    check(fd >= 0, "create by bare name");
    grv_write_fd(fd, "cwd", 3);
    grv_close(fd);
    check(grv_stat("/ft-bare.txt", &st) == 0 && st.size == 3,
          "a bare create lands in the CWD");
    check(grv_stat("/bin/ft-bare.txt", &st) != 0,
          "a bare create does not land in /bin");
    check(grv_unlink(bare) == 0, "unlink the bare name");

    // --- unlink while open: the handle keeps the node alive ---
    // unlink drops the directory entry; a reader that already has the file must
    // still see its own bytes, even after the freed memory would be reused.
    const char* uaf   = "/tmp/ftdir/uaf.txt";
    const char* reuse = "/tmp/ftdir/reuse.txt";
    grv_unlink(uaf);
    grv_unlink(reuse);

    fd = grv_open(uaf, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    check(fd >= 0, "open for unlink-while-open");
    grv_write_fd(fd, "keepme", 6);
    check(grv_unlink(uaf) == 0, "unlink while open");
    check(grv_stat(uaf, &st) != 0, "the unlinked name is gone");

    // Churn the heap so a freed buffer would be handed to the next file.
    int rfd = grv_open(reuse, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    grv_write_fd(rfd, "ZZZZZZZZZZZZZZZZZZZZZZZZ", 24);
    grv_close(rfd);

    check(grv_lseek(fd, 0, 0) == 0, "seek the unlinked handle");
    check(grv_write_fd(fd, "KEEP", 4) == 4, "write the unlinked handle");
    check(grv_lseek(fd, 0, 0) == 0, "seek back");
    n = grv_read_fd(fd, buf, sizeof(buf));
    check(n == 6 && memcmp(buf, "KEEPme", 6) == 0,
          "the open handle still reads and writes its own bytes");
    grv_close(fd);
    check(grv_unlink(reuse) == 0, "unlink the reuse file");

    // --- the ramfs has a ceiling, and says ENOSPC at it ---
    // The backing store is the kernel heap: without a cap, a runaway writer
    // takes the whole thing and there is no error to report.
    const char* big = "/tmp/big.bin";
    grv_unlink(big);
    fd = grv_open(big, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    check(fd >= 0, "open for the size-cap test");
    static char chunk[4096];
    for (int i = 0; i < 4096; i++) chunk[i] = (char)(i & 0x7F);

    long total = 0;
    int capped = 0;
    for (int i = 0; i < 4096 && !capped; i++) { // up to 16MB of attempts
        errno = 0;
        long w = grv_write_fd(fd, chunk, sizeof(chunk));
        if (w < 0) { capped = (errno == ENOSPC); break; }
        total += w;
    }
    check(capped, "writing past the ramfs cap -> ENOSPC");
    check(total == (1 << 20), "the cap is per file, and the file reached it");
    grv_close(fd);

    // ...and the room comes back when the file goes.
    const char* big2 = "/tmp/big2.bin";
    grv_unlink(big2);
    fd = grv_open(big2, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    long again = 0;
    for (int i = 0; i < 256; i++) {
        long w = grv_write_fd(fd, chunk, sizeof(chunk));
        if (w < 0) break;
        again += w;
    }
    check(again == (1 << 20), "unlinking a big file frees its room");
    grv_close(fd);
    check(grv_unlink(big) == 0 && grv_unlink(big2) == 0, "clean up the big files");

    // --- an overflowing write count is refused, never copied ---
    // 'offset + count' wraps to a small end, which used to pass the cap check
    // and copy far past the heap buffer. The write must fail cleanly, leave the
    // file as it was, and leave the heap usable.
    const char* ovf = "/tmp/ftdir/ovf.bin";
    grv_unlink(ovf);
    fd = grv_open(ovf, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    check(fd >= 0, "open for the overflow test");
    check(grv_write_fd(fd, "safe", 4) == 4, "write before the overflow");

    unsigned long wrap = ~0UL - (1UL << 20) + 100; // 2^64 - 1 MB + 100: wraps
    errno = 0;
    long refused = grv_write_fd(fd, chunk, wrap);
    check(refused == -1 && errno == EINVAL, "overflowing count -> EINVAL");
    unsigned long absurd = 1UL << 40;              // absurd, but does not wrap
    errno = 0;
    refused = grv_write_fd(fd, chunk, absurd);
    check(refused == -1 && errno == EINVAL, "absurd count -> EINVAL");
    grv_close(fd);

    check(grv_stat(ovf, &st) == 0 && st.size == 4, "refused writes left the size alone");
    fd = grv_open(ovf, GRV_O_RDONLY);
    n = grv_read_fd(fd, buf, sizeof(buf));
    check(n == 4 && memcmp(buf, "safe", 4) == 0, "refused writes left the bytes alone");
    grv_close(fd);
    check(grv_unlink(ovf) == 0, "clean up the overflow file");

    // The heap still works after the refusals: a fresh file allocates and reads.
    const char* after = "/tmp/ftdir/after.bin";
    grv_unlink(after);
    fd = grv_open(after, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    check(fd >= 0 && grv_write_fd(fd, "intact", 6) == 6, "write after the refusals");
    grv_close(fd);
    fd = grv_open(after, GRV_O_RDONLY);
    n = grv_read_fd(fd, buf, sizeof(buf));
    check(n == 6 && memcmp(buf, "intact", 6) == 0, "the heap is intact after the refusals");
    grv_close(fd);
    check(grv_unlink(after) == 0, "clean up the heap check");

    if (fails == 0) { printf("filetest: all checks pass\n"); return 0; }
    printf("filetest: %d check(s) failed\n", fails);
    return 1;
}
