// klib implementation — see klib.h.
//
// kprint's output channel is the full console: serial (host terminal)
// plus Console::print (the kernel's text screen model). klib owns the
// voice; Console owns the text; Compositor owns the pixels, and the
// flush after each spoken string is what makes text appear promptly.

#include "klib.h"

#include "kernel/io.h"

namespace Console {
    void print(const char* str); // gfx's text model
}
namespace Compositor {
    void present(); // gfx's damage flush
}

extern "C" void kprint(const char* str) {
    // Atomic: with interrupts off no other task can inject its output
    // between our characters (kprint is also called from IRQ/fault context,
    // so save and restore the flag rather than a bare sti).
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    for (int i = 0; str[i] != '\0'; i++) {
        outb(0x3f8, str[i]);    // host terminal (serial)
    }
    Console::print(str);        // kernel text screen (cells)
    Compositor::present();      // repaint whatever changed
    if (flags & (1ull << 9)) asm volatile("sti");
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
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        uint64_t n = (value >> (60 - i * 4)) & 0xF;
        buf[2 + i] = (n < 10) ? (char)('0' + n) : (char)('A' + n - 10);
    }
    buf[18] = '\0';
    kprint(buf);
}

// 'a' + 'b' + 'c' as one atomic write (see klib.h).
extern "C" void kprint3(const char* a, const char* b, const char* c) {
    char buf[192];
    int n = 0;
    const char* parts[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        for (const char* p = parts[i]; p != nullptr && *p != '\0' &&
                                 n < (int)sizeof(buf) - 1; p++)
            buf[n++] = *p;
    }
    buf[n] = '\0';
    kprint(buf);
}

// "label" + a decimal number + "suffix", as one atomic write.
extern "C" void kprint_lus(const char* label, uint64_t value, const char* suffix) {
    char buf[160];
    int n = 0;
    for (const char* p = label; *p != '\0' && n < (int)sizeof(buf) - 32; p++)
        buf[n++] = *p;

    char digits[24];
    int d = 0;
    if (value == 0) digits[d++] = '0';
    while (value > 0) { digits[d++] = (char)('0' + value % 10); value /= 10; }
    while (d > 0 && n < (int)sizeof(buf) - 2) buf[n++] = digits[--d];

    for (const char* p = suffix; *p != '\0' && n < (int)sizeof(buf) - 1; p++)
        buf[n++] = *p;
    buf[n] = '\0';
    kprint(buf);
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
