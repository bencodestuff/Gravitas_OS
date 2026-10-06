#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <grv/syscall.h>

// filetest — exercises seek/stat/truncate/create/remove/rename on the writable
// ramfs (/tmp). Prints PASS/FAIL for the test harness.

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

    if (fails == 0) { printf("filetest: all checks pass\n"); return 0; }
    printf("filetest: %d check(s) failed\n", fails);
    return 1;
}
