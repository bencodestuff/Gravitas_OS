#include "vmm.h"
#include "pmm.h"
#include <limine.h>

extern "C" void kprint(const char* str);

// --- EXTERNAL REQUESTS ---
// We link to the ones defined in main.cpp
extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_kernel_address_request kernel_address_request;

namespace VMM {
    // --- Flags ---
    constexpr uint64_t PTE_PRESENT = 1;
    constexpr uint64_t PTE_WRITABLE = 2;
    constexpr uint64_t PTE_USER = 4;
    constexpr uint64_t PTE_NO_CACHE = 0x10;
    // NX (No Execute) bit is bit 63, but we'll stick to basic flags for Level 2.

    static uint64_t* kernel_pml4 = nullptr;
    static uint64_t hhdm_offset = 0;

    // --- Helpers ---
    static void memset(void* ptr, int value, uint64_t size) {
        uint8_t* p = (uint8_t*)ptr;
        for (uint64_t i = 0; i < size; i++) p[i] = value;
    }

    // Helper to print hex (Debugging)
    static void vmm_print_hex(uint64_t value) {
        char buffer[20];
        int pos = 0; buffer[pos++]='0'; buffer[pos++]='x';
        if(value==0) buffer[pos++]='0';
        else {
            for(int i=60;i>=0;i-=4) {
                uint8_t n=(value>>i)&0xF;
                if(n==0 && pos==2) continue;
                buffer[pos++]=(n<10)?(n+'0'):(n-10+'A');
            }
        }
        buffer[pos++]='\n'; buffer[pos]='\0';
        kprint(buffer);
    }

    static uint64_t* get_next_level(uint64_t* current_level, uint64_t index, bool allocate) {
        uint64_t entry = current_level[index];
        if (entry & PTE_PRESENT) {
            uint64_t phys = entry & 0x000FFFFFFFFFF000;
            return (uint64_t*)(phys + hhdm_offset);
        }
        if (!allocate) return nullptr;

        uint64_t new_table_phys = PMM::alloc_page();
        if (new_table_phys == 0) return nullptr;

        uint64_t* new_table_virt = (uint64_t*)(new_table_phys + hhdm_offset);
        memset(new_table_virt, 0, 4096);
        
        // Map as Present | Writable | User (Permissions handled at last level)
        current_level[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        return new_table_virt;
    }

    void map_page(uint64_t physical_addr, uint64_t virtual_addr, uint64_t flags) {
        uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
        uint64_t pdp_idx  = (virtual_addr >> 30) & 0x1FF;
        uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
        uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

        uint64_t* pdp = get_next_level(kernel_pml4, pml4_idx, true);
        if(!pdp) return;
        uint64_t* pd = get_next_level(pdp, pdp_idx, true);
        if(!pd) return;
        uint64_t* pt = get_next_level(pd, pd_idx, true);
        if(!pt) return;

        pt[pt_idx] = physical_addr | flags;
    }

    // --- MAIN INIT ---
    void init(struct limine_memmap_response* map) {
        kprint("[VMM] Initializing...\n");

        if (hhdm_request.response == nullptr || kernel_address_request.response == nullptr) {
             kprint("CRITICAL: Requests missing!\n");
             for(;;) asm("hlt");
        }
        hhdm_offset = hhdm_request.response->offset;

        // 1. Allocate Kernel PML4
        uint64_t pml4_phys = PMM::alloc_page();
        kernel_pml4 = (uint64_t*)(pml4_phys + hhdm_offset);
        memset(kernel_pml4, 0, 4096);
        kprint("[VMM] PML4 created.\n");

        // 2. Map the Kernel Itself
        // We look for regions marked as KERNEL_AND_MODULES
        uint64_t kernel_phys_base = kernel_address_request.response->physical_base;
        uint64_t kernel_virt_base = kernel_address_request.response->virtual_base;
        
        kprint("[VMM] Mapping Kernel Segments...\n");
        for (uint64_t i = 0; i < map->entry_count; i++) {
            struct limine_memmap_entry* entry = map->entries[i];

            if (entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
                // Calculate the specific virtual address for this chunk
                // Virt = BaseVirt + (ChunkPhys - BasePhys)
                uint64_t offset_from_base = entry->base - kernel_phys_base;
                uint64_t virt_start = kernel_virt_base + offset_from_base;

                for (uint64_t j = 0; j < entry->length; j += 4096) {
                    // Flags: Present | Writable (We'll refine execute later)
                    map_page(entry->base + j, virt_start + j, PTE_PRESENT | PTE_WRITABLE);
                }
            }
        }

        // 3. Map All Physical Memory (HHDM)
        kprint("[VMM] Mapping Physical Memory (HHDM)...\n");

        for (uint64_t i = 0; i < map->entry_count; i++) {
            struct limine_memmap_entry* entry = map->entries[i];
            
            // Default flags: Present and Writable
            uint64_t flags = PTE_PRESENT | PTE_WRITABLE;

            // If this region is the Framebuffer, DISABLE CACHING!
            if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
                flags |= PTE_NO_CACHE;
                kprint("[VMM] Framebuffer found and uncached.\n");
            }

            // Map the region
            for (uint64_t j = 0; j < entry->length; j += 4096) {
                uint64_t phys = entry->base + j;
                uint64_t virt = phys + hhdm_offset;
                
                map_page(phys, virt, flags);
            }
        }

        kprint("[VMM] Identity Mapping Complete.\n");

        // 4. THE SWITCH
        kprint("[VMM] Switching to new Page Table...\n");
        switch_pml4(pml4_phys);
        kprint("[VMM] SUCCESS: We are running on custom page tables!\n");
    }

    void unmap_page(uint64_t virtual_addr) { (void)virtual_addr; }
    void switch_pml4(uint64_t pml4_phys_addr) {
        asm volatile("mov %0, %%cr3" : : "r"(pml4_phys_addr) : "memory");
    }
    uint64_t get_kernel_pml4_phys() {
        return (uint64_t)kernel_pml4 - hhdm_offset;
    }
}