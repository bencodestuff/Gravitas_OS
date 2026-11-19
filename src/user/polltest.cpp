// polltest — POLL, the readiness syscall.
//
// A poll must answer three questions without getting them wrong: is there
// something to read (and does asking leave it there), does the deadline hold
// when nothing happens, and does a change made by another task end the wait
// rather than the deadline. The third is the one a busy loop can fake, so it is
// repeated: every pass has its own fresh pipe and its own writer task.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("polltest: FAIL: %s\n", what); fails++; }
}

// The array a forked child polls. It is at file scope, so the page is writable
// data: a fork write-protects every writable page it copies and marks it
// copy-on-write in the parent as well. The kernel's revents store has to make
// the child's page private first; if it went straight into the shared frame,
// the value the parent is still holding would change under it.
#define COW_SENTINEL 0x4000
static struct grv_pollfd cow_entry;

// Child mode: hold the write end and write one byte after a pause, so the
// parent's poll has to be woken by another task rather than by its own test.
static int late_writer(int argc, char** argv) {
    if (argc < 3) return 2;
    int wr = atoi(argv[2]);
    grv_sleep(150);
    char c = 'L';
    long n = grv_write_fd(wr, &c, 1);
    grv_close(wr);
    return n == 1 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && argv[1] != 0 && strcmp(argv[1], "late-writer") == 0)
        return late_writer(argc, argv);

    struct grv_pollfd one;
    char c = 0;

    // --- an empty pipe: nothing to read, room to write ---------------------
    int p[2];
    check(grv_pipe(p) == 0, "pipe() for the first pair");

    one.fd = p[0]; one.events = GRV_POLLIN; one.revents = -1;
    long r = grv_poll(&one, 1, 0);
    check(r == 0, "an empty pipe is not readable");
    check(one.revents == 0, "revents is cleared when nothing is ready");

    one.fd = p[1]; one.events = GRV_POLLOUT;
    r = grv_poll(&one, 1, 0);
    check(r == 1 && (one.revents & GRV_POLLOUT), "an empty pipe is writable");

    // --- a byte makes it readable, and polling does not take it ------------
    check(grv_write_fd(p[1], "A", 1) == 1, "write one byte");
    struct grv_pollfd both[2];
    both[0].fd = p[0]; both[0].events = GRV_POLLIN;  both[0].revents = 0;
    both[1].fd = p[1]; both[1].events = GRV_POLLOUT; both[1].revents = 0;
    r = grv_poll(both, 2, 0);
    check(r == 2, "both entries report after a byte");
    check((both[0].revents & GRV_POLLIN) && (both[1].revents & GRV_POLLOUT),
          "read end POLLIN and write end POLLOUT");
    check(grv_read_fd(p[0], &c, 1) == 1 && c == 'A',
          "the byte is still there after a poll");

    // --- the deadline, when nothing happens --------------------------------
    unsigned long t0 = grv_ticks();
    one.fd = p[0]; one.events = GRV_POLLIN;
    r = grv_poll(&one, 1, 100);
    unsigned long dt = grv_ticks() - t0;
    check(r == 0, "an empty pipe times out");
    check(dt >= 4 && dt <= 60, "the timeout waited about 100 ms");

    // --- a write from another task ends the wait ---------------------------
    for (int round = 0; round < 5; round++) {
        int q[2];
        if (grv_pipe(q) != 0) { check(0, "pipe() for a wake test"); break; }
        char fdarg[8];
        snprintf(fdarg, sizeof fdarg, "%d", q[1]);
        const char* av[3] = { "polltest.elf", "late-writer", fdarg };
        long pid = grv_spawn_bg("polltest.elf", av, 3);
        if (pid <= 0) { check(0, "spawn the late writer"); grv_close(q[0]); grv_close(q[1]); break; }

        one.fd = q[0]; one.events = GRV_POLLIN; one.revents = 0;
        t0 = grv_ticks();
        r = grv_poll(&one, 1, 2000);
        dt = grv_ticks() - t0;
        check(r == 1 && (one.revents & GRV_POLLIN),
              "a write from another task wakes a poll");
        check(dt < 1900, "the wake came before the deadline");
        check(grv_read_fd(q[0], &c, 1) == 1 && c == 'L', "the late byte reads back");
        check(grv_wait(pid) == 0, "the late writer exited cleanly");
        grv_close(q[0]);
        grv_close(q[1]);
    }

    // --- EOF: the last writer closes ---------------------------------------
    grv_close(p[1]);
    one.fd = p[0]; one.events = GRV_POLLOUT; one.revents = 0;  // ask for the wrong thing
    r = grv_poll(&one, 1, 0);
    check(r == 1, "EOF is ready even though POLLOUT was asked for");
    check((one.revents & GRV_POLLIN) && (one.revents & GRV_POLLHUP),
          "EOF reports POLLIN and POLLHUP");
    check(grv_read_fd(p[0], &c, 1) == 0, "a read at EOF returns 0");
    grv_close(p[0]);

    // --- a pipe whose reader has gone --------------------------------------
    int q[2];
    check(grv_pipe(q) == 0, "pipe() for the broken end");
    grv_close(q[0]);
    one.fd = q[1]; one.events = GRV_POLLOUT; one.revents = 0;
    r = grv_poll(&one, 1, 0);
    check(r == 1 && (one.revents & GRV_POLLERR),
          "a pipe with no reader reports POLLERR instead of POLLOUT");
    check((one.revents & GRV_POLLOUT) == 0, "a broken pipe is not ready for output");
    grv_close(q[1]);

    // --- a polled array is the child's own after a fork --------------------
    int fp[2];
    if (grv_pipe(fp) != 0) {
        check(0, "pipe() for the forked poll");
    } else if (grv_write_fd(fp[1], "F", 1) != 1) {
        check(0, "write one byte before the fork");
        grv_close(fp[0]);
        grv_close(fp[1]);
    } else {
        cow_entry.fd = fp[0];
        cow_entry.events = GRV_POLLIN;
        cow_entry.revents = COW_SENTINEL;
        long pid = grv_fork();
        if (pid == 0) {
            // Child: the entry it polls is copy-on-write, never written since
            // the split. grv_exit skips stdio so the parent's buffers stay its
            // own; the exit code carries the whole result back.
            long cr = grv_poll(&cow_entry, 1, 0);
            int seen = cow_entry.revents;
            int fd_seen = cow_entry.fd;
            grv_exit(cr == 1 && (seen & GRV_POLLIN) && fd_seen == fp[0] ? 0 : 1);
        }
        if (pid < 0) {
            check(0, "fork() for the forked poll");
        } else {
            check(grv_wait(pid) == 0, "the forked child polled the readable pipe");
            check(cow_entry.revents == COW_SENTINEL,
                  "a forked child's poll leaves the parent's revents alone");
            check(cow_entry.fd == fp[0], "the parent's entry is otherwise untouched");
        }
        cow_entry.revents = 0;
        r = grv_poll(&cow_entry, 1, 0);
        check(r == 1 && (cow_entry.revents & GRV_POLLIN),
              "the parent polls the same array after the fork");
        grv_close(fp[0]);
        grv_close(fp[1]);
    }

    // --- descriptors that are not there ------------------------------------
    one.fd = 15; one.events = GRV_POLLIN; one.revents = 0;   // inside the table, unused
    r = grv_poll(&one, 1, 0);
    check(r == 1 && (one.revents & GRV_POLLNVAL), "an unused slot reports POLLNVAL");

    one.fd = 16; one.events = GRV_POLLIN; one.revents = 0;   // outside the table
    errno = 0;
    r = grv_poll(&one, 1, 0);
    check(r == -1 && errno == EINVAL, "a descriptor outside the table is EINVAL");

    errno = 0;
    r = grv_poll(&one, GRV_POLL_MAX_FDS + 1, 0);
    check(r == -1 && errno == EINVAL, "one entry past the cap is EINVAL");

    // --- files and the console --------------------------------------------
    one.fd = 1; one.events = GRV_POLLIN; one.revents = 0;
    r = grv_poll(&one, 1, 0);
    check(r == 0, "the console never reports readable (the key is not taken)");
    one.events = GRV_POLLOUT;
    r = grv_poll(&one, 1, 0);
    check(r == 1 && (one.revents & GRV_POLLOUT), "the console is writable");

    r = grv_poll(0, 0, 30);
    check(r == 0, "an empty set waits out its timeout and returns 0");

    if (fails == 0) { printf("polltest: all checks pass\n"); return 0; }
    printf("polltest: %d check(s) failed\n", fails);
    return 1;
}
