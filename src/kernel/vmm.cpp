#include "vmm.h"
#include "pmm.h"
#include <limine.h>

extern "C" void kprint(const char* str);

// --- EXTERNAL REQUESTS ---
extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_kernel_address_request kernel_address_request;

namespace VMM {
    // --- Flags ---
    constexpr uint64_t PTE_PRESENT = 1;
    constexpr uint64_t PTE_WRITABLE = 2;
    constexpr uint64_t PTE_USER = 4;
    constexpr uint64_t PTE_NO_CACHE = 0x10;

    static uint64_t* kernel_pml4 = nullptr;
    static uint64_t hhdm_offset = 0;

    // --- Helpers ---
    static void memset(void* ptr, int value, uint64_t size) {
        uint8_t* p = (uint8_t*)ptr;
        for (uint64_t i = 0; i < size; i++) p[i] = value;
    }

    // THE FIXED FUNCTION
    static uint64_t* get_next_level(uint64_t* current_level, uint64_t index, bool allocate, bool user) {
        uint64_t entry = current_level[index];
        
        if (entry & PTE_PRESENT) {
            // --- CRITICAL FIX: UPGRADE PERMISSIONS ---
            // If we are mapping a User page, but the existing table is Kernel-only,
            // we MUST add the USER bit. Otherwise, the CPU blocks access at this level.
            if (user && !(entry & PTE_USER)) {
                current_level[index] |= PTE_USER;
            }
            // -----------------------------------------

            uint64_t phys = entry & 0x000FFFFFFFFFF000;
            return (uint64_t*)(phys + hhdm_offset);
        }

        if (!allocate) return nullptr;

        uint64_t new_table_phys = PMM::alloc_page();
        if (new_table_phys == 0) return nullptr;

        uint64_t* new_table_virt = (uint64_t*)(new_table_phys + hhdm_offset);
        memset(new_table_virt, 0, 4096);
        
        // When creating a new level, if 'user' is true, we flag it as USER allowed.
        uint64_t entry_flags = PTE_PRESENT | PTE_WRITABLE;
        if (user) entry_flags |= PTE_USER;
        
        current_level[index] = new_table_phys | entry_flags;
        return new_table_virt;
    }

    bool is_mapped(uint64_t virtual_addr) {
        uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
        uint64_t pdp_idx  = (virtual_addr >> 30) & 0x1FF;
        uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
        uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

        uint64_t* pdp = get_next_level(kernel_pml4, pml4_idx, false, false);
        if (!pdp) return false;
        uint64_t* pd = get_next_level(pdp, pdp_idx, false, false);
        if (!pd) return false;
        uint64_t* pt = get_next_level(pd, pd_idx, false, false);
        if (!pt) return false;

        uint64_t entry = pt[pt_idx];
        return (entry & PTE_PRESENT) != 0;
    }

    void map_page(uint64_t physical_addr, uint64_t virtual_addr, uint64_t flags) {
        uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
        uint64_t pdp_idx  = (virtual_addr >> 30) & 0x1FF;
        uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
        uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

        bool user = (flags & PTE_USER) != 0;

        uint64_t* pdp = get_next_level(kernel_pml4, pml4_idx, true, user);
        if(!pdp) return;
        
        uint64_t* pd = get_next_level(pdp, pdp_idx, true, user);
        if(!pd) return;
        
        uint64_t* pt = get_next_level(pd, pd_idx, true, user);
        if(!pt) return;

        // Apply flags to the final page
        uint64_t phys_frame = physical_addr & 0x000FFFFFFFFFF000;
        pt[pt_idx] = phys_frame | (flags & 0xFFF);
        
        // Flush TLB for this specific address immediately
        asm volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
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
                    map_page(entry->base + j, virt_start + j, PTE_PRESENT | PTE_WRITABLE);
                }
            }
        }

        // 2. Map HHDM (Physical Memory)
        for (uint64_t i = 0; i < map->entry_count; i++) {
            struct limine_memmap_entry* entry = map->entries[i];
            uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
            if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) flags |= PTE_NO_CACHE;

            for (uint64_t j = 0; j < entry->length; j += 4096) {
                map_page(entry->base + j, entry->base + j + hhdm_offset, flags);
            }
        }

        switch_pml4(pml4_phys);
        kprint("[VMM] Initialized.\n");
    }

    void unmap_page(uint64_t virtual_addr) { (void)virtual_addr; }
    void switch_pml4(uint64_t pml4_phys_addr) {
        asm volatile("mov %0, %%cr3" : : "r"(pml4_phys_addr) : "memory");
    }
    uint64_t get_kernel_pml4_phys() {
        return (uint64_t)kernel_pml4 - hhdm_offset;
    }
}