// Freestanding runtime support: the compiler is ALLOWED to emit calls to
// these four functions at any time (loop idioms, {0} initializers, struct
// copies) even with -fno-builtin. A freestanding environment must provide
// them. Kernel-local namespace versions (elf.cpp etc.) stay untouched.

#include <stddef.h>
#include <stdint.h>

// kprint_hex prints through the kernel console (kprint lives in
// framebuffer.cpp, kprint_uint in main.cpp)
extern "C" void kprint(const char* str);

extern "C" {

void* memset(void* dest, int value, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    uint8_t v = (uint8_t)value;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dest;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

// Hex printer shared by the fault reporter and the task debugger.
// (kprint_uint is decimal-only; faults and CR3s want hex.)
void kprint_hex(uint64_t value) {
    kprint("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint64_t n = (value >> i) & 0xF;
        char c = (n < 10) ? '0' + n : 'A' + (n - 10);
        char s[2] = {c, '\0'};
        kprint(s);
    }
}

} // extern "C"