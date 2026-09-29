#include <stdint.h>

// cat — prints a file from the tarfs.
// Usage: cat <file>   (argv[1] = file name)
// A standalone userland program: spawned by the shell with argv,
// runs in ring 3, exits when done.

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

// read file: rax=3, rdi=name, rsi=buf, rdx=max -> rax = bytes (or -1)
static uint64_t sys_read_file(const char* name, char* buf, uint64_t max) {
    uint64_t ret;
    asm volatile(
        "int $0x80\n"
        : "=a"(ret)
        : "a"(3), "D"(name), "S"(buf), "d"(max)
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

extern "C" void _start(uint64_t argc, char** argv) {
    if (argc < 2) {
        print("cat: missing file name\n");
        sys_exit();
    }

    static char filebuf[4096];
    uint64_t n = sys_read_file(argv[1], filebuf, sizeof(filebuf));
    if (n == (uint64_t)-1) {
        print("cat: no such file\n");
        sys_exit();
    }

    for (uint64_t i = 0; i < n; i++) sys_putc(filebuf[i]);
    sys_putc('\n');

    sys_exit();
}
