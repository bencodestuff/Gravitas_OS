#include "fs/elf.h"
#include "kernel/core/pmm.h" // We need to alloc RAM
#include "kernel/core/vmm.h" // We need to map RAM
#include <limine.h>        // For HHDM request

extern "C" void kprint(const char* str);
extern "C" void kprint_hex(uint64_t n);

// We need the HHDM offset to write to physical RAM safely
extern volatile struct limine_hhdm_request hhdm_request;

namespace ELF {

    // Helper: Simple memcpy
    void memcpy(void* dest, const void* src, uint64_t n) {
        uint8_t* d = (uint8_t*)dest;
        const uint8_t* s = (const uint8_t*)src;
        for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    }

    // Helper: Simple memset (for BSS/Zeroing)
    void memset(void* dest, int val, uint64_t n) {
        uint8_t* d = (uint8_t*)dest;
        for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    }

    // ELF identification bytes (e_ident) and the values we accept.
    constexpr uint8_t EI_CLASS    = 4;
    constexpr uint8_t EI_DATA     = 5;
    constexpr uint8_t ELFCLASS64  = 2;
    constexpr uint8_t ELFDATA2LSB = 1;
    constexpr uint16_t ET_EXEC    = 2; // static executable at a fixed address

    // The loader maps every PT_LOAD at its own p_vaddr with the user bit set.
    // The kernel's mappings live in the upper half of the PML4 (entries
    // 256..511), which clone_kernel_space() shares into every process; a
    // segment there would walk — and OR PTE_USER into — those shared tables,
    // handing ring 3 a window into kernel memory. Reject anything at or above
    // the start of that half.
    constexpr uint64_t USER_VA_LIMIT = 0x0000800000000000ull;

    uint64_t load_elf(uint8_t* file_data, uint64_t file_size) {
        kprint("[ELF] Loading...\n");

        // The header itself must fit before it can be read.
        if (file_data == nullptr || file_size < sizeof(Elf64_Ehdr)) return 0;
        Elf64_Ehdr* header = (Elf64_Ehdr*)file_data;

        // Identify the file: magic, 64-bit, little-endian, and a type we can
        // actually run. ET_DYN/PIE would need relocations this loader does
        // not do, so it is rejected rather than misloaded.
        if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E' ||
            header->e_ident[2] != 'L' || header->e_ident[3] != 'F') return 0;
        if (header->e_ident[EI_CLASS] != ELFCLASS64) return 0;
        if (header->e_ident[EI_DATA]  != ELFDATA2LSB) return 0;
        if (header->e_type != ET_EXEC) return 0;

        // The program header table must lie entirely inside the file, with
        // entries the size the indexing below assumes.
        if (header->e_phentsize != sizeof(Elf64_Phdr)) return 0;
        if (header->e_phoff > file_size) return 0;
        if (header->e_phnum > (file_size - header->e_phoff) / header->e_phentsize) return 0;

        uint64_t hhdm_offset = hhdm_request.response->offset;
        Elf64_Phdr* ph_table = (Elf64_Phdr*)(file_data + header->e_phoff);

        bool loaded = false;
        for (int i = 0; i < header->e_phnum; i++) {
            Elf64_Phdr* ph = &ph_table[i];

            if (ph->p_type != PT_LOAD) continue;

            // The segment's file bytes must exist, its memory image must be
            // at least as large as them, and every byte it touches must land
            // in the user half without the arithmetic wrapping.
            if (ph->p_filesz > ph->p_memsz) return 0;
            if (ph->p_offset > file_size || ph->p_filesz > file_size - ph->p_offset) return 0;
            if (ph->p_memsz == 0) continue;
            if (ph->p_vaddr >= USER_VA_LIMIT ||
                ph->p_memsz > USER_VA_LIMIT - ph->p_vaddr) return 0;

            loaded = true;
            {
                // 1. Calculate the range of pages we need
                uint64_t start_addr = ph->p_vaddr;
                uint64_t end_addr   = ph->p_vaddr + ph->p_memsz;
                
                // Align start down to page boundary (0x1000)
                uint64_t start_page = start_addr & ~0xFFF;
                // Align end up to page boundary
                uint64_t end_page   = (end_addr + 0xFFF) & ~0xFFF;

                // 2. Loop through every page in this segment
                for (uint64_t curr = start_page; curr < end_page; curr += 4096) {
                    
                    // A. Allocate Physical RAM
                    uint64_t phys = PMM::alloc_page(); 
                    if (phys == 0) { kprint("[ELF] OOM!\n"); return 0; }

                    // B. Map it to User Mode (0x07 = User | RW | Present)
                    // We map it RWX (Read/Write/Execute) for simplicity
                    VMM::map_page(phys, curr, 0x07);

                    // C. Calculate the "Backdoor" address to write to
                    // (Physical Address + HHDM Offset)
                    uint8_t* dest_ptr = (uint8_t*)(phys + hhdm_offset);

                    // D. Clear the page first (Zero it out)
                    memset(dest_ptr, 0, 4096);

                    // E. Copy Data from File -> RAM
                    // A page's file bytes begin (page start - p_vaddr) bytes
                    // into the segment. The first page need not be aligned:
                    // it starts `skew` bytes in, so only 4096 - skew of its
                    // bytes come from the file and the rest of the file data
                    // shifts to the following pages.
                    uint64_t seg_off = (curr > ph->p_vaddr) ? (curr - ph->p_vaddr) : 0;
                    if (seg_off < ph->p_filesz) {
                        uint64_t dest_off = (curr > ph->p_vaddr) ? 0 : (ph->p_vaddr - curr);
                        uint64_t copy_len = 4096 - dest_off;
                        if (seg_off + copy_len > ph->p_filesz) {
                            copy_len = ph->p_filesz - seg_off;
                        }

                        memcpy(dest_ptr + dest_off, file_data + ph->p_offset + seg_off, copy_len);
                    }
                }
            }
        }

        // Nothing loadable means no image; do not hand back an entry point
        // for it. e_entry must also name an address in the same window the
        // segments were allowed into — a kernel-half entry is the same bug
        // one step later.
        if (!loaded) return 0;
        if (header->e_entry == 0 || header->e_entry >= USER_VA_LIMIT) return 0;

        return header->e_entry; // Return the start address (e.g., 0x400000)
    }
}