#include <stdio.h>
#include <string.h>
#include <grv/syscall.h>

// shmtest — the two kinds of mapping:
//
//   * a FILE mapped read-only out of the immutable image, zero-copy (the bytes
//     are the image's own frames, so nothing is copied and nothing can change);
//   * a SHARED REGION, which two tasks read and write through — what four-word
//     messages could never carry.
//
// Its partner is shmhelper.elf, which attaches the same region and proves both
// tasks see the same pages. Prints PASS/FAIL for the test harness.

#define MSG_GO     0x7001
#define MSG_RESULT 0x7002

#define OK_READ    0x1  /* the partner saw our pattern */
#define OK_DENIED  0x2  /* the partner was refused, as it should have been */

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("shmtest: FAIL: %s\n", what); fails++; }
}

/* Wait for the partner's result message, or give up. */
static int wait_result(struct grv_msg* out) {
    for (int i = 0; i < 400; i++) {
        if (grv_recv(out, 0) == 0 && out->type == MSG_RESULT) return 1;
        grv_sleep(5);
    }
    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    struct grv_map m;

    // --- a file mapped read-only, zero-copy ---
    // /lua/hello.lua still lives in the immutable image (the tests above never
    // write to it), so it can be mapped; a file the ramfs owns cannot.
    check(grv_map_file("/lua/hello.lua", &m) == 0 && m.size > 0 && m.address != 0,
          "map a file");
    if (m.size > 0) {
        struct grv_stat st;
        check(grv_stat("/lua/hello.lua", &st) == 0 && st.size == m.size,
              "the mapped size is the file's size");
        char buf[256];
        int fd = grv_open("/lua/hello.lua", GRV_O_RDONLY);
        long n = fd >= 0 ? grv_read_fd(fd, buf, sizeof(buf)) : -1;
        if (fd >= 0) grv_close(fd);
        check(n > 0 && memcmp((const void*)m.address, buf, (size_t)n) == 0,
              "mapped bytes match a read");
        check(grv_munmap(m.address) == 0, "unmap the file");
    }

    // A bigger file: several pages, and it does not start on a page boundary,
    // so the mapping has to offset into its first page.
    struct grv_map big;
    if (grv_map_file("/Pictures/Rainier.bmp", &big) == 0) {
        check(big.size > 4096, "mapped a multi-page file");
        char head[16];
        int fd = grv_open("/Pictures/Rainier.bmp", GRV_O_RDONLY);
        long n = fd >= 0 ? grv_read_fd(fd, head, sizeof(head)) : -1;
        if (fd >= 0) grv_close(fd);
        check(n == (long)sizeof(head) &&
              memcmp((const void*)big.address, head, sizeof(head)) == 0,
              "a multi-page mapping starts at the file's first byte");
        check(grv_munmap(big.address) == 0, "unmap the big file");
    }

    // A file the ramfs owns cannot be mapped: its bytes can be reallocated.
    int fd = grv_open("/tmp/not-mappable.txt", GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    if (fd >= 0) { grv_write_fd(fd, "x", 1); grv_close(fd); }
    check(grv_map_file("/tmp/not-mappable.txt", &m) != 0,
          "a writable file refuses a mapping");
    check(grv_map_file("/no/such/file", &m) != 0, "a missing file refuses a mapping");

    // --- a shared region, read and written by two tasks ---
    int id = grv_shm_create(4096);
    check(id > 0, "create a shared region");
    if (id > 0) {
        struct grv_map a;
        check(grv_shm_map(id, &a) == 0 && a.size == 4096, "attach it here");
        if (a.size == 4096) {
            volatile unsigned int* p = (volatile unsigned int*)a.address;
            for (int i = 0; i < 64; i++) p[i] = 0x11110000u + (unsigned)i;
            check(p[0] == 0x11110000u && p[63] == 0x1111003Fu,
                  "wrote through the mapping");

            const char* cargv[2] = { "shmhelper.elf", "ok" };
            long child = grv_spawn_bg("shmhelper.elf", cargv, 2);
            check(child > 0, "spawn the partner");
            if (child > 0) {
                check(grv_shm_grant(id, (int)child) == 0, "grant the region");
                struct grv_msg go;
                go.from = 0; go.type = MSG_GO; go.a = (unsigned long)id; go.b = 0;
                grv_send(child, &go);

                struct grv_msg r;
                check(wait_result(&r) && r.a == OK_READ,
                      "the partner read our pattern");
                check(grv_wait(child) == 0, "the partner exited cleanly");
                // Its writes are visible here: it bumped every word.
                check(p[0] == 0x11110001u && p[63] == 0x11110040u,
                      "the partner's writes are visible to us");
            }
        }
        check(grv_shm_close(id) == 0, "close the region");
    }

    // --- a task without a grant is refused ---
    int secret = grv_shm_create(4096);
    check(secret > 0, "create a second region");
    if (secret > 0) {
        const char* cargv[2] = { "shmhelper.elf", "deny" };
        long child = grv_spawn_bg("shmhelper.elf", cargv, 2);
        if (child > 0) {
            struct grv_msg go;
            go.from = 0; go.type = MSG_GO; go.a = (unsigned long)secret; go.b = 0;
            grv_send(child, &go);
            struct grv_msg r;
            check(wait_result(&r) && r.a == OK_DENIED,
                  "a task without a grant was refused");
            grv_wait(child);
        }
        check(grv_shm_close(secret) == 0, "close the second region");
    }

    // A handle we do not own cannot be closed.
    check(grv_shm_close(9999) != 0, "closing an unknown region is refused");

    if (fails == 0) { printf("shmtest: all checks pass\n"); return 0; }
    printf("shmtest: %d check(s) failed\n", fails);
    return 1;
}
