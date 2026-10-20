#pragma once
#include <stdint.h>

#include "kernel/core/vmm.h"

// user_ptr.h: validation of pointer arguments at the syscall boundary.
//
// Ring 3 hands the kernel raw addresses and lengths. Nothing may dereference
// one before checking it: an unmapped or too-short buffer faults in ring 0,
// where the fault contract halts the machine, and the kernel half (HHDM,
// kernel image, page tables) is mapped in every address space, so an unchecked
// address above the split would silently move bytes to or from kernel memory.
//
// Every check walks the CURRENT address space's page tables, which during a
// syscall is the caller's, and covers every page the range touches. These
// helpers are for the boundary only: a kernel-internal caller passing its own
// (upper-half) buffer must not go through them.

namespace UserPtr {
    // The user/kernel split: user addresses live below this, the kernel's
    // upper half above it. One definition, in the VMM.
    constexpr uint64_t USER_TOP = VMM::USER_VA_LIMIT;

    // [ptr, ptr + len) is entirely user memory. 'write' asks for a writable
    // leaf (an output buffer); otherwise read access is enough. Null, an
    // address at or above the split, and a range that wraps or crosses the
    // split are all rejected. A zero length touches nothing, so it is
    // accepted (read/write with a count of 0).
    bool valid(uint64_t ptr, uint64_t len, bool write);

    // A NUL-terminated string whose terminator lies within 'max' bytes: what
    // a path argument must be, so the VFS's own scans stay inside the range.
    bool valid_string(uint64_t ptr, uint64_t max);

    // The first 'max' bytes of a string, or up to its terminator. For names
    // the kernel truncates: true once every byte it could read is mapped,
    // whether or not a terminator arrived inside the limit.
    bool valid_prefix(uint64_t ptr, uint64_t max);
}
