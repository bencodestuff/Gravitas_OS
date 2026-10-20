#include <stdio.h>
#include <errno.h>
#include <grv/syscall.h>

// badptr: proves the syscall boundary validates user ranges.
//
// Every call below hands the kernel a null pointer, an unmapped address, a
// kernel address, or a range that runs off the end of the mapped heap. Each
// must come back -EFAULT: a dereference of any of them would fault in ring 0,
// where the fault contract halts the machine, and a kernel address would let
// ring 3 read or write kernel memory.

static int failures;

static void expect_efault(const char* what, long ret) {
    if (ret == -1 && errno == EFAULT) {
        printf("badptr: %s -> EFAULT\n", what);
        return;
    }
    printf("badptr: %s WRONG (ret=%ld errno=%d)\n", what, ret, errno);
    failures++;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // The kernel half: HHDM, kernel image, page tables. Mapped everywhere, so
    // an unchecked pointer here would move bytes to or from kernel memory.
    const char* kernel = (const char*)0xFFFFFF0000000000ul;
    // A canonical user address that this process has never mapped.
    const char* hole = (const char*)0x0000700000000000ul;

    errno = 0;

    // Buffers: null, unmapped, kernel, and the wrong direction.
    expect_efault("read null", grv_read_fd(0, nullptr, 16));
    expect_efault("read unmapped", grv_read_fd(0, (void*)hole, 16));
    expect_efault("read kernel", grv_read_fd(0, (void*)kernel, 16));
    expect_efault("write kernel", grv_write_fd(1, kernel, 16));
    expect_efault("write unmapped", grv_write_fd(1, hole, 16));

    // Paths: the VFS scans these to the terminator.
    expect_efault("open kernel path", grv_open(kernel, 0));
    expect_efault("open unmapped path", grv_open(hole, 0));
    expect_efault("mkdir null path", grv_mkdir(nullptr));
    expect_efault("unlink unmapped path", grv_unlink(hole));

    // Structure outputs are written straight into the caller's memory.
    struct grv_stat st;
    expect_efault("stat kernel path", grv_stat(kernel, &st));
    expect_efault("stat bad output", grv_stat("/", (struct grv_stat*)kernel));
    expect_efault("stat unmapped output", grv_stat("/", (struct grv_stat*)hole));
    expect_efault("fstat bad output", grv_fstat(0, (struct grv_stat*)kernel));

    expect_efault("statfs bad output", grv_statfs("/", (struct grv_statfs*)kernel));

    // Arrays: 264-byte records, and a count large enough to overflow a
    // length calculation if it were not bounded.
    struct grv_dirent de;
    expect_efault("readdir bad array", grv_readdir("/", (struct grv_dirent*)kernel, 4));
    expect_efault("readdir huge count", grv_readdir("/", &de, 0x7FFFFFFFFFFFFFFFL));

    // Strings and argv.
    // getcwd returns the raw result (its wrapper sets no errno), so the
    // rejection is the negated code itself.
    long gc = syscall2(GRV_SYSCALL_GETCWD, 0, 64);
    if (gc == -(long)EFAULT) {
        printf("badptr: getcwd null -> EFAULT\n");
    } else {
        printf("badptr: getcwd null WRONG (ret=%ld)\n", gc);
        failures++;
    }
    expect_efault("spawn kernel name", grv_spawn(kernel, nullptr, 0));
    expect_efault("spawn unmapped argv", grv_spawn("crash.elf", (const char* const*)hole, 1));
    expect_efault("send bad message", grv_send(1, (struct grv_msg*)kernel));
    expect_efault("recv bad message", grv_recv((struct grv_msg*)hole, 0));

    // A pipe writes two ints.
    expect_efault("pipe unmapped", grv_pipe((int*)hole));

    // Geometry outputs (raw calls: these do not set errno).
    if ((long)syscall3(GRV_SYSCALL_SURFACE_CREATE, 64, 64,
                       (long)kernel) != -(long)EFAULT) {
        printf("badptr: surface_create bad output WRONG\n");
        failures++;
    } else {
        printf("badptr: surface_create bad output -> EFAULT\n");
    }
    if (syscall1(GRV_SYSCALL_FBINFO, (long)hole) != -(long)EFAULT) {
        printf("badptr: fbinfo bad output WRONG\n");
        failures++;
    } else {
        printf("badptr: fbinfo bad output -> EFAULT\n");
    }

    // A range that starts mapped and runs into the next, unmapped page: the
    // page-by-page check has to see the second page too. Grow the break to a
    // page boundary plus exactly one page, so 'top' is the first address that
    // is not mapped. Nothing between the calls allocates.
    char* aligned = (char*)(((unsigned long)grv_brk(nullptr) + 4095ul) & ~4095ul);
    char* top = (char*)grv_brk(aligned + 4096);
    if (top != aligned + 4096) {
        printf("badptr: brk grew to the wrong place\n");
        failures++;
    } else {
        expect_efault("range crossing the mapped top", grv_write_fd(1, top - 8, 16));
    }

    if (failures == 0) {
        printf("badptr: all checks pass\n");
        return 0;
    }
    printf("badptr: %d checks FAILED\n", failures);
    return 1;
}
