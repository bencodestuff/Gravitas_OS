#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <grv/syscall.h>

// posixtest — exercises the POSIX facade (fcntl/unistd/sys-stat/dirent) against
// the filesystem and the console descriptors 0/1/2. Prints PASS/FAIL for the
// test harness.

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("posixtest: FAIL: %s\n", what); fails++; }
}

// --- five readers on one pipe ---------------------------------------------
// A pipe parks blocked tasks by task id in a fixed four-slot list, and every
// wake path walks that list. Five concurrent readers therefore force the one
// case the kernel must survive: a reader that cannot be registered must not be
// parked with no way back. Writers then write and close, and every reader must
// come away with its byte (or EOF, on the pipe that never gets one) instead of
// sleeping forever.
#define MSG_GO     0x7101
#define MSG_RESULT 0x7102
#define READERS    5

// Reader mode: the parent spawned this copy of the program with the pipe's two
// ends. argv[1] is "pipe-read", argv[2] the read end, argv[3] the write end it
// inherited and must drop: a reader that kept its own write end would hold the
// writer count above zero, so no reader could ever reach EOF.
static int pipe_reader(int argc, char** argv) {
    if (argc < 4) return 2;
    int rd = atoi(argv[2]);
    int wr = atoi(argv[3]);
    grv_close(wr);
    struct grv_msg go;
    if (grv_recv(&go, 1) != 0 || go.type != MSG_GO) return 3;
    char c = 0;
    long n = grv_read_fd(rd, &c, 1);
    struct grv_msg out;
    out.from = 0;
    out.type = MSG_RESULT;
    out.a = n < 0 ? (unsigned long)-1 : (unsigned long)n;
    out.b = (unsigned char)c;
    grv_send((long)go.from, &out);     // the sender id is kernel-stamped
    return n < 0 ? 1 : 0;
}

// Five readers on one fresh pipe, all blocked before anything happens, then
// either five bytes and a close, or just a close. 'want_data' picks which.
static void pipe_competing_readers(int want_data) {
    int pfd[2];
    if (grv_pipe(pfd) != 0) { check(0, "pipe() for five readers"); return; }
    char rd[8], wr[8];
    snprintf(rd, sizeof rd, "%d", pfd[0]);
    snprintf(wr, sizeof wr, "%d", pfd[1]);

    long pid[READERS];
    int spawned = 0;
    for (int i = 0; i < READERS; i++) {
        const char* av[4] = {"posixtest.elf", "pipe-read", rd, wr};
        pid[i] = grv_spawn_bg("posixtest.elf", av, 4);
        if (pid[i] > 0) spawned++;
    }
    check(spawned == READERS, want_data ? "spawn five readers (data)"
                                        : "spawn five readers (EOF)");
    for (int i = 0; i < READERS; i++) {
        if (pid[i] <= 0) continue;
        struct grv_msg go;
        go.from = 0; go.type = MSG_GO; go.a = 0; go.b = 0;
        grv_send(pid[i], &go);
    }

    // Give every reader time to block on the empty pipe: that is the state
    // under test, with more waiters than the kernel's list can hold.
    grv_sleep(300);
    if (want_data)
        check(grv_write_fd(pfd[1], "ABCDE", READERS) == READERS,
              "write five bytes to a contended pipe");
    grv_close(pfd[1]);                 // last writer: data is in, or EOF
    grv_close(pfd[0]);                 // our own read end is no reader

    int got = 0, with_bytes = 0;
    unsigned long end = grv_ticks() + 200;    // 4 s at 50 Hz
    while (got < READERS && (long)(grv_ticks() - end) < 0) {
        struct grv_msg m;
        if (grv_recv(&m, 0) == 0 && m.type == MSG_RESULT) {
            got++;
            if (m.a != 0) with_bytes++;
        } else {
            grv_sleep(10);
        }
    }
    check(got == READERS, want_data ? "all five readers reported after data"
                                    : "all five readers reported EOF");
    check(with_bytes == (want_data ? READERS : 0),
          want_data ? "every reader got its byte"
                    : "no reader got a byte on the empty pipe");
}

int main(int argc, char** argv) {
    if (argc > 1 && argv[1] != 0 && strcmp(argv[1], "pipe-read") == 0)
        return pipe_reader(argc, argv);

    const char* path = "/tmp/posix.txt";
    unlink(path);   // clean slate if a previous run left it

    // --- open / write / lseek / close ---------------------------------
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open O_CREAT|O_TRUNC");
    if (fd < 0) { printf("posixtest: %d check(s) failed\n", fails); return 1; }
    check(write(fd, "hello posix", 11) == 11, "write");
    check(lseek(fd, 0, SEEK_SET) == 0, "lseek");
    check(close(fd) == 0, "close");

    // --- stat ----------------------------------------------------------
    struct stat st;
    check(stat(path, &st) == 0, "stat");
    check(st.st_size == 11, "stat size");
    check(S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode), "stat mode is regular");

    fd = open(path, O_RDONLY);
    check(fd >= 0, "open O_RDONLY");
    char buf[32];
    check(read(fd, buf, 11) == 11 && memcmp(buf, "hello posix", 11) == 0, "read");
    check(close(fd) == 0, "close read");

    // --- the console descriptors --------------------------------------
    check(write(STDOUT_FILENO, "posixtest: via write(1)\n", 24) == 24, "write(1)");
    check(access(path, F_OK) == 0, "access existing");
    check(access("/tmp/no-such-file", F_OK) == -1, "access missing");

    // --- getcwd / chdir -------------------------------------------------
    char cwd[64];
    check(getcwd(cwd, sizeof cwd) != 0 && cwd[0] == '/', "getcwd");
    check(chdir("/tmp") == 0, "chdir /tmp");
    check(getcwd(cwd, sizeof cwd) != 0 && strcmp(cwd, "/tmp") == 0,
          "getcwd after chdir");
    check(chdir("/") == 0, "chdir back");

    // --- dirent ---------------------------------------------------------
    DIR* d = opendir("/bin");
    check(d != 0, "opendir /bin");
    if (d != 0) {
        int count = 0, saw_tests = 0;
        ino_t ino = 0;
        struct dirent* e;
        while ((e = readdir(d)) != 0) {
            count++;
            if (e->d_ino != 0) ino = e->d_ino;
            if (strcmp(e->d_name, "tests") == 0 && e->d_type == DT_DIR)
                saw_tests = 1;
        }
        check(count > 0, "readdir found entries");
        check(saw_tests, "readdir: tests is a directory");
        check(ino != 0, "readdir reports inode numbers");
        check(closedir(d) == 0, "closedir");
    }

    check(stat("/bin", &st) == 0 && S_ISDIR(st.st_mode), "stat a directory");
    check(st.st_ino != 0, "stat reports an inode number");
    check((st.st_mode & 0777) != 0, "stat reports permission bits");

    // --- errors ---------------------------------------------------------
    errno = 0;
    check(open("/tmp/no-such-file", O_RDONLY) == -1 && errno == ENOENT,
          "open missing -> ENOENT");
    errno = 0;
    // The image tree is writable through the overlay: opening an image file for
    // writing copies it into the upper layer (O_TRUNC would shadow it instead).
    int wfd = open("/hello.txt", O_WRONLY | O_CREAT);
    check(wfd >= 0, "open an image file for writing (overlay)");
    if (wfd >= 0) close(wfd);
    errno = 0;
    check(opendir("/hello.txt") == 0, "opendir a file fails");

    // --- mkdir / rmdir --------------------------------------------------
    unlink("/tmp/pd");
    check(mkdir("/tmp/pd", 0755) == 0, "mkdir");
    check(stat("/tmp/pd", &st) == 0 && S_ISDIR(st.st_mode), "mkdir then stat");
    check(rmdir("/tmp/pd") == 0, "rmdir");

    check(unlink(path) == 0, "unlink");
    check(access(path, F_OK) == -1, "gone after unlink");

    // --- five readers on one pipe ---------------------------------------
    // Data first, then the empty pipe: the second case exercises the close
    // wake, where the last writer's exit is the only thing a reader can run on.
    pipe_competing_readers(1);
    pipe_competing_readers(0);

    if (fails == 0) { printf("posixtest: all checks pass\n"); return 0; }
    printf("posixtest: %d check(s) failed\n", fails);
    return 1;
}
