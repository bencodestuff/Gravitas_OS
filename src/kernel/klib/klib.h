#pragma once
#include <stddef.h>
#include <stdint.h>

// klib — the kernel's own tiny support library.
//   kprint       : one console voice (serial + the Console text screen)
//   kprint_uint  : decimal printer (no heap, no varargs)
//   kprint_hex   : hex printer (0x-prefixed, uppercase digits)
//   memset/memcpy/memmove/memcmp : freestanding compiler requirements —
//                  clang may emit calls to these at any time even with
//                  -fno-builtin, so a freestanding kernel must provide them
//
// Deliberately NOT here: string helpers used by subsystems (each module
// keeps its own minimal strlen/strcpy, local to what needs it) and any
// allocation — that's core/heap.

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value);
extern "C" void kprint_hex(uint64_t value);

// The two below emit a whole line in ONE atomic write. Multi-call printing is
// not atomic across calls — interrupts are on between them — so a task could
// inject its output into the middle of a boot line (it did).
extern "C" void kprint3(const char* a, const char* b, const char* c);
extern "C" void kprint_lus(const char* label, uint64_t value, const char* suffix);

extern "C" char* strcpy(char* dest, const char* src); // klib

// The freestanding memory functions the compiler may call at any time.
extern "C" void* memset(void* dest, int value, size_t n);
extern "C" void* memcpy(void* dest, const void* src, size_t n);
extern "C" void* memmove(void* dest, const void* src, size_t n);
extern "C" int   memcmp(const void* a, const void* b, size_t n);