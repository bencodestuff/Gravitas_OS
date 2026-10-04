#include "kernel/core/vmm.h"
#include "kernel/klib/klib.h"
#include "kernel/core/pmm.h"
#include <limine.h>


// --- EXTERNAL REQUESTS ---
extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_kernel_address_request kernel_address_request;

namespace VMM {
    // --- Flags ---
    constexpr uint64_t PTE_PRESENT = 1;
    constexpr uint64_t PTE_WRITABLE = 2;
    constexpr uint64_t PTE_USER = 4;
    constexpr uint64_t PTE_NO_CACHE = 0x10;
    // Software-only bit (52..62 are free in a non-present PTE): marks a VA
    // as reserved with no backing frame. range_free() treats it as used.
    constexpr uint64_t PTE_GUARD = 1ull << 62;

    static uint64_t* kernel_pml4 = nullptr;
    static uint64_t hhdm_offset = 0;

    // --- Helpers ---
    static void memset(void* ptr, int value, uint64_t size) {
        uint8_t* p = (uint8_t*)ptr;
        for (uint64_t i = 0; i < size; i++) p[i] = value;
    }

    uint64_t current_pml4() {
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        return cr3;
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

    bool is_mapped(uint64_t virtual_addr) {
        uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
        uint64_t pdp_idx  = (virtual_addr >> 30) & 0x1FF;
        uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
        uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

        uint64_t* pt = get_next_level_in(current_pml4(), pml4_idx, pdp_idx, pd_idx, false, false);
        if (!pt) return false;
        return (pt[pt_idx] & PTE_PRESENT) != 0;
    }

    void map_page_in(uint64_t pml4_phys, uint64_t physical_addr,
                     uint64_t virtual_addr, uint64_t flags) {
        uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
        uint64_t pdp_idx  = (virtual_addr >> 30) & 0x1FF;
        uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
        uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

        bool user = (flags & PTE_USER) != 0;

        uint64_t* pt = get_next_level_in(pml4_phys, pml4_idx, pdp_idx, pd_idx, true, user);
        if (!pt) return;

        // Apply flags to the final page
        uint64_t phys_frame = physical_addr & 0x000FFFFFFFFFF000;
        pt[pt_idx] = phys_frame | (flags & 0xFFF);

        // Flush TLB for this specific address immediately (harmless if the
        // address belongs to a space that isn't currently active)
        asm volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }

    void map_page(uint64_t physical_addr, uint64_t virtual_addr, uint64_t flags) {
        map_page_in(current_pml4(), physical_addr, virtual_addr, flags);
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

    // --- User VA allocator (walk-the-tables, no metadata) ---
    // Region: 256MB .. 1GB. ELF code loads at 0x400000 below it; the
    // kernel's upper half is above.
    constexpr uint64_t USER_VA_BASE = 0x10000000; // 256MB
    constexpr uint64_t USER_VA_END  = 0x40000000; // 1GB

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

    // Claim a VA with a software guard PTE (no backing frame). The range
    // becomes invisible to range_free() until the caller maps over it —
    // structurally safe, and unlike the old scheme it burns no frame that
    // the caller would then overwrite and leak.
    static bool reserve_guard(uint64_t pml4_phys, uint64_t va, bool user) {
        uint64_t pml4_idx = (va >> 39) & 0x1FF;
        uint64_t pdp_idx  = (va >> 30) & 0x1FF;
        uint64_t pd_idx   = (va >> 21) & 0x1FF;
        uint64_t pt_idx   = (va >> 12) & 0x1FF;

        uint64_t* pt = get_next_level_in(pml4_phys, pml4_idx, pdp_idx, pd_idx, true, user);
        if (pt == nullptr) return false;
        pt[pt_idx] = PTE_GUARD;
        return true;
    }

    uint64_t alloc_user_range(uint64_t pml4_phys, uint64_t pages) {
        if (pages == 0) return 0;
        uint64_t candidate = USER_VA_BASE;
        while (candidate + pages * 4096 <= USER_VA_END) {
            if (range_free(pml4_phys, candidate, pages)) {
                // Guard on the FIRST page only: one bit is enough to make
                // the whole scanned range non-free.
                if (!reserve_guard(pml4_phys, candidate, true)) return 0;
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

    // Recursively free a table page and everything below it (tables + frames).
    static void free_space_level(uint64_t table_phys, int level) {
        uint64_t* table = (uint64_t*)(table_phys + hhdm_offset);
        for (int i = 0; i < 512; i++) {
            uint64_t entry = table[i];
            if (!(entry & PTE_PRESENT)) continue;
            uint64_t next_phys = entry & 0x000FFFFFFFFFF000;

            if (level < 4) {
                // Intermediate table: recurse, then free the table itself
                free_space_level(next_phys, level + 1);
            }
            // Leaf level (4): entries are data frames — free them
            PMM::free_page(next_phys);
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