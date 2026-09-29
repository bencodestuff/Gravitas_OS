/* C++ runtime support for freestanding Gravitas userland.
   No libstdc++, no exceptions, no RTTI — just the pieces clang
   emits calls to: operator new/delete, local-static guards. */

#include <stdlib.h>
#include <stddef.h>

#include <grv/syscall.h>

// Provided by crt0.c after walking .init_array
extern "C" void __cxa_finalize(void* dso);

/* --- operator new/delete: thin wrappers over malloc --- */
void* operator new(size_t size)   { return malloc(size); }
void* operator new[](size_t size) { return malloc(size); }
void operator delete(void* p) noexcept   { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, unsigned long) noexcept   { free(p); }
void operator delete[](void* p, unsigned long) noexcept { free(p); }

extern "C" {

/* --- Local static guards (single-threaded: always "not in progress") --- */
int __cxa_guard_acquire(long long* guard) {
    // bit 0 = initialized, bit 8 = in progress
    if (*guard & 1) return 0;
    *guard |= 0x100;
    return 1;
}
void __cxa_guard_release(long long* guard) {
    *guard = 1;
}
void __cxa_guard_abort(long long* guard) {
    *guard &= ~0x100ll;
}

/* --- Exception personality: never called (we compile -fno-exceptions),
       but libgcc references it in some code paths --- */
void __cxa_pure_virtual() {
    abort();
}

} // extern "C"
