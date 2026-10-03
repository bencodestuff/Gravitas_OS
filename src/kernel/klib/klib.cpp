// klib implementation — see klib.h.
//
// kprint's output channel is the full console: serial (host terminal)
// plus Framebuffer::screen_print (the visible framebuffer). When M5
// decouples the console, only this function changes shape.

#include "klib.h"

#include "kernel/io.h"

namespace Framebuffer {
    void screen_print(const char* str); // gfx's text renderer
}

extern "C" void kprint(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) {
        outb(0x3f8, str[i]);    // host terminal (serial)
    }
    Framebuffer::screen_print(str); // QEMU window (screen)
}

extern "C" void kprint_uint(uint64_t value) {
    if (value == 0) {
        kprint("0");
        return;
    }
    char buffer[21]; // max u64 = 20 digits + NUL
    int pos = 20;
    buffer[pos] = '\0';
    while (value > 0) {
        buffer[--pos] = (value % 10) + '0';
        value /= 10;
    }
    kprint(&buffer[pos]);
}

extern "C" void kprint_hex(uint64_t value) {
    kprint("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint64_t n = (value >> i) & 0xF;
        char c = (n < 10) ? '0' + n : 'A' + (n - 10);
        char s[2] = {c, '\0'};
        kprint(s);
    }
}

// Freestanding compiler requirements (clang emits calls to these for
// loop idioms, {0} initializers and struct copies even under -fno-builtin).

extern "C" void* memset(void* dest, int value, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    uint8_t v = (uint8_t)value;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dest;
}

extern "C" void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

extern "C" void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dest;
}

extern "C" int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

extern "C" char* strcpy(char* dest, const char* src) {
    char* ret = dest;
    while ((*dest++ = *src++) != '\0') {
        // copy
    }
    return ret;
}
