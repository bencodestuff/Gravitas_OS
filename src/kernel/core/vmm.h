#pragma once
#include <stdint.h>
#include <limine.h> // Need this for the struct definition

namespace VMM {
    // UPDATED: Now requires the memory map to know what to map!
    void init(struct limine_memmap_response* map);

    // The user/kernel split: every user virtual address is below this, and the
    // kernel's upper half (HHDM, image, page tables) starts at it. The upper
    // half is shared into every address space, so a syscall must never treat
    // an address at or above this as a user buffer.
    constexpr uint64_t USER_VA_LIMIT = 0x0000800000000000ull;

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

    // The leaf PTE for 'virtual_addr' in an explicit address space, exactly as
    // stored (present, guard, or otherwise), or 0 when the tables do not reach
    // a leaf. Walks the same tables as is_mapped() and allocates nothing.
    // Callers that must know whether the mapping belongs to a user (PTE_USER)
    // and whether it is writable use this; check PTE_PRESENT themselves.
    uint64_t leaf_pte_in(uint64_t pml4_phys, uint64_t virtual_addr);

    // Leaf flags for map_page_in: the low 12 bits are stored in the PTE
    // verbatim, so anything here survives.
    constexpr uint64_t PTE_PRESENT  = 1;
    constexpr uint64_t PTE_WRITABLE = 2;
    constexpr uint64_t PTE_USER     = 4;
    constexpr uint64_t PTE_NO_CACHE = 0x10;
    // Bit 9 (free for the OS): the leaf maps memory the task does NOT own — a
    // file in the immutable image, or a shared region. The address-space
    // teardown clears the mapping but leaves the frames alone; whoever owns
    // them frees them.
    constexpr uint64_t PTE_SHARED = 1ull << 9;

    // The physical address behind a kernel (HHDM) pointer — how the tar image's
    // files become mappable: the bytes are already in memory, so a read-only
    // mapping is zero-copy.
    uint64_t phys_of(const void* va);

    // The kernel's own pointer to a physical address (HHDM + phys): how the
    // kernel touches frames it allocated (a shared region has to be zeroed).
    void* phys_to_kernel(uint64_t phys);

    // Create a fresh address space sharing the kernel's upper-half mappings.
    // Returns the physical address of the new PML4.
    uint64_t clone_kernel_space();

    // Find and reserve a free virtual range in 'pml4' (user region:
    // USER_VA_BASE .. USER_VA_END). Returns the base VA, or 0 on failure.
    // Used for stacks, ELF loads, shared buffers — anything that needs a
    // guaranteed-unique mapping in that address space.
    uint64_t alloc_user_range(uint64_t pml4_phys, uint64_t pages);

    // Free all user-half frames and tables of an address space (the kernel
    // upper half is shared and left alone). Used at process exit.
    void destroy_user_space(uint64_t pml4_phys);
}