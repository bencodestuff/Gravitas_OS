#include "kernel/core/user_ptr.h"
#include "kernel/core/vmm.h"

// See user_ptr.h for the contract. The walk is VMM's: one leaf lookup per
// page, in the address space that is active (the calling task's).

namespace UserPtr {

    static bool page_ok(uint64_t va, bool write) {
        uint64_t pte = VMM::leaf_pte_in(VMM::current_pml4(), va);
        if (!(pte & VMM::PTE_PRESENT) || !(pte & VMM::PTE_USER)) return false;
        if (!write) return true;
        if (pte & VMM::PTE_WRITABLE) return true;
        // A fork write-protects every writable page it copies and marks it
        // copy-on-write, in the parent as well as the child. The bytes behind
        // such a page are still this task's to change: the first store makes
        // the page private. A read-only page that was never writable is not
        // marked, and stays refused.
        return (pte & VMM::PTE_COW) && !(pte & VMM::PTE_SHARED);
    }

    bool valid(uint64_t ptr, uint64_t len, bool write) {
        if (len == 0) return true;
        if (ptr == 0 || ptr >= USER_TOP) return false;
        if (len > USER_TOP - ptr) return false;   // wraps, or runs into the kernel half

        uint64_t last = ptr + len - 1;
        uint64_t first_page = ptr & ~0xFFFull;
        for (uint64_t va = first_page; va <= last; va += 4096) {
            if (!page_ok(va, write)) return false;
        }
        if (!write) return true;

        // An output buffer is written by plain kernel stores, and a store to a
        // copy-on-write page faults with the user bit clear (the store comes
        // from ring 0), where the fault contract cannot resolve it: it would
        // dump and halt. So make every page in the range private now, after
        // the whole range has passed as writable-origin. The active PML4 is the
        // caller's during a syscall, and resolve_cow_fault invalidates the TLB
        // entry it changes for exactly that space. Running out of memory here
        // fails the range before anything has been written.
        uint64_t pml4 = VMM::current_pml4();
        for (uint64_t va = first_page; va <= last; va += 4096) {
            uint64_t pte = VMM::leaf_pte_in(pml4, va);
            if (!(pte & VMM::PTE_WRITABLE) &&
                !VMM::resolve_cow_fault(pml4, va))
                return false;
        }
        return true;
    }

    // Scan bytes, checking each page once as it is entered. The byte itself is
    // only read after its page passed page_ok(), so an unmapped address is
    // never dereferenced.
    static bool scan(uint64_t ptr, uint64_t max, bool need_nul) {
        if (ptr == 0 || ptr >= USER_TOP) return false;
        for (uint64_t i = 0; i < max; i++) {
            uint64_t addr = ptr + i;
            if (addr >= USER_TOP) return false;   // ran into the kernel half
            if (i == 0 || (addr & 0xFFF) == 0) {
                if (!page_ok(addr, false)) return false;
            }
            if (*(const char*)(uintptr_t)addr == '\0') return true;
        }
        // Every byte within the limit is mapped. A path needs the terminator
        // inside it; a truncating read is content to stop at the limit.
        return !need_nul;
    }

    bool valid_string(uint64_t ptr, uint64_t max) {
        return scan(ptr, max, true);
    }

    bool valid_prefix(uint64_t ptr, uint64_t max) {
        return scan(ptr, max, false);
    }
}
