#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <grv/syscall.h>

// memtest — the memory-boundary regressions:
//
//   * a huge sparse image on /disk must be refused by spawn before the kernel
//     fills the heap with it, and the shell must still be alive afterwards;
//   * brk shrink must unmap the pages it removes (so a regrow does not leak a
//     second set of frames), and pages handed to userland must be zeroed;
//   * a task that maps the framebuffer and exits must not hand the scanout
//     frames back to the PMM (free memory must not jump).
//
// The helper mode is the same program spawned with "fbmap": it maps the
// framebuffer once and exits, which is the teardown path the parent measures.

#define MIB (1024ul * 1024ul)
#define BRK_TEST_BYTES MIB
#define BRK_TEST_PAGES (BRK_TEST_BYTES / 4096)

static int fails = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        printf("memtest: FAIL: %s\n", what);
        fails++;
    }
}

static long mem_free(void) {
    struct grv_sysinfo si;
    grv_sysinfo(&si);
    return (long)si.mem_free;
}

// The child mode: one FBINFO call, then exit, so the parent can measure the
// frame counters across address-space teardown.
static int fbmap_child(void) {
    struct grv_fb_info info;
    if (grv_fb_info(&info) == 0)
        printf("memtest: child mapped the framebuffer (%lu bytes)\n", info.size);
    else
        printf("memtest: child has no framebuffer\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "fbmap") == 0) return fbmap_child();

    // --- (a) spawn of an oversized sparse image ---
    // ftruncate only changes i_size on ext2, so this costs no disk blocks. The
    // kernel must refuse it with E2BIG rather than trying to allocate a
    // gigabyte from the 32 MB heap (where the old code read through null).
    const char* sparse = "/disk/memtest-sparse.bin";
    int fd = open(sparse, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check(fd >= 0, "create the sparse image");
    if (fd >= 0) {
        check(grv_ftruncate(fd, 0x40000000ul) == 0, "grow it to 1 GiB sparsely");
        close(fd);

        errno = 0;
        long child = grv_spawn(sparse, nullptr, 0);
        check(child == -1, "spawn refuses an oversized image");
        check(errno == E2BIG, "the refusal is E2BIG");
        printf("memtest: oversized spawn refused (errno %d)\n", errno);
        unlink(sparse);
    }
    printf("memtest: kernel survived the oversized spawn\n");

    // --- (b) brk shrink, regrow and zeroing ---
    // brk is managed directly here, not through malloc: libc's allocator
    // caches the break and this program does not allocate after this point.
    char* base = (char*)(((unsigned long)grv_brk(nullptr) + 4095ul) & ~4095ul);
    char* grown = base + BRK_TEST_BYTES;

    long free_before = mem_free();
    check(grv_brk(grown) == grown, "grow brk by 1 MiB");

    long free_grown = mem_free();
    long used = free_before - free_grown;
    check(used >= (long)BRK_TEST_PAGES && used <= (long)BRK_TEST_PAGES + 8,
          "growing brk uses one frame per page (plus page tables)");

    // Every new page must read as zero: alloc_page returns whatever the frame
    // last held, and the program can read it before writing.
    unsigned long nonzero = 0;
    for (unsigned long i = 0; i < BRK_TEST_BYTES; i += 4096) {
        const unsigned char* page = (const unsigned char*)base + i;
        for (unsigned long j = 0; j < 4096; j++)
            if (page[j] != 0) { nonzero++; break; }
    }
    check(nonzero == 0, "brk pages are zeroed before they reach userland");
    if (nonzero) printf("memtest: %lu grown page(s) held stale bytes\n", nonzero);

    check(grv_brk(base) == base, "shrink brk back");
    long free_shrunk = mem_free();
    check(free_before - free_shrunk <= 8,
          "shrinking brk returns the frames (only page tables remain)");

    // Regrow the same range. If shrink had left the pages mapped, this would
    // replace live PTEs and leak another 256 frames.
    check(grv_brk(grown) == grown, "regrow brk by 1 MiB");
    long free_regrown = mem_free();
    used = free_before - free_regrown;
    check(used >= (long)BRK_TEST_PAGES && used <= (long)BRK_TEST_PAGES + 8,
          "regrowing brk does not leak the first set of frames");
    check(grv_brk(base) == base, "shrink brk again");

    // --- (c) framebuffer mapping on task teardown ---
    long free_child_before = mem_free();
    const char* cargv[2] = { "memtest.elf", "fbmap" };
    long child = grv_spawn_bg("memtest.elf", cargv, 2);
    check(child > 0, "spawn the framebuffer child");
    if (child > 0) {
        check(grv_wait(child) == 0, "wait for the framebuffer child");
        long free_child_after = mem_free();
        long delta = free_child_after - free_child_before;
        if (delta < 0) delta = -delta;
        check(delta <= 16,
              "a task mapping and unmapping the framebuffer leaves the frame count alone");
        printf("memtest: framebuffer teardown delta %ld frames\n", delta);
    }

    if (fails == 0) {
        printf("memtest: all checks pass\n");
        return 0;
    }
    printf("memtest: %d check(s) FAILED\n", fails);
    return 1;
}
