#include "elf.h"
#include "../kernel/pmm.h" // We need to alloc RAM
#include "../kernel/vmm.h" // We need to map RAM
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

    uint64_t load_elf(uint8_t* file_data) {
        kprint("[ELF] Loading...\n");
        Elf64_Ehdr* header = (Elf64_Ehdr*)file_data;

        // Verify Magic (Safe check)
        if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E') return 0;

        uint64_t hhdm_offset = hhdm_request.response->offset;
        Elf64_Phdr* ph_table = (Elf64_Phdr*)(file_data + header->e_phoff);

        for (int i = 0; i < header->e_phnum; i++) {
            Elf64_Phdr* ph = &ph_table[i];

            if (ph->p_type == PT_LOAD) {
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
                    // We need to figure out which part of the file goes into this specific page.
                    // This logic handles partial pages (offsets) correctly.
                    
                    uint64_t page_offset = curr - start_page; // How far into the segment are we?
                    uint64_t file_offset = ph->p_offset + page_offset;
                    
                    // How much to copy?
                    // If file_size is smaller than memory_size (BSS), we stop copying early.
                    if (page_offset < ph->p_filesz) {
                        uint64_t copy_len = 4096;
                        if (page_offset + copy_len > ph->p_filesz) {
                            copy_len = ph->p_filesz - page_offset;
                        }

                        // Adjust for the very first page if vaddr isn't 4096 aligned
                        // (Linkers often start at 0x400000 exactly, so this is usually 0)
                        uint64_t page_skew = (curr == start_page) ? (ph->p_vaddr & 0xFFF) : 0;
                        
                        memcpy(dest_ptr + page_skew, file_data + file_offset, copy_len);
                    }
                }
            }
        }
        
        return header->e_entry; // Return the start address (e.g., 0x400000)
    }
}