/* C++ runtime support for freestanding Gravitas userland.
   No libstdc++, no exceptions, no RTTI — just the pieces clang
   emits calls to: operator new/delete, local-static guards. */

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#include <grv/syscall.h>

/* --- Stack smash protector ---
   Clang references the global __stack_chk_guard on ELF (not the TLS form).
   crt0 seeds it once with boot-time entropy before main() runs. Any fixed
   nonzero seed is fine — each process lives in its own address space, so
   guards are private and there's nothing to guess across processes. */
extern "C" __attribute__((visibility("hidden")))
uint64_t __stack_chk_guard = 0x00C0FFEE00000000ull;

static void grv_print(const char* s) {
    while (*s) grv_putc(*s++);
}

extern "C" [[noreturn]] void __stack_chk_fail(void) {
    grv_print("\n!! stack smashing detected — aborting\n");
    // Straight to the exit syscall: grv_exit() would truncate the code to
    // int, and the SMASH marker is only meaningful at full width.
    syscall1(GRV_SYSCALL_EXIT, (long)0x534D415348ull /* "SMASH" */);
    __builtin_unreachable();
}

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
