// crt0 for third-party C programs (Lua and friends): the same startup as
// crt0.cpp, but it calls an unmangled C main. C++ programs get crt0.cpp, whose
// main is mangled.
//
// The kernel enters here with (argc, argv) in RDI/RSI (System V). Walk global
// constructors, call main(), and exit with its return value.

#include <stddef.h>
#include <stdint.h>

#include <grv/syscall.h>

extern "C" int main(int argc, char** argv);

typedef void (*ctor_t)(void);

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
