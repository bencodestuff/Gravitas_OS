#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <grv/syscall.h>

// limittest — the capacities an external program can actually reach.
//
// docs/limits.md states the numbers; this proves the ones a guest can drive:
// the per-task descriptor count, a full mailbox, a 200-byte name on the ramfs,
// and a working directory whose absolute path is longer than the old 63-byte
// field. It deliberately does not fill /disk: the shipped image is shared with
// every other test in this group, and a full disk would make later tests fail
// for the wrong reason. ext2 ENOSPC is deferred (see docs/limits.md).

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("limittest: FAIL: %s\n", what); fails++; }
}

// The VFS keeps 16 descriptor slots and 0, 1 and 2 are the standard streams.
#define USABLE_FDS 13

// C++ operator new must return memory aligned to
// __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16 on x86-64). Odd sizes are the point:
// with an 8-byte-rounded allocator the second payload lands at 8 mod 16.
static void test_new_alignment(void) {
    const size_t sizes[10] = {1, 3, 9, 17, 33, 63, 65, 129, 257, 511};
    char* p[10] = {0};

    int allocated = 1;
    int aligned = 1;
    for (int i = 0; i < 10; i++) {
        p[i] = new char[sizes[i]];
        if (p[i] == 0) { allocated = 0; break; }
        if (((unsigned long)p[i] & 15ul) != 0) aligned = 0;
        memset(p[i], 0xA5, sizes[i]);
    }
    check(allocated, "odd-sized objects can be allocated");
    check(aligned, "operator new returns 16-byte aligned memory for odd sizes");

    // Free every other block and allocate into the holes again: split and
    // reuse must not disturb the alignment of either the hole or its
    // neighbours.
    for (int i = 0; i < 10; i += 2) { delete[] p[i]; p[i] = 0; }
    aligned = 1;
    for (int i = 0; i < 10; i += 2) {
        p[i] = new char[sizes[i] + 8];
        if (p[i] == 0) { aligned = 0; break; }
        if (((unsigned long)p[i] & 15ul) != 0) aligned = 0;
    }
    check(aligned, "reused odd-sized blocks stay 16-byte aligned");
    for (int i = 0; i < 10; i++) delete[] p[i];
}

static void test_fd_capacity(void) {
    int fds[USABLE_FDS];
    int n = 0;
    int extra = 0;

    for (;;) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/limits-fd-%d", n);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            check(errno == EMFILE, "the descriptor past the cap says EMFILE");
            break;
        }
        if (n == USABLE_FDS) {
            // One more than documented: do not keep opening forever.
            close(fd);
            extra = 1;
            break;
        }
        fds[n++] = fd;
    }

    check(!extra, "no 14th descriptor is handed out");
    check(n == USABLE_FDS, "13 usable descriptors after the three standard streams");
    if (n > 0) check(fds[0] == 3, "the first open() is descriptor 3");

    for (int i = 0; i < n; i++) {
        close(fds[i]);
        char path[64];
        snprintf(path, sizeof(path), "/tmp/limits-fd-%d", i);
        unlink(path);
    }

    // Closing them all must give the slots back.
    int fd = open("/tmp/limits-after-close", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd == 3, "closing the table makes descriptor 3 available again");
    if (fd >= 0) { close(fd); unlink("/tmp/limits-after-close"); }
}

static void test_mailbox_capacity(void) {
    static struct grv_taskinfo tasks[64];
    int n = grv_task_list(tasks, 64);
    long me = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(tasks[i].name, "limittest.elf") == 0 &&
            tasks[i].state == 2) {
            me = tasks[i].id;
            break;
        }
    }
    check(me > 0, "task list names the running test");
    if (me <= 0) return;

    struct grv_msg m;
    for (int i = 0; i < 16; i++) {
        memset(&m, 0, sizeof(m));
        m.type = 77;
        m.a = (unsigned long)(i + 1);
        errno = 0;
        long r = grv_send(me, &m);
        check(r == 0, "a message into an empty mailbox is delivered");
        if (r != 0) return;
    }

    memset(&m, 0, sizeof(m));
    errno = 0;
    check(grv_send(me, &m) == -1 && errno == EAGAIN,
          "the 17th message says EAGAIN");

    // FIFO order, then the box is empty again.
    for (int i = 0; i < 16; i++) {
        memset(&m, 0, sizeof(m));
        long r = grv_recv(&m, 0);
        check(r == 0 && m.a == (unsigned long)(i + 1),
              "messages drain in the order they were sent");
        if (r != 0) return;
    }
    memset(&m, 0, sizeof(m));
    errno = 0;
    check(grv_recv(&m, 0) == -1 && errno == EAGAIN,
          "a drained mailbox polls as EAGAIN");
}

static void test_shared_region_limits(void) {
    // Shared regions are capped at 64 MiB (MAX_REGION_BYTES in memmap.cpp)
    // and page-rounded. A length near UINT64_MAX used to wrap that rounding
    // to zero, pass the cap check and return an id for an unusable region.
    check(grv_shm_create(~0ul) < 0, "a region of all-ones bytes is refused");
    check(grv_shm_create(~0ul - 100) < 0, "a region near UINT64_MAX is refused");
    check(grv_shm_create((64ul << 20) + 1) < 0, "a region past 64 MiB is refused");
    check(grv_shm_create(0) < 0, "a zero-byte region is refused");

    // A refusal must not consume the id sequence or a region slot: the next
    // ordinary request still works.
    int id = grv_shm_create(4096);
    check(id > 0, "an ordinary region still succeeds after the refusals");
    if (id > 0) check(grv_shm_close(id) == 0, "and can be closed");
}

static void test_long_name(void) {
    // 200 bytes: longer than the old 63-byte RamFS field, well inside NAME_MAX.
    char name[201];
    for (int i = 0; i < 200; i++) name[i] = (char)('a' + (i % 26));
    name[200] = '\0';

    char path[512];
    snprintf(path, sizeof(path), "/tmp/%s", name);

    unlink(path); // a previous run may have left it
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "a 200-byte name can be created on /tmp");

    const char* text = "long-name-ok";
    if (fd >= 0) {
        check(write(fd, text, strlen(text)) == (ssize_t)strlen(text),
              "and written through");
        close(fd);
    }

    // The reopen is the real test: a truncated stored name does not match the
    // 200 bytes that asked for it.
    fd = open(path, O_RDONLY);
    check(fd >= 0, "the same 200-byte name opens again");
    if (fd >= 0) {
        char buf[32];
        ssize_t got = read(fd, buf, sizeof(buf) - 1);
        if (got < 0) got = 0;
        buf[got] = '\0';
        check(strcmp(buf, text) == 0, "and reads back the bytes written");
        close(fd);
    }

    // readdir reports the whole name too, not a prefix.
    DIR* d = opendir("/tmp");
    int listed = 0;
    struct dirent* e;
    while (d != 0 && (e = readdir(d)) != 0) {
        if (strcmp(e->d_name, name) == 0) { listed = 1; break; }
    }
    if (d != 0) closedir(d);
    check(listed, "the listing carries the whole 200-byte name");

    unlink(path);
}

static void test_long_cwd(void) {
    // An absolute path comfortably past the old 63-byte CWD field.
    char dir[128];
    int n = 0;
    const char* base = "/tmp/";
    for (const char* p = base; *p; p++) dir[n++] = *p;
    for (int i = 0; i < 80; i++) dir[n++] = 'c';
    dir[n] = '\0';
    check(n > 63, "the test directory's path is longer than 63 bytes");

    mkdir(dir, 0755);

    char file[160];
    snprintf(file, sizeof(file), "%s/f.txt", dir);
    unlink(file);
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "a file in the long directory can be created");
    if (fd >= 0) {
        check(write(fd, "cwd-ok", 6) == 6, "and written");
        close(fd);
    }

    errno = 0;
    check(chdir(dir) == 0, "chdir accepts an 86-byte directory path");

    char cwd[512];
    errno = 0;
    check(getcwd(cwd, sizeof(cwd)) != 0 && strcmp(cwd, dir) == 0,
          "getcwd reports the whole path");

    fd = open("f.txt", O_RDONLY);   // relative to the new CWD
    check(fd >= 0, "a relative name resolves inside the long CWD");
    if (fd >= 0) {
        char buf[16];
        ssize_t got = read(fd, buf, sizeof(buf) - 1);
        if (got < 0) got = 0;
        buf[got] = '\0';
        check(strcmp(buf, "cwd-ok") == 0, "and reads the file there");
        close(fd);
    }

    chdir("/tmp");
    unlink(file);
    check(rmdir(dir) == 0, "the long directory can be removed again");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    test_fd_capacity();
    test_mailbox_capacity();
    test_new_alignment();
    test_shared_region_limits();
    test_long_name();
    test_long_cwd();

    if (fails == 0) { printf("limittest: all checks pass\n"); return 0; }
    printf("limittest: %d check(s) failed\n", fails);
    return 1;
}
