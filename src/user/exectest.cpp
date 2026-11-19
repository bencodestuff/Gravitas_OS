// exectest: exec replaces the image and keeps the process.
//
// The contract under test:
//   1. a successful exec does not return: the new image starts at its own
//      entry point, on its own stack, with the memory of the old one gone
//      (the file's initialised globals are back, not what the old image set);
//   2. the process survives: the same task id, so a parent blocked in wait
//      still collects the status, and the exit status is the code the new
//      image passed to exit();
//   3. the working directory, the console binding and open descriptors are
//      kept; a descriptor keeps its open file description, so a write before
//      and a write after the exec land at consecutive offsets;
//   4. a failed exec (missing file, not an ELF) returns -1 with errno and
//      leaves the caller exactly as it was: same id, same cwd, same global
//      values, same open descriptors;
//   5. the process is not duplicated: the parent's wait for the id it forked
//      is the one that collects the new image's status.
//
// Modes (argv[1]):
//   (none)  driver: forks the two cases below and checks what they leave
//   after   the post-exec image: identity, cwd, memory and descriptor checks
//   fail    the failure case: bad execs must change nothing

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <grv/syscall.h>

#define FRESH_MAGIC 0xE1F0A5A5UL   // the initialiser in this file
#define OLD_MAGIC   0x5EED1234UL   // what the old image writes over it

#define EXEC_OK 42                 // "after" mode exits with this
#define FAIL_OK 7                  // "fail" mode exits with this

static volatile unsigned long image_magic = FRESH_MAGIC;
static int failures = 0;

static void fail(const char* what) {
    printf("[exectest] FAIL %s\n", what);
    failures++;
}

// The running task's id: exactly one task reports state 2 (running), and that
// is this one. Used to prove the id is the same on both sides of an exec.
static int self_id(void) {
    struct grv_taskinfo tasks[32];
    int n = grv_task_list(tasks, 32);
    for (int i = 0; i < n; i++)
        if (tasks[i].state == 2) return tasks[i].id;
    return -1;
}

static int write_text(const char* path, const char* text, int flags) {
    int fd = grv_open(path, flags);
    if (fd < 0) return -1;
    if (grv_write_fd(fd, text, (unsigned long)strlen(text)) !=
        (long)strlen(text)) {
        grv_close(fd);
        return -1;
    }
    return fd;
}

// Read a whole file into buf (NUL-terminated). Returns the length, or -1.
static long read_text(const char* path, char* buf, unsigned long cap) {
    int fd = grv_open(path, GRV_O_RDONLY);
    if (fd < 0) return -1;
    long n = grv_read_fd(fd, buf, cap - 1);
    grv_close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

// --- the new image ---

static int after_mode(void) {
    printf("[exectest] after: image is running\n");

    int id = self_id();
    if (id <= 0) fail("could not find our own task id");

    char cwd[256];
    long clen = grv_getcwd(cwd, sizeof(cwd));
    if (clen < 0 || strcmp(cwd, "/tmp") != 0) {
        printf("[exectest] cwd is '%s', want /tmp\n", cwd);
        fail("working directory did not survive exec");
    }

    // The old image set image_magic to OLD_MAGIC just before the exec. A new
    // address space starts from the file's initialiser instead.
    if (image_magic != FRESH_MAGIC) {
        printf("[exectest] global is %lx, want %lx\n", image_magic, FRESH_MAGIC);
        fail("memory of the old image survived exec");
    }

    // The old image recorded its id and the descriptor it left open.
    char state[64];
    if (read_text("/tmp/exec-state.txt", state, sizeof(state)) < 0) {
        fail("could not read the state the old image left");
        return 1;
    }
    int old_id = -1, fd = -1;
    {
        // The libc has no sscanf; the file is "<id> <fd>\n".
        int i = 0;
        old_id = 0;
        while (state[i] >= '0' && state[i] <= '9')
            old_id = old_id * 10 + (state[i++] - '0');
        while (state[i] == ' ') i++;
        fd = 0;
        while (state[i] >= '0' && state[i] <= '9')
            fd = fd * 10 + (state[i++] - '0');
        if (old_id <= 0 || fd < 0) {
            fail("state file is malformed");
            return 1;
        }
    }
    printf("[exectest] after: id %d -> %d, fd %d\n", old_id, id, fd);
    if (id != old_id) fail("task id changed across exec");

    // The descriptor is still the same open file description: the write
    // continues where the old image stopped.
    if (fd < 0 || grv_write_fd(fd, "B", 1) != 1)
        fail("descriptor did not survive exec");
    else
        grv_close(fd);

    char content[16];
    if (read_text("/tmp/exec-fd.txt", content, sizeof(content)) < 0 ||
        strcmp(content, "AB") != 0) {
        printf("[exectest] file has '%s', want 'AB'\n", content);
        fail("file offset did not survive exec");
    }

    printf("[exectest] after: id=%d cwd=%s\n", id, cwd);
    if (failures) return 1;
    printf("[exectest] after: PASS\n");
    return EXEC_OK;
}

// --- the failure case ---

static int fail_mode(void) {
    printf("[exectest] fail: checking a failed exec changes nothing\n");
    int id = self_id();
    if (id <= 0) fail("could not find our own task id");

    if (grv_chdir("/tmp") != 0) fail("chdir /tmp failed");

    int fd = write_text("/tmp/exec-fail.txt", "C",
                        GRV_O_CREATE | GRV_O_TRUNC | GRV_O_WRONLY);
    if (fd < 0) fail("could not open the scratch file");
    image_magic = OLD_MAGIC;

    const char* av[] = {"exectest.elf"};
    errno = 0;
    long r = grv_exec("no-such-program.elf", av, 1);
    printf("[exectest] fail: exec(missing) -> %d errno=%d\n", (int)r, errno);
    if (r != -1 || errno != ENOENT) fail("a missing image did not report ENOENT");

    errno = 0;
    r = grv_exec("/hello.txt", av, 1);
    printf("[exectest] fail: exec(not an ELF) -> %d errno=%d\n", (int)r, errno);
    if (r != -1 || errno != ENOEXEC)
        fail("a non-ELF file did not report ENOEXEC");

    if (self_id() != id) fail("failed exec replaced the process");
    if (image_magic != OLD_MAGIC) fail("failed exec zeroed our memory");

    char cwd[256];
    if (grv_getcwd(cwd, sizeof(cwd)) < 0 || strcmp(cwd, "/tmp") != 0)
        fail("failed exec changed the working directory");

    // The descriptor must still be open and still at the same offset.
    if (fd < 0 || grv_write_fd(fd, "D", 1) != 1) {
        fail("failed exec lost an open descriptor");
    } else {
        grv_close(fd);
    }
    char content[16];
    if (read_text("/tmp/exec-fail.txt", content, sizeof(content)) < 0 ||
        strcmp(content, "CD") != 0) {
        printf("[exectest] file has '%s', want 'CD'\n", content);
        fail("failed exec disturbed a descriptor");
    }

    if (failures) return 1;
    printf("[exectest] fail: PASS\n");
    return FAIL_OK;
}

// --- the driver ---

int main(int argc, char** argv) {
    const char* mode = (argc > 1) ? argv[1] : "";
    if (strcmp(mode, "after") == 0) return after_mode();
    if (strcmp(mode, "fail") == 0) return fail_mode();

    grv_unlink("/tmp/exec-fd.txt");
    grv_unlink("/tmp/exec-state.txt");
    grv_unlink("/tmp/exec-fail.txt");

    // Round 1: a successful exec in a forked child. The parent's wait on the
    // id it forked is what proves the process (not just the image) survived.
    long pid = grv_fork();
    if (pid < 0) {
        printf("[exectest] FAIL fork -> %d\n", (int)pid);
        return 1;
    }
    if (pid == 0) {
        int id = self_id();
        printf("[exectest] child: id %d, exec now\n", id);
        if (id <= 0) grv_exit(1);
        if (grv_chdir("/tmp") != 0) grv_exit(1);

        int fd = write_text("/tmp/exec-fd.txt", "A",
                            GRV_O_CREATE | GRV_O_TRUNC | GRV_O_WRONLY);
        if (fd < 0) grv_exit(1);

        char state[64];
        snprintf(state, sizeof(state), "%d %d\n", id, fd);
        int sfd = write_text("/tmp/exec-state.txt", state,
                             GRV_O_CREATE | GRV_O_TRUNC | GRV_O_WRONLY);
        if (sfd < 0) grv_exit(1);
        grv_close(sfd);

        image_magic = OLD_MAGIC;
        const char* av[] = {"exectest.elf", "after"};
        long r = grv_exec("exectest.elf", av, 2);
        printf("[exectest] FAIL exec returned %d errno=%d\n", (int)r, errno);
        grv_exit(1);
    }

    long code = grv_wait(pid);
    printf("[exectest] wait(%d) -> %d (want %d)\n", (int)pid, (int)code,
           EXEC_OK);
    if (code != EXEC_OK) fail("the parent did not collect the new image's status");

    char content[16];
    if (read_text("/tmp/exec-fd.txt", content, sizeof(content)) < 0 ||
        strcmp(content, "AB") != 0) {
        printf("[exectest] file has '%s', want 'AB'\n", content);
        fail("descriptor did not carry across the exec");
    }

    // Round 2: two failing execs in another forked child; it must come back
    // to the same image and report so with its own exit code.
    long pid2 = grv_fork();
    if (pid2 < 0) {
        printf("[exectest] FAIL fork -> %d\n", (int)pid2);
        return 1;
    }
    if (pid2 == 0) grv_exit(fail_mode());

    code = grv_wait(pid2);
    printf("[exectest] wait(%d) -> %d (want %d)\n", (int)pid2, (int)code,
           FAIL_OK);
    if (code != FAIL_OK) fail("a failed exec damaged the caller");
    if (read_text("/tmp/exec-fail.txt", content, sizeof(content)) < 0 ||
        strcmp(content, "CD") != 0)
        fail("failed exec lost the descriptor's contents");

    if (failures) {
        printf("[exectest] FAIL %d check(s)\n", failures);
        return 1;
    }
    printf("[exectest] PASS\n");
    return 0;
}
