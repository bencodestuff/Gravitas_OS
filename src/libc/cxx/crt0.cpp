// crt0 — the real userland entry point.
// The kernel enters here with (argc, argv) in RDI/RSI (System V).
// crt0 walks global constructors, calls main(), and exits with
// main's return value. Programs no longer define _start themselves.
// main is a regular C++ main (mangled) — programs just write
// int main(int argc, char** argv).

#include <stddef.h>
#include <stdint.h>

#include <grv/syscall.h>

int main(int argc, char** argv);

typedef void (*ctor_t)(void);

/* These come from the linker script / default section layout */
extern "C" {
    extern ctor_t __init_array_start[];
    extern ctor_t __init_array_end[];
}

extern "C" __attribute__((noinline))
void _start(uint64_t argc, char** argv) {
    for (ctor_t* c = __init_array_start; c < __init_array_end; c++) {
        (*c)();
    }

    int code = main((int)argc, argv);
    grv_exit(code);
}
