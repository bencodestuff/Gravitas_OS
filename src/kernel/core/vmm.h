#pragma once
#include <stdint.h>
#include <limine.h> // Need this for the struct definition

namespace VMM {
    // UPDATED: Now requires the memory map to know what to map!
    void init(struct limine_memmap_response* map);

    // Map into the CURRENT address space
    void map_page(uint64_t physical_addr, uint64_t virtual_addr, uint64_t flags);
    // Map into an explicit address space (used for building new processes)
    void map_page_in(uint64_t pml4_phys, uint64_t physical_addr,
                     uint64_t virtual_addr, uint64_t flags);

    // Drop 'pages' mappings from an explicit address space: the PTEs are
    // cleared (frames are NOT freed — that's the caller's), so a range can
    // be handed back to alloc_user_range. TLB entries are flushed only when
    // the space is the one currently active; other spaces have no cached
    // translations to invalidate.
    void unmap_user_range(uint64_t pml4_phys, uint64_t virtual_addr, uint64_t pages);

    void switch_pml4(uint64_t pml4_phys_addr);
    uint64_t current_pml4();          // read CR3
    uint64_t get_kernel_pml4_phys();
    bool is_mapped(uint64_t virtual_addr);

    // Create a fresh address space sharing the kernel's upper-half mappings.
    // Returns the physical address of the new PML4.
    uint64_t clone_kernel_space();

    // Find and reserve a free virtual range in 'pml4' (user region:
    // USER_VA_BASE .. USER_VA_END). Returns the base VA, or 0 on failure.
    // Used for stacks, ELF loads, shared buffers — anything that needs a
    // guaranteed-unique mapping in that address space.
    uint64_t alloc_user_range(uint64_t pml4_phys, uint64_t pages);

    // Mark a VA range as reserved WITHOUT mapping pages (e.g. the brk
    // arena start). Subsequent alloc_user_range calls will skip it.
    bool reserve_user_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t pages);

    // Free all user-half frames and tables of an address space (the kernel
    // upper half is shared and left alone). Used at process exit.
    void destroy_user_space(uint64_t pml4_phys);
}