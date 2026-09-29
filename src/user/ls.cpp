#include <stdint.h>

// ls — lists the files in the tarfs.
// A real standalone userland program: spawned by the shell via the
// spawn syscall, runs in ring 3, and exits when done.

static void sys_putc(char c) {
    asm volatile(
        "mov $0, %%rax\n"
        "mov %0, %%rdi\n"
        "int $0x80\n"
        : : "r"((uint64_t)c) : "rax", "rdi"
    );
}

static void print(const char* s) {
    while (*s) sys_putc(*s++);
}

// list files: rax=2, rdi=buf, rsi=max
static uint64_t sys_list(char* buf, uint64_t max) {
    uint64_t ret;
    asm volatile(
        "int $0x80\n"
        : "=a"(ret)
        : "a"(2), "D"(buf), "S"(max)
        : "memory"
    );
    return ret;
}

// exit: rax=5 — never returns
static void sys_exit() {
    asm volatile(
        "mov $5, %%rax\n"
        "int $0x80\n"
        : : : "memory"
    );
}

extern "C" void _start() {
    static char listbuf[2048];
    sys_list(listbuf, sizeof(listbuf));

    print("tarfs contents:\n");
    print(listbuf);
    print("\n");

    sys_exit();
}
