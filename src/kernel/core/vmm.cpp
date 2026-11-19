#include "kernel/core/vmm.h"
#include "kernel/klib/klib.h"
#include "kernel/core/pmm.h"
#include <limine.h>


// --- EXTERNAL REQUESTS ---
extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_kernel_address_request kernel_address_request;

namespace VMM {
    // --- Flags ---
    // PTE_* and PTE_SHARED live in vmm.h: the mapping interface (memmap.cpp)
    // needs them too.
    // Software-only bit (52..62 are free in a non-present PTE): marks a VA
    // as reserved with no backing frame. range_free() treats it as used.
    constexpr uint64_t PTE_GUARD = 1ull << 62;

    static uint64_t* kernel_pml4 = nullptr;
    static uint64_t hhdm_offset = 0;
    constexpr uint64_t USER_VA_END = 0x40000000;

    // --- Helpers ---
    static void memset(void* ptr, int value, uint64_t size) {
        uint8_t* p = (uint8_t*)ptr;
        for (uint64_t i = 0; i < size; i++) p[i] = value;
    }

    uint64_t current_pml4() {
#ifdef VMM_HOST_TEST
        return 0;
#else
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        return cr3;
#endif
    }

    // THE FIXED FUNCTION — walks an explicit PML4 (physical address),
    // allocating intermediate tables in that space when asked.
    static uint64_t* get_next_level_in(uint64_t pml4_phys, uint64_t pml4_idx,
                                       uint64_t pdp_idx, uint64_t pd_idx,
                                       bool allocate, bool user) {
        uint64_t* pml4 = (uint64_t*)(pml4_phys + hhdm_offset);
        uint64_t entry = pml4[pml4_idx];
        if (!(entry & PTE_PRESENT)) {
            if (!allocate) return nullptr;
            uint64_t new_table_phys = PMM::alloc_page();
            if (new_table_phys == 0) return nullptr;
            uint64_t* new_table_virt = (uint64_t*)(new_table_phys + hhdm_offset);
            memset(new_table_virt, 0, 4096);
            uint64_t entry_flags = PTE_PRESENT | PTE_WRITABLE;
            if (user) entry_flags |= PTE_USER;
            pml4[pml4_idx] = new_table_phys | entry_flags;
            entry = pml4[pml4_idx];
        }
        // Upgrade permissions if a user page shares this table path
        if (user && !(entry & PTE_USER)) {
            pml4[pml4_idx] = entry | PTE_USER;
            entry = pml4[pml4_idx];
        }
        uint64_t* pdp = (uint64_t*)((entry & 0x000FFFFFFFFFF000) + hhdm_offset);

        entry = pdp[pdp_idx];
        if (!(entry & PTE_PRESENT)) {
            if (!allocate) return nullptr;
            uint64_t new_table_phys = PMM::alloc_page();
            if (new_table_phys == 0) return nullptr;
            uint64_t* new_table_virt = (uint64_t*)(new_table_phys + hhdm_offset);
            memset(new_table_virt, 0, 4096);
            uint64_t entry_flags = PTE_PRESENT | PTE_WRITABLE;
            if (user) entry_flags |= PTE_USER;
            pdp[pdp_idx] = new_table_phys | entry_flags;
            entry = pdp[pdp_idx];
        }
        if (user && !(entry & PTE_USER)) {
            pdp[pdp_idx] = entry | PTE_USER;
            entry = pdp[pdp_idx];
        }
        uint64_t* pd = (uint64_t*)((entry & 0x000FFFFFFFFFF000) + hhdm_offset);

        entry = pd[pd_idx];
        if (!(entry & PTE_PRESENT)) {
            if (!allocate) return nullptr;
            uint64_t new_table_phys = PMM::alloc_page();
            if (new_table_phys == 0) return nullptr;
            uint64_t* new_table_virt = (uint64_t*)(new_table_phys + hhdm_offset);
            memset(new_table_virt, 0, 4096);
            uint64_t entry_flags = PTE_PRESENT | PTE_WRITABLE;
            if (user) entry_flags |= PTE_USER;
            pd[pd_idx] = new_table_phys | entry_flags;
            entry = pd[pd_idx];
        }
        if (user && !(entry & PTE_USER)) {
            pd[pd_idx] = entry | PTE_USER;
            entry = pd[pd_idx];
        }
        return (uint64_t*)((entry & 0x000FFFFFFFFFF000) + hhdm_offset);
    }

    uint64_t phys_of(const void* va) {
        return (uint64_t)(uintptr_t)va - hhdm_offset;
    }

    void* phys_to_kernel(uint64_t phys) {
        return (void*)(uintptr_t)(phys + hhdm_offset);
    }

    uint64_t leaf_pte_in(uint64_t pml4_phys, uint64_t virtual_addr) {
        uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
        uint64_t pdp_idx  = (virtual_addr >> 30) & 0x1FF;
        uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
        uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

        uint64_t* pt = get_next_level_in(pml4_phys, pml4_idx, pdp_idx, pd_idx, false, false);
        if (!pt) return 0;
        return pt[pt_idx];
    }

    bool is_mapped(uint64_t virtual_addr) {
        return (leaf_pte_in(current_pml4(), virtual_addr) & PTE_PRESENT) != 0;
    }

    bool map_page_in(uint64_t pml4_phys, uint64_t physical_addr,
                     uint64_t virtual_addr, uint64_t flags) {
        uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
        uint64_t pdp_idx  = (virtual_addr >> 30) & 0x1FF;
        uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
        uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

        bool user = (flags & PTE_USER) != 0;

        uint64_t* pt = get_next_level_in(pml4_phys, pml4_idx, pdp_idx, pd_idx, true, user);
        if (!pt) return false;

        // Refuse to replace a live leaf. Silently overwriting it orphans the
        // old frame (nothing records it) and destroys whatever the mapping
        // held, so the caller must learn that the address was taken. A guard
        // PTE is deliberately not present and is meant to be mapped over.
        if (pt[pt_idx] & PTE_PRESENT) return false;

        // Apply flags to the final page
        uint64_t phys_frame = physical_addr & 0x000FFFFFFFFFF000;
        pt[pt_idx] = phys_frame | (flags & 0xFFF);

        // Flush TLB for this specific address immediately (harmless if the
        // address belongs to a space that isn't currently active)
        if (pml4_phys == current_pml4())
            asm volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
        return true;
    }

    bool map_page(uint64_t physical_addr, uint64_t virtual_addr, uint64_t flags) {
        return map_page_in(current_pml4(), physical_addr, virtual_addr, flags);
    }

    void init(struct limine_memmap_response* map) {
        kprint("[VMM] Initializing...\n");

        if (hhdm_request.response == nullptr || kernel_address_request.response == nullptr) {
             kprint("CRITICAL: Requests missing!\n");
             for(;;) asm("hlt");
        }
        hhdm_offset = hhdm_request.response->offset;

        uint64_t pml4_phys = PMM::alloc_page();
        kernel_pml4 = (uint64_t*)(pml4_phys + hhdm_offset);
        memset(kernel_pml4, 0, 4096);

        // 1. Map Kernel
        uint64_t kernel_phys_base = kernel_address_request.response->physical_base;
        uint64_t kernel_virt_base = kernel_address_request.response->virtual_base;
        
        for (uint64_t i = 0; i < map->entry_count; i++) {
            struct limine_memmap_entry* entry = map->entries[i];
            if (entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
                uint64_t offset = entry->base - kernel_phys_base;
                uint64_t virt_start = kernel_virt_base + offset;
                for (uint64_t j = 0; j < entry->length; j += 4096) {
                    map_page_in(pml4_phys, entry->base + j, virt_start + j, PTE_PRESENT | PTE_WRITABLE);
                }
            }
        }

        // 2. Map HHDM (Physical Memory)
        for (uint64_t i = 0; i < map->entry_count; i++) {
            struct limine_memmap_entry* entry = map->entries[i];
            uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
            if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) flags |= PTE_NO_CACHE;

            for (uint64_t j = 0; j < entry->length; j += 4096) {
                map_page_in(pml4_phys, entry->base + j, entry->base + j + hhdm_offset, flags);
            }
        }

        switch_pml4(pml4_phys);
        kprint("[VMM] Initialized.\n");
    }

    void switch_pml4(uint64_t pml4_phys_addr) {
        asm volatile("mov %0, %%cr3" : : "r"(pml4_phys_addr) : "memory");
    }
    uint64_t get_kernel_pml4_phys() {
        return (uint64_t)kernel_pml4 - hhdm_offset;
    }

    uint64_t clone_kernel_space() {
        uint64_t new_pml4_phys = PMM::alloc_page();
        if (new_pml4_phys == 0) return 0; // caller reports ENOMEM
        uint64_t* new_pml4 = (uint64_t*)(new_pml4_phys + hhdm_offset);
        memset(new_pml4, 0, 4096);

        // Share the kernel's upper half (entries 256..511). Copying the
        // top-level entries shares the entire kernel subtree, so kernel
        // mappings made later are visible in every address space.
        // The user half (0..255) stays zero: each process builds its own.
        for (int i = 256; i < 512; i++) {
            new_pml4[i] = kernel_pml4[i];
        }
        return new_pml4_phys;
    }

    uint64_t clone_user_space(uint64_t source_pml4) {
        uint64_t child = clone_kernel_space();
        if (!child) return 0;
        // User allocations and ELF mappings are below 1 GiB in this kernel.
        // Copy guard reservations as well as leaves, so child VA allocation
        // cannot overlap the parent's outstanding reservations.
        for (uint64_t va = 0; va < USER_VA_END; va += 4096) {
            uint64_t pte = leaf_pte_in(source_pml4, va);
            if (pte & PTE_GUARD) {
                uint64_t* pt = get_next_level_in(child, (va >> 39) & 0x1ff,
                    (va >> 30) & 0x1ff, (va >> 21) & 0x1ff, true, true);
                if (!pt) { destroy_user_space(child); return 0; }
                pt[(va >> 12) & 0x1ff] = PTE_GUARD;
                continue;
            }
            if (!(pte & PTE_PRESENT)) continue;
            uint64_t child_flags = pte & 0xFFF;
            if (!(pte & PTE_SHARED) && (pte & PTE_WRITABLE)) {
                if (!PMM::retain_page(pte & 0x000FFFFFFFFFF000ull)) {
                    destroy_user_space(child);
                    return 0;
                }
                child_flags = (child_flags & ~PTE_WRITABLE) | PTE_COW;
            } else if (pte & PTE_COW) {
                if (!PMM::retain_page(pte & 0x000FFFFFFFFFF000ull)) {
                    destroy_user_space(child);
                    return 0;
                }
            }
            if (!map_page_in(child, pte & 0x000FFFFFFFFFF000ull, va, child_flags)) {
                if (!(pte & PTE_SHARED)) PMM::free_page(pte & 0x000FFFFFFFFFF000ull);
                destroy_user_space(child);
                return 0;
            }
            if (!(pte & PTE_SHARED) && (pte & PTE_WRITABLE)) {
                uint64_t* parent_pt = get_next_level_in(source_pml4,
                    (va >> 39) & 0x1ff, (va >> 30) & 0x1ff,
                    (va >> 21) & 0x1ff, false, false);
                parent_pt[(va >> 12) & 0x1ff] = child_flags;
                if (source_pml4 == current_pml4())
                    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
            }
        }
        return child;
    }

    bool resolve_cow_fault(uint64_t pml4_phys, uint64_t address) {
        if (address >= USER_VA_LIMIT) return false;
        uint64_t page = address & ~0xFFFull;
        uint64_t pte = leaf_pte_in(pml4_phys, page);
        if (!(pte & PTE_PRESENT) || !(pte & PTE_USER) ||
            !(pte & PTE_COW) || (pte & PTE_SHARED)) return false;
        uint64_t old_phys = pte & 0x000FFFFFFFFFF000ull;
        uint64_t* pt = get_next_level_in(pml4_phys, (page >> 39) & 0x1ff,
            (page >> 30) & 0x1ff, (page >> 21) & 0x1ff, false, false);
        if (!pt) return false;
        uint64_t idx = (page >> 12) & 0x1ff;
        if (PMM::page_references(old_phys) == 1) {
            pt[idx] = (pte | PTE_WRITABLE) & ~PTE_COW;
            if (pml4_phys == current_pml4())
                asm volatile("invlpg (%0)" :: "r"(page) : "memory");
            return true;
        }
        uint64_t fresh = PMM::alloc_page();
        if (!fresh) return false;
        uint8_t* to = (uint8_t*)phys_to_kernel(fresh);
        const uint8_t* from = (const uint8_t*)phys_to_kernel(old_phys);
        for (uint64_t i = 0; i < 4096; i++) to[i] = from[i];
        pt[idx] = fresh | ((pte | PTE_WRITABLE) & 0xFFF & ~PTE_COW);
        PMM::free_page(old_phys);
        if (pml4_phys == current_pml4())
            asm volatile("invlpg (%0)" :: "r"(page) : "memory");
        return true;
    }

    bool copy_to_user(uint64_t pml4_phys, uint64_t destination,
                      const void* source, uint64_t length) {
        if (destination >= USER_VA_LIMIT || length > USER_VA_LIMIT - destination)
            return false;
        const uint8_t* in = (const uint8_t*)source;
        for (uint64_t done = 0; done < length;) {
            uint64_t va = destination + done;
            if (resolve_cow_fault(pml4_phys, va)) {}
            uint64_t pte = leaf_pte_in(pml4_phys, va);
            if (!(pte & PTE_PRESENT) || !(pte & PTE_USER) ||
                !(pte & PTE_WRITABLE)) return false;
            uint64_t offset = va & 0xFFF;
            uint64_t chunk = 4096 - offset;
            if (chunk > length - done) chunk = length - done;
            uint8_t* out = (uint8_t*)phys_to_kernel(
                (pte & 0x000FFFFFFFFFF000ull) + offset);
            for (uint64_t i = 0; i < chunk; i++) out[i] = in[done + i];
            done += chunk;
        }
        return true;
    }

    // --- User VA allocator (walk-the-tables, no metadata) ---
    // Region: 256MB .. 1GB. ELF code loads at 0x400000 below it; the
    // kernel's upper half is above. USER_VA_BASE lives in vmm.h because brk
    // has to stop before the allocator's arena.

    // Is the page range unmapped in this space? (creates nothing)
    static bool range_free(uint64_t pml4_phys, uint64_t vaddr, uint64_t pages) {
        for (uint64_t p = 0; p < pages; p++) {
            uint64_t va = vaddr + p * 4096;
            uint64_t pml4_idx = (va >> 39) & 0x1FF;
            uint64_t pdp_idx  = (va >> 30) & 0x1FF;
            uint64_t pd_idx   = (va >> 21) & 0x1FF;
            uint64_t pt_idx   = (va >> 12) & 0x1FF;

            uint64_t* pt = get_next_level_in(pml4_phys, pml4_idx, pdp_idx, pd_idx, false, false);
            if (pt == nullptr) continue;          // table absent -> free
            if (pt[pt_idx] & (PTE_PRESENT | PTE_GUARD)) return false;
        }
        return true;
    }

    // Take guard markers back out of a range that was never handed out. Only
    // markers are cleared, so this can never drop a real mapping, and it
    // allocates nothing on the way back.
    static void drop_guard(uint64_t pml4_phys, uint64_t va, uint64_t pages) {
        for (uint64_t p = 0; p < pages; p++) {
            uint64_t addr = va + p * 4096;
            uint64_t pml4_idx = (addr >> 39) & 0x1FF;
            uint64_t pdp_idx  = (addr >> 30) & 0x1FF;
            uint64_t pd_idx   = (addr >> 21) & 0x1FF;
            uint64_t pt_idx   = (addr >> 12) & 0x1FF;

            uint64_t* pt = get_next_level_in(pml4_phys, pml4_idx, pdp_idx,
                                             pd_idx, false, false);
            if (pt == nullptr) continue;
            if (pt[pt_idx] & PTE_GUARD) pt[pt_idx] = 0;
        }
    }

    // Claim every page of a VA range with a software guard PTE (no backing
    // frame). range_free() tests pages one at a time, so EVERY page has to
    // carry a marker: guarding only the first would leave the tail looking
    // free and a later reservation could overlap it. The guards are
    // non-present, so map_page_in() replaces them as the caller maps real
    // pages, and the ones that are never mapped stay as reservations.
    // Cost is one page table per 2MB of range, created only where the range
    // reaches a table that does not exist yet, and bounded by the arena: the
    // 768MB user region holds at most 384 of them. Nothing had to be tracked
    // separately, and address-space teardown frees those tables like any
    // other, which releases the reservation with them.
    static bool reserve_guard(uint64_t pml4_phys, uint64_t va, uint64_t pages,
                              bool user) {
        for (uint64_t p = 0; p < pages; p++) {
            uint64_t addr = va + p * 4096;
            uint64_t pml4_idx = (addr >> 39) & 0x1FF;
            uint64_t pdp_idx  = (addr >> 30) & 0x1FF;
            uint64_t pd_idx   = (addr >> 21) & 0x1FF;
            uint64_t pt_idx   = (addr >> 12) & 0x1FF;

            uint64_t* pt = get_next_level_in(pml4_phys, pml4_idx, pdp_idx,
                                             pd_idx, true, user);
            if (pt == nullptr) {
                // Out of table pages part way through: hand back the markers
                // already written, so a failed allocation leaves no
                // reservation behind.
                drop_guard(pml4_phys, va, p);
                return false;
            }
            pt[pt_idx] = PTE_GUARD;
        }
        return true;
    }

    uint64_t alloc_user_range(uint64_t pml4_phys, uint64_t pages) {
        if (pages == 0) return 0;
        uint64_t candidate = USER_VA_BASE;
        while (candidate + pages * 4096 <= USER_VA_END) {
            if (range_free(pml4_phys, candidate, pages)) {
                if (!reserve_guard(pml4_phys, candidate, pages, true))
                    return 0;
                return candidate;
            }
            candidate += 4096;
        }
        return 0; // region exhausted
    }

    void unmap_user_range(uint64_t pml4_phys, uint64_t virtual_addr, uint64_t pages) {
        bool current = (pml4_phys == current_pml4());
        for (uint64_t p = 0; p < pages; p++) {
            uint64_t va = virtual_addr + p * 4096;
            uint64_t pml4_idx = (va >> 39) & 0x1FF;
            uint64_t pdp_idx  = (va >> 30) & 0x1FF;
            uint64_t pd_idx   = (va >> 21) & 0x1FF;
            uint64_t pt_idx   = (va >> 12) & 0x1FF;

            uint64_t* pt = get_next_level_in(pml4_phys, pml4_idx, pdp_idx, pd_idx, false, false);
            if (pt == nullptr) continue;
            if (!(pt[pt_idx] & (PTE_PRESENT | PTE_GUARD))) continue;

            pt[pt_idx] = 0;
            // Only the active space has cached translations to drop; a
            // switch to any other space flushes the TLB wholesale.
            if (current) asm volatile("invlpg (%0)" :: "r"(va) : "memory");
        }
    }

    bool user_range_free(uint64_t pml4_phys, uint64_t virtual_addr,
                         uint64_t pages) {
        return range_free(pml4_phys, virtual_addr, pages);
    }

    void unmap_and_free_user_range(uint64_t pml4_phys, uint64_t virtual_addr,
                                   uint64_t pages) {
        bool current = (pml4_phys == current_pml4());
        for (uint64_t p = 0; p < pages; p++) {
            uint64_t va = virtual_addr + p * 4096;
            uint64_t pml4_idx = (va >> 39) & 0x1FF;
            uint64_t pdp_idx  = (va >> 30) & 0x1FF;
            uint64_t pd_idx   = (va >> 21) & 0x1FF;
            uint64_t pt_idx   = (va >> 12) & 0x1FF;

            uint64_t* pt = get_next_level_in(pml4_phys, pml4_idx, pdp_idx, pd_idx, false, false);
            if (pt == nullptr) continue;
            uint64_t pte = pt[pt_idx];
            if (!(pte & PTE_PRESENT)) continue;

            // Shared frames belong to an image, a region or a device. The
            // task owns everything else it mapped here, so free that.
            if (!(pte & PTE_SHARED))
                PMM::free_page(pte & 0x000FFFFFFFFFF000);
            pt[pt_idx] = 0;
            if (current) asm volatile("invlpg (%0)" :: "r"(va) : "memory");
        }
    }

    // Recursively free a table page and everything below it (tables + frames).
    // Ownership rule: this call owns the table page it is handed and frees it
    // once its entries are done, so a caller that recurses into a child must
    // not free that child's page itself.
    // Frames marked PTE_SHARED belong to someone else (an image, a shared
    // region): the mapping goes, the memory does not.
    static void free_space_level(uint64_t table_phys, int level) {
        uint64_t* table = (uint64_t*)(table_phys + hhdm_offset);
        for (int i = 0; i < 512; i++) {
            uint64_t entry = table[i];
            if (!(entry & PTE_PRESENT)) continue;
            uint64_t next_phys = entry & 0x000FFFFFFFFFF000;

            if (level < 4) {
                // Intermediate table: the callee frees its own page.
                free_space_level(next_phys, level + 1);
            } else if (!(entry & PTE_SHARED)) {
                // Leaf: a data frame this task owns
                PMM::free_page(next_phys);
            }
        }
        PMM::free_page(table_phys);
    }

    void destroy_user_space(uint64_t pml4_phys) {
        if (pml4_phys == get_kernel_pml4_phys()) return; // never nuke the kernel

        uint64_t* pml4 = (uint64_t*)(pml4_phys + hhdm_offset);
        for (int i = 0; i < 256; i++) { // user half only
            uint64_t entry = pml4[i];
            if (!(entry & PTE_PRESENT)) continue;
            free_space_level(entry & 0x000FFFFFFFFFF000, 2);
            pml4[i] = 0;
        }
        PMM::free_page(pml4_phys);
    }
}
