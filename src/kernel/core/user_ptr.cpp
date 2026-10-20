#include "kernel/core/user_ptr.h"
#include "kernel/core/vmm.h"

// See user_ptr.h for the contract. The walk is VMM's: one leaf lookup per
// page, in the address space that is active (the calling task's).

namespace UserPtr {

    static bool page_ok(uint64_t va, bool write) {
        uint64_t pte = VMM::leaf_pte_in(VMM::current_pml4(), va);
        if (!(pte & VMM::PTE_PRESENT) || !(pte & VMM::PTE_USER)) return false;
        if (write && !(pte & VMM::PTE_WRITABLE)) return false;
        return true;
    }

    bool valid(uint64_t ptr, uint64_t len, bool write) {
        if (len == 0) return true;
        if (ptr == 0 || ptr >= USER_TOP) return false;
        if (len > USER_TOP - ptr) return false;   // wraps, or runs into the kernel half

        uint64_t last = ptr + len - 1;
        for (uint64_t va = ptr & ~0xFFFull; va <= last; va += 4096) {
            if (!page_ok(va, write)) return false;
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
